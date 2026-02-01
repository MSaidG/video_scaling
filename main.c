#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h" // Requires stb_image.h in the same folder

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

struct {
  int fd;
  uint32_t current_fb_id;

  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;

  struct gbm_device *gbm_dev;
  struct gbm_surface *gbm_surf;
  struct gbm_bo *current_bo;

  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;

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
  // 1. Flush GL commands
  eglSwapBuffers(kms.egl_disp, kms.egl_surf);

  // 2. Lock the new buffer
  struct gbm_bo *next_bo = gbm_surface_lock_front_buffer(kms.gbm_surf);

  // --- SAFETY CHECK ---
  if (!next_bo) {
    fprintf(stderr, "Error: Failed to lock front buffer! skipping flip.\n");
    return;
  }
  // --------------------

  uint32_t next_fb_id = get_fb_for_bo(next_bo);

  // 3. Queue the page flip
  // Check kms.crtc just in case (though init_kms should catch it now)
  if (kms.crtc) {
    int ret = drmModePageFlip(kms.fd, kms.crtc->crtc_id, next_fb_id,
                              DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
    if (ret == 0) {
      waiting_for_flip = 1;
    } else {
      fprintf(stderr, "Flip failed: %d\n", ret);
    }
  }

  // 4. Cleanup old buffer
  if (kms.current_bo) {
    gbm_surface_release_buffer(kms.gbm_surf, kms.current_bo);
    drmModeRmFB(kms.fd, kms.current_fb_id);
  }

  kms.current_bo = next_bo;
  kms.current_fb_id = next_fb_id;
}

void cleanup(GLuint program, GLuint texture) {
  printf("Cleaning up resources...\n");

  // 1. OpenGL Cleanup (Best to do while context is still alive)
  if (program)
    glDeleteProgram(program);
  if (texture)
    glDeleteTextures(1, &texture);

  // 2. RELEASE THE GBM BUFFER FIRST (Fixes Valgrind Use-After-Free)
  // We must release the specific buffer before we destroy the surface that
  // manages it.
  if (kms.current_bo) {
    gbm_surface_release_buffer(kms.gbm_surf, kms.current_bo);
    // We also need to remove the Framebuffer from DRM
    drmModeRmFB(kms.fd, kms.current_fb_id);
    kms.current_bo = NULL;
  }

  // 3. EGL Cleanup
  if (kms.egl_disp != EGL_NO_DISPLAY) {
    eglMakeCurrent(kms.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);

    // Now it is safe to destroy the surface
    if (kms.egl_surf != EGL_NO_SURFACE)
      eglDestroySurface(kms.egl_disp, kms.egl_surf);
    if (kms.egl_ctx != EGL_NO_CONTEXT)
      eglDestroyContext(kms.egl_disp, kms.egl_ctx);
    eglTerminate(kms.egl_disp);
  }

  // 4. GBM Device Cleanup
  if (kms.gbm_surf)
    gbm_surface_destroy(kms.gbm_surf);
  if (kms.gbm_dev)
    gbm_device_destroy(kms.gbm_dev);

  // 5. DRM/KMS Cleanup
  if (kms.connector)
    drmModeFreeConnector(kms.connector);

  // Check if CRTC was allocated before accessing it (Safety check)
  if (kms.crtc)
    drmModeFreeCrtc(kms.crtc);

  if (kms.fd >= 0)
    close(kms.fd);

  printf("Cleanup Done.\n");
}

int init_kms() {
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0) {
    perror("Cannot open DRM device");
    return -1;
  }

  drmModeRes *res = drmModeGetResources(kms.fd);
  if (!res) {
    perror("Cannot get DRM resources");
    return -1;
  }

  // Find Connector
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *conn = drmModeGetConnector(kms.fd, res->connectors[i]);
    if (conn->connection == DRM_MODE_CONNECTED) {
      kms.connector = conn;
      break;
    }
    drmModeFreeConnector(conn);
  }
  if (!kms.connector) {
    fprintf(stderr, "Error: No monitor connected\n");
    return -1;
  }

  kms.mode = kms.connector->modes[0];

  // Find CRTC
  drmModeEncoder *enc = NULL;
  if (kms.connector->encoder_id) {
    enc = drmModeGetEncoder(kms.fd, kms.connector->encoder_id);
  }

  if (enc && enc->crtc_id) {
    kms.crtc = drmModeGetCrtc(kms.fd, enc->crtc_id);
  } else {
    // Fallback: Pick the first available CRTC if encoder isn't set
    if (res->count_crtcs > 0) {
      kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[0]);
    }
  }

  if (enc)
    drmModeFreeEncoder(enc);

  // --- CRITICAL FIX: Check if CRTC was actually found ---
  if (!kms.crtc) {
    fprintf(stderr,
            "Error: Could not find a valid CRTC (Display Controller)\n");
    return -1;
  }

  // Setup GBM
  kms.gbm_dev = gbm_create_device(kms.fd);
  if (!kms.gbm_dev) {
    fprintf(stderr, "Error: Failed to create GBM device\n");
    return -1;
  }

  kms.gbm_surf = gbm_surface_create(kms.gbm_dev, kms.mode.hdisplay,
                                    kms.mode.vdisplay, GBM_FORMAT_ARGB8888,
                                    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!kms.gbm_surf) {
    fprintf(stderr, "Error: Failed to create GBM surface\n");
    return -1;
  }

  // Setup EGL
  kms.egl_disp =
      eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, kms.gbm_dev, NULL);
  if (!eglInitialize(kms.egl_disp, NULL, NULL)) {
    fprintf(stderr, "Error: EGL Init failed\n");
    return -1;
  }

  eglBindAPI(EGL_OPENGL_ES_API);

  EGLConfig config;
  EGLint n;
  EGLint attribs[] = {EGL_RED_SIZE,   8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                      EGL_ALPHA_SIZE, 8, EGL_NONE};
  if (!eglChooseConfig(kms.egl_disp, attribs, &config, 1, &n) || n != 1) {
    fprintf(stderr, "Error: No suitable EGL config found\n");
    return -1;
  }

  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT,
                       (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
  kms.egl_surf = eglCreateWindowSurface(
      kms.egl_disp, config, (EGLNativeWindowType)kms.gbm_surf, NULL);

  if (kms.egl_surf == EGL_NO_SURFACE) {
    fprintf(stderr, "Error: Failed to create EGL Surface\n");
    return -1;
  }

  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);

  // Free resources pointer
  drmModeFreeResources(res);

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
  GLuint vs = create_shader(vs_src, GL_VERTEX_SHADER);
  GLuint fs = create_shader(fs_src, GL_FRAGMENT_SHADER);

  glAttachShader(prog, vs);
  glAttachShader(prog, fs);

  glLinkProgram(prog);

  glDeleteShader(vs);
  glDeleteShader(fs);

  GLuint tex_id = load_png_texture("videos/test.png");
  if (tex_id == 0) {
    cleanup(prog, 0); // Cleanup if texture load fails
    return -1;
  }
  glUseProgram(prog);

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

  cleanup(prog, tex_id);
  return 0;
}
