#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h> // For DRM_FORMAT_ARGB8888
#include <xf86drm.h>
#include <xf86drmMode.h>

// --- EXTENSION DEFINITIONS ---
// We need to load these function pointers manually
typedef EGLImageKHR(EGLAPIENTRYP PFNEGLCREATEIMAGEKHRPROC)(
    EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
    const EGLint *attrib_list);
typedef EGLBoolean(EGLAPIENTRYP PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy,
                                                           EGLImageKHR image);
typedef void(GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(
    GLenum target, GLeglImageOES image);

PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

#define VID_W 1920
#define VID_H 1080
#define RAW_FILE "nv12_1080p60.yuv"

// --- YUV TO RGB SHADER (Same as before) ---
const char *vs_src = "attribute vec4 a_pos;    \n"
                     "attribute vec2 a_tex;    \n"
                     "varying vec2 v_tex;      \n"
                     "void main() {            \n"
                     "   gl_Position = a_pos;  \n"
                     "   v_tex = a_tex;        \n"
                     "}                        \n";

const char *fs_src = "precision mediump float;             \n"
                     "varying vec2 v_tex;                  \n"
                     "uniform sampler2D tex_y;             \n"
                     "uniform sampler2D tex_uv;            \n"
                     "void main() {                        \n"
                     "  float y = texture2D(tex_y, v_tex).r;          \n"
                     "  vec4 uv_raw = texture2D(tex_uv, v_tex);       \n"
                     "  float u = uv_raw.r - 0.5;                     \n"
                     "  float v = uv_raw.a - 0.5;                     \n"
                     "  float r = y + 1.402 * v;                      \n"
                     "  float g = y - 0.344 * u - 0.714 * v;          \n"
                     "  float b = y + 1.772 * u;                      \n"
                     "  gl_FragColor = vec4(r, g, b, 1.0);            \n"
                     "}                                    \n";

GLuint create_shader(const char *src, GLenum type) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, 512, NULL, log);
    fprintf(stderr, "Shader Compile Error: %s\n", log);
    return 0;
  }
  return s;
}

// --- RESOURCES ---
typedef struct {
  uint32_t handle;
  uint32_t stride;
  uint32_t size;
  uint32_t fb_id;
  int prime_fd; // File Descriptor for DMABUF export
  uint8_t *map;

  // Zero-Copy Resources
  EGLImageKHR egl_img;
  GLuint tex_id;
  GLuint fbo_id;
} DumbBuffer;

struct {
  int fd;
  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;
  uint32_t plane_primary_id;
  uint32_t plane_overlay_id;
  DumbBuffer bufs[2]; // Double Buffering
  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf; // Pbuffer (Dummy)

  // GPU Resources
  GLuint prog;
  GLuint vbo;

  // Video Input
  int cam_fd;
  size_t frame_size;
  unsigned char *frame_buffer;
  GLuint tex_y;
  GLuint tex_uv;
} kms;

volatile sig_atomic_t running = 1;

void handle_sigint(int sig) { running = 0; }

double get_time_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// --- LOAD EXTENSIONS ---
int load_egl_extensions() {
  eglCreateImageKHR =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  eglDestroyImageKHR =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  glEGLImageTargetTexture2DOES =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
          "glEGLImageTargetTexture2DOES");

  if (!eglCreateImageKHR || !glEGLImageTargetTexture2DOES) {
    fprintf(stderr,
            "Error: Failed to load EGL_EXT_image_dma_buf_import extensions!\n");
    return -1;
  }
  return 0;
}

