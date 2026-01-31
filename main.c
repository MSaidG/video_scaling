#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h" // Requires stb_image.h in the same folder

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct {
  int fd;
  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;
  struct gbm_device *gbm_dev;
  struct gbm_surface *gbm_surf;
  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;
  uint32_t current_fb_id;
  struct gbm_bo *current_bo;
} kms;

int waiting_for_flip = 0;
volatile sig_atomic_t running = 1;

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void *data) {
  *(int *)data = 0;
}

uint32_t get_fb_for_bo(struct gbm_bo *bo) {
  uint32_t fb_id;
  uint32_t handle = gbm_bo_get_handle(bo).u32;
  uint32_t stride = gbm_bo_get_stride(bo);
  uint32_t width = gbm_bo_get_width(bo);
  uint32_t height = gbm_bo_get_height(bo);
  drmModeAddFB(kms.fd, width, height, 24, 32, stride, handle, &fb_id);
  return fb_id;
}

void swap_buffers_kms() {
  eglSwapBuffers(kms.egl_disp, kms.egl_surf);
  struct gbm_bo *next_bo = gbm_surface_lock_front_buffer(kms.gbm_surf);
  uint32_t next_fb_id = get_fb_for_bo(next_bo);
  drmModePageFlip(kms.fd, kms.crtc->crtc_id, next_fb_id,
                  DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
  waiting_for_flip = 1;
  if (kms.current_bo)
    gbm_surface_release_buffer(kms.gbm_surf, kms.current_bo);
  kms.current_bo = next_bo;
}

int init_kms() {
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    return -1;

  drmModeRes *res = drmModeGetResources(kms.fd);
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *conn = drmModeGetConnector(kms.fd, res->connectors[i]);
    if (conn->connection == DRM_MODE_CONNECTED) {
      kms.connector = conn;
      break;
    }
    drmModeFreeConnector(conn);
  }
  if (!kms.connector)
    return -1;
  kms.mode = kms.connector->modes[0];

  drmModeEncoder *enc = drmModeGetEncoder(kms.fd, kms.connector->encoders[0]);
  kms.crtc = drmModeGetCrtc(kms.fd, enc ? enc->crtc_id : res->crtcs[0]);
  if (enc)
    drmModeFreeEncoder(enc);

  kms.gbm_dev = gbm_create_device(kms.fd);
  kms.gbm_surf = gbm_surface_create(kms.gbm_dev, kms.mode.hdisplay,
                                    kms.mode.vdisplay, GBM_FORMAT_ARGB8888,
                                    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

  kms.egl_disp =
      eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, kms.gbm_dev, NULL);
  eglInitialize(kms.egl_disp, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);

  EGLConfig config;
  EGLint n;
  EGLint attribs[] = {EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                      EGL_ALPHA_SIZE, 8, EGL_NONE};
  eglChooseConfig(kms.egl_disp, attribs, &config, 1, &n);

  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT,
                       (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
  kms.egl_surf = eglCreateWindowSurface(
      kms.egl_disp, config, (EGLNativeWindowType)kms.gbm_surf, NULL);
  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);

  return 0;
}

// --- SHADERS ---

const char *vs_src =
    "attribute vec4 a_pos;    \n"
    "attribute vec2 a_tex;    \n"
    "varying vec2 v_tex;      \n"
    "void main() {            \n"
    "   gl_Position = a_pos;  \n" // * vec4(0.5, 0.5, 0.5, 1.0)
    "   v_tex = a_tex;        \n" // * vec2(2.0, 2.0) * vec2(0.5, 0.5)
    "}                        \n";

const char *fs_src =
    "precision mediump float; \n"
    "varying vec2 v_tex;      \n"
    "uniform sampler2D u_tex; \n"
    "void main() {            \n"
    "   gl_FragColor = texture2D(u_tex, v_tex); \n" // * vec4(1.0, 0.0,
                                                    // 0.0, 1.0)
    "}                        \n";

GLuint create_shader(const char *src, GLenum type) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  // (Skipping error check for brevity, but you should add it in production)
  return s;
}


GLuint load_png_texture(const char *filename) {
  int w, h, ch;
  unsigned char *data =
      stbi_load(filename, &w, &h, &ch, 4); // Force 4 channels (RGBA)
  if (!data) {
    fprintf(stderr, "Failed to load image: %s\n", filename);
    return 0;
  }

  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);

  // Upload to GPU
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               data);

  // Set filtering (REQUIRED or texture wont show)
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  stbi_image_free(data); // Free CPU RAM
  return tex;
}

void handle_sigint(int sig) { running = 0; }


int main() {
  signal(SIGINT, handle_sigint);

  if (init_kms() != 0) {
    fprintf(stderr, "KMS Init Failed\n");
    return -1;
  }

  // 1. Setup Shaders
  GLuint prog = glCreateProgram();
  glAttachShader(prog, create_shader(vs_src, GL_VERTEX_SHADER));
  glAttachShader(prog, create_shader(fs_src, GL_FRAGMENT_SHADER));
  glLinkProgram(prog);
  glUseProgram(prog);

  // 2. Setup Texture
  GLuint tex_id = load_png_texture("videos/test.png");
  if (tex_id == 0)
    return -1;

  // 3. Setup Geometry (Full screen quad)
  // x, y, u, v
  GLfloat vertices[] = {
      -1.0f, 1.0f,  0.0f, 0.0f, // Top Left
      -1.0f, -1.0f, 0.0f, 1.0f, // Bottom Left
      1.0f,  1.0f,  1.0f, 0.0f, // Top Right
      1.0f,  -1.0f, 1.0f, 1.0f  // Bottom Right
  };

  GLint loc_pos = glGetAttribLocation(prog, "a_pos");
  GLint loc_tex = glGetAttribLocation(prog, "a_tex");

  glEnableVertexAttribArray(loc_pos);
  glEnableVertexAttribArray(loc_tex);

  // Stride is 4 floats (x,y,u,v). Pos is offset 0. Tex is offset 2.
  glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        vertices);
  glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        vertices + 2);

  // Enable Alpha Blending (for PNG transparency)
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // --- RENDER LOOP ---
  drmEventContext ev = {0};
  ev.version = 2;
  ev.page_flip_handler = page_flip_handler;
  fd_set fds;

  printf("Rendering PNG... Ctrl+C to exit.\n");

  while (running) {
    // --- CORRECTED WAIT LOGIC ---
    while (waiting_for_flip) {
      // 1. If Ctrl+C was pressed, stop waiting immediately
      if (!running)
        break;

      FD_ZERO(&fds);
      FD_SET(kms.fd, &fds);

      // 2. Wait for VSync (blocking)
      int ret = select(kms.fd + 1, &fds, NULL, NULL, NULL);

      if (ret > 0) {
        // Success: VSync event arrived, handle it (clears waiting_for_flip)
        drmHandleEvent(kms.fd, &ev);
      } else if (ret < 0) {
        // Error: If interrupted by Ctrl+C (EINTR), break the loop
        if (errno == EINTR)
          break;

        // Real error? Print it and bail
        perror("select error");
        break;
      }
    }

    // 3. Final check: If we stopped waiting because of Ctrl+C, exit the main
    // loop
    if (!running)
      break;

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f); // Dark gray background
    glClear(GL_COLOR_BUFFER_BIT);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    swap_buffers_kms();
  }

  return 0;
}