// --- ZERO-COPY BUFFER CREATION ---
int create_dumb_buffer_fbo(DumbBuffer *buf) {
  // 1. Create Dumb Buffer (Same as before)
  struct drm_mode_create_dumb create_req = {0};
  create_req.width = kms.mode.hdisplay;
  create_req.height = kms.mode.vdisplay;
  create_req.bpp = 32;
  if (ioctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0)
    return -1;

  buf->handle = create_req.handle;
  buf->stride = create_req.pitch;
  buf->size = create_req.size;

  // 2. Add FB for Display Controller (Same as before)
  // Note: Using Depth 24, Bpp 32 usually implies ARGB/XRGB (Little Endian BGRA)
  int ret = drmModeAddFB(kms.fd, kms.mode.hdisplay, kms.mode.vdisplay, 24, 32,
                         buf->stride, buf->handle, &buf->fb_id);
  if (ret) {
    perror("drmModeAddFB failed");
    return -1;
  }

  // 3. EXPORT PRIME FD (The New Step!)
  // This gets a file descriptor we can give to EGL
  struct drm_prime_handle prime = {0};
  prime.handle = buf->handle;
  prime.flags = DRM_CLOEXEC | DRM_RDWR;
  if (ioctl(kms.fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
    perror("PRIME Export failed");
    return -1;
  }
  buf->prime_fd = prime.fd;

  // 4. Create EGL Image from DMABUF
  // Note: DRM_FORMAT_ARGB8888 corresponds to the standard 32-bit layout on
  // ZynqMP
  EGLint attribs[] = {EGL_WIDTH,
                      kms.mode.hdisplay,
                      EGL_HEIGHT,
                      kms.mode.vdisplay,
                      EGL_LINUX_DRM_FOURCC_EXT,
                      DRM_FORMAT_ARGB8888,
                      EGL_DMA_BUF_PLANE0_FD_EXT,
                      buf->prime_fd,
                      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                      0,
                      EGL_DMA_BUF_PLANE0_PITCH_EXT,
                      buf->stride,
                      EGL_NONE};

  buf->egl_img = eglCreateImageKHR(kms.egl_disp, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
  if (buf->egl_img == EGL_NO_IMAGE_KHR) {
    fprintf(stderr, "eglCreateImageKHR failed: 0x%x\n", eglGetError());
    return -1;
  }

  // 5. Create Texture and Bind EGL Image
  glGenTextures(1, &buf->tex_id);
  glBindTexture(GL_TEXTURE_2D, buf->tex_id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, buf->egl_img);

  // 6. Create FBO and Attach Texture
  glGenFramebuffers(1, &buf->fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         buf->tex_id, 0);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "FBO Incomplete!\n");
    return -1;
  }

  // Unbind
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return 0;
}

// --- VIDEO INPUT (MMAP) ---
int init_raw_input() {
  kms.cam_fd = open(RAW_FILE, O_RDONLY);
  if (kms.cam_fd < 0) {
    fprintf(stderr, "Error opening %s\n", RAW_FILE);
    return -1;
  }

  struct stat sb;
  fstat(kms.cam_fd, &sb);
  size_t file_size = sb.st_size;
  kms.frame_size = VID_W * VID_H * 3 / 2;

  kms.frame_buffer =
      mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, kms.cam_fd, 0);
  if (kms.frame_buffer == MAP_FAILED)
    return -1;

  printf("Mapped Raw File: %zu MB\n", file_size / 1024 / 1024);

  // Y Texture
  glGenTextures(1, &kms.tex_y);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, kms.tex_y);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  // UV Texture
  glGenTextures(1, &kms.tex_uv);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, kms.tex_uv);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  return 0;
}

void update_input_textures() {
  static int frame_idx = 0;
  unsigned char *frame_start = kms.frame_buffer + (frame_idx * kms.frame_size);
  unsigned char *uv_start = frame_start + (VID_W * VID_H);

  // Upload Y
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, kms.tex_y);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, VID_W, VID_H, 0, GL_LUMINANCE,
               GL_UNSIGNED_BYTE, frame_start);

  // Upload UV
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, kms.tex_uv);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, VID_W / 2, VID_H / 2, 0,
               GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv_start);

  frame_idx++;
  if (frame_idx >= 900)
    frame_idx = 0;
}

// --- INIT FUNCTIONS ---
int init_gles_objects() {
  kms.prog = glCreateProgram();
  GLuint vs = create_shader(vs_src, GL_VERTEX_SHADER);
  GLuint fs = create_shader(fs_src, GL_FRAGMENT_SHADER);
  if (!vs || !fs)
    return -1;

  glAttachShader(kms.prog, vs);
  glAttachShader(kms.prog, fs);
  glLinkProgram(kms.prog);
  glUseProgram(kms.prog);

  // VBO (Standard Orientation)
  GLfloat vertices[] = {
      // X      Y      U     V
      -1.0f, 1.0f,  0.0f, 1.0f, // Top Left     (V was 0.0)
      -1.0f, -1.0f, 0.0f, 0.0f, // Bottom Left  (V was 1.0)
      1.0f,  1.0f,  1.0f, 1.0f, // Top Right    (V was 0.0)
      1.0f,  -1.0f, 1.0f, 0.0f  // Bottom Right (V was 1.0)
  };

  glGenBuffers(1, &kms.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, kms.vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

  GLint loc_pos = glGetAttribLocation(kms.prog, "a_pos");
  GLint loc_tex = glGetAttribLocation(kms.prog, "a_tex");
  glEnableVertexAttribArray(loc_pos);
  glEnableVertexAttribArray(loc_tex);
  glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)0);
  glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)(2 * sizeof(float)));

  glUniform1i(glGetUniformLocation(kms.prog, "tex_y"), 0);
  glUniform1i(glGetUniformLocation(kms.prog, "tex_uv"), 1);

  return 0;
}

void cleanup() {
  // Add detailed cleanup if needed, prioritizing DRM/FD closure
  if (kms.fd >= 0)
    close(kms.fd);
}

int init_drm() {
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    return -1;
  // ... (Assuming DRM init code same as before to find connector/CRTC)
  // For brevity, using the same robust init as previous working version
  drmModeRes *res = drmModeGetResources(kms.fd);
  kms.connector =
      drmModeGetConnector(kms.fd, res->connectors[0]); // Simplified for example
  kms.mode = kms.connector->modes[0];
  kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[0]);
  drmModeFreeResources(res);

  kms.plane_primary_id = 39;
  kms.plane_overlay_id = 41;
  return 0;
}

int init_egl() {
  kms.egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(kms.egl_disp, NULL, NULL)) {
    kms.egl_disp = eglGetDisplay((EGLNativeDisplayType)kms.fd);
    eglInitialize(kms.egl_disp, NULL, NULL);
  }
  eglBindAPI(EGL_OPENGL_ES_API);

  EGLConfig config;
  EGLint num_configs;
  EGLint attribs[] = {EGL_SURFACE_TYPE,
                      EGL_PBUFFER_BIT,
                      EGL_RED_SIZE,
                      8,
                      EGL_GREEN_SIZE,
                      8,
                      EGL_BLUE_SIZE,
                      8,
                      EGL_ALPHA_SIZE,
                      8,
                      EGL_RENDERABLE_TYPE,
                      EGL_OPENGL_ES2_BIT,
                      EGL_NONE};
  eglChooseConfig(kms.egl_disp, attribs, &config, 1, &num_configs);

  // Dummy Pbuffer (We won't really use it for rendering, but EGL context needs
  // a surface sometimes)
  EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  kms.egl_surf = eglCreatePbufferSurface(kms.egl_disp, config, pbuffer_attribs);

  EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT, ctx_attribs);
  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);

  return load_egl_extensions();
}

int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);

  if (init_drm() < 0)
    return -1;
  if (init_egl() < 0)
    return -1;

  // Create FBO-backed Dumb Buffers
  if (create_dumb_buffer_fbo(&kms.bufs[0]) < 0)
    return -1;
  if (create_dumb_buffer_fbo(&kms.bufs[1]) < 0)
    return -1;

  if (init_raw_input() < 0) {
    cleanup();
    return -1;
  }
  if (init_gles_objects() < 0) {
    cleanup();
    return -1;
  }

  printf("Setup Complete. Zero-Copy Pipeline Active.\n");

  // Disable Console
  drmModeSetPlane(kms.fd, kms.plane_overlay_id, kms.crtc->crtc_id, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0);

  int back_buf_idx = 0;

  // FPS Stats
  double last_time = get_time_sec();
  int frames = 0;

  while (running) {
    // 1. Upload YUV Data (CPU -> GPU Texture)
    update_input_textures();

    // 2. Bind the FBO (Render directly to the Dumb Buffer)
    glBindFramebuffer(GL_FRAMEBUFFER, kms.bufs[back_buf_idx].fbo_id);

    // 3. Render
    glViewport(0, 0, kms.mode.hdisplay, kms.mode.vdisplay);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // 4. Finish (Ensure GPU is done writing before Display reads it)
    glFinish();

    // 5. Flip (Display Controller reads the new buffer)
    int ret = drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                              kms.bufs[back_buf_idx].fb_id, 0, 0, 0,
                              kms.mode.hdisplay, kms.mode.vdisplay, 0, 0,
                              kms.mode.hdisplay << 16, kms.mode.vdisplay << 16);

    if (ret)
      perror("Flip failed");

    back_buf_idx = (back_buf_idx + 1) % 2;

    // FPS
    frames++;
    double current_time = get_time_sec();
    if (current_time - last_time >= 1.0) {
      printf("FPS: %d\n", frames);
      frames = 0;
      last_time = current_time;
    }
  }

  cleanup();
  return 0;
}