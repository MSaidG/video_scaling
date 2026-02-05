#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// FIX: Define GL_BGRA_EXT manually if missing
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

// --- SHADERS (TEXTURE MODE) ---
const char *vs_src = "attribute vec4 a_pos;    \n"
                     "attribute vec2 a_tex;    \n"
                     "varying vec2 v_tex;      \n"
                     "void main() {            \n"
                     "   gl_Position = a_pos;  \n"
                     "   v_tex = a_tex;        \n"
                     "}                        \n";

const char *fs_src =
    "precision mediump float; \n"
    "varying vec2 v_tex;      \n"
    "uniform sampler2D u_tex; \n"
    "void main() {            \n"
    "   vec4 color = texture2D(u_tex, v_tex); \n"
    "   gl_FragColor = vec4(color.b, color.g, color.r, color.a); \n"
    "}                        \n";

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
  uint8_t *map;
} DumbBuffer;

struct {
  int fd;
  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;
  uint32_t plane_primary_id;
  uint32_t plane_overlay_id;
  DumbBuffer bufs[2];
  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;
  // GPU Resources
  GLuint prog;
  GLuint vbo;
  GLuint tex;
} kms;

volatile sig_atomic_t running = 1;

void handle_sigint(int sig) { running = 0; }

double get_time_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// --- TEXTURE LOADER ---
GLuint load_png_texture(const char *filename) {
  int w, h, ch;
  // Load image decoded into CPU memory
  unsigned char *data =
      stbi_load(filename, &w, &h, &ch, 4); // Force 4 channels (RGBA)
  if (!data) {
    fprintf(stderr, "Failed to load image: %s\n", filename);
    return 0;
  }

  GLuint tex;
  glGenTextures(1, &tex);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);

  // Upload CPU memory to GPU Memory
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               data);

  // Required filtering parameters
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  stbi_image_free(data);
  return tex;
}

int init_gles_objects() {
  // 1. Shaders
  kms.prog = glCreateProgram();
  GLuint vs = create_shader(vs_src, GL_VERTEX_SHADER);
  GLuint fs = create_shader(fs_src, GL_FRAGMENT_SHADER);

  if (!vs || !fs)
    return -1;

  glAttachShader(kms.prog, vs);
  glAttachShader(kms.prog, fs);
  glLinkProgram(kms.prog);
  glUseProgram(kms.prog);

  // 2. VBO Setup (x, y, u, v)
  // FIX: Swapped V coordinates to flip image vertically
  // Top row gets V=1.0, Bottom row gets V=0.0
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

  // 3. Attributes
  GLint loc_pos = glGetAttribLocation(kms.prog, "a_pos");
  GLint loc_tex = glGetAttribLocation(kms.prog, "a_tex");
  GLint loc_sampler = glGetUniformLocation(kms.prog, "u_tex");

  glEnableVertexAttribArray(loc_pos);
  glEnableVertexAttribArray(loc_tex);

  glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)0);
  glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)(2 * sizeof(float)));

  // Force Texture Unit 0
  glUniform1i(loc_sampler, 0);

  // Enable Blending for transparency
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  return 0;
}

void cleanup() {
  printf("\nCleaning up...\n");
  if (kms.prog)
    glDeleteProgram(kms.prog);
  if (kms.tex)
    glDeleteTextures(1, &kms.tex);
  if (kms.vbo)
    glDeleteBuffers(1, &kms.vbo);

  if (kms.fd >= 0 && kms.crtc) {
    drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0);
  }

  if (kms.egl_disp != EGL_NO_DISPLAY) {
    eglMakeCurrent(kms.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    eglTerminate(kms.egl_disp);
  }

  for (int i = 0; i < 2; i++) {
    if (kms.bufs[i].map)
      munmap(kms.bufs[i].map, kms.bufs[i].size);
    if (kms.bufs[i].fb_id)
      drmModeRmFB(kms.fd, kms.bufs[i].fb_id);
    if (kms.bufs[i].handle) {
      struct drm_mode_destroy_dumb dreq = {.handle = kms.bufs[i].handle};
      ioctl(kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    }
  }
  close(kms.fd);
  printf("Done.\n");
}

int init_drm() {
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    return -1;

  drmModeRes *res = drmModeGetResources(kms.fd);
  if (!res)
    return -1;

  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *conn = drmModeGetConnector(kms.fd, res->connectors[i]);
    if (conn->connection == DRM_MODE_CONNECTED) {
      kms.connector = conn;
      break;
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);

  if (!kms.connector) {
    fprintf(stderr, "No monitor connected\n");
    return -1;
  }

  kms.mode = kms.connector->modes[0];
  drmModeEncoder *enc = drmModeGetEncoder(kms.fd, kms.connector->encoder_id);
  if (enc)
    kms.crtc = drmModeGetCrtc(kms.fd, enc->crtc_id);
  else {
    res = drmModeGetResources(kms.fd);
    kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[0]);
    drmModeFreeResources(res);
  }

  kms.plane_primary_id = 39;
  kms.plane_overlay_id = 41;
  return 0;
}

int create_dumb_buffer(DumbBuffer *buf) {
  struct drm_mode_create_dumb create_req = {0};
  create_req.width = kms.mode.hdisplay;
  create_req.height = kms.mode.vdisplay;
  create_req.bpp = 32;
  if (ioctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0)
    return -1;
  buf->handle = create_req.handle;
  buf->stride = create_req.pitch;
  buf->size = create_req.size;

  struct drm_mode_map_dumb map_req = {0};
  map_req.handle = buf->handle;
  ioctl(kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req);
  buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, kms.fd,
                  map_req.offset);
  memset(buf->map, 0, buf->size);
  return drmModeAddFB(kms.fd, kms.mode.hdisplay, kms.mode.vdisplay, 24, 32,
                      buf->stride, buf->handle, &buf->fb_id);
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

  EGLint pbuffer_attribs[] = {EGL_WIDTH, kms.mode.hdisplay, EGL_HEIGHT,
                              kms.mode.vdisplay, EGL_NONE};
  kms.egl_surf = eglCreatePbufferSurface(kms.egl_disp, config, pbuffer_attribs);

  EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT, ctx_attribs);
  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);
  return 0;
}

int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);

  if (init_drm() < 0)
    return -1;
  if (create_dumb_buffer(&kms.bufs[0]) < 0)
    return -1;
  if (create_dumb_buffer(&kms.bufs[1]) < 0)
    return -1;
  if (init_egl() < 0)
    return -1;

  if (init_gles_objects() < 0) {
    cleanup();
    return -1;
  }

  // Load the texture now
  kms.tex = load_png_texture("test.png");
  if (!kms.tex) {
    printf("Failed to load test.png\n");
    cleanup();
    return -1;
  }

  printf("Setup Complete. Rendering 'test.png' (BGRA Mode)...\n");

  // Disable Console, Enable Primary
  drmModeSetPlane(kms.fd, kms.plane_overlay_id, kms.crtc->crtc_id, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0);
  drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                  kms.bufs[0].fb_id, 0, 0, 0, kms.mode.hdisplay,
                  kms.mode.vdisplay, 0, 0, kms.mode.hdisplay << 16,
                  kms.mode.vdisplay << 16);

  int back_buf_idx = 1;

  while (running) {
    // 1. Clear to Gray
    glClearColor(0.2, 0.2, 0.2, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);

    // 2. Draw Textured Quad
    // Note: Bind texture unit 0, as set by glUniform1i
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, kms.tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glFinish();

    // 3. Copy (FIX: Use BGRA to fix blue/red swap)
    glReadPixels(0, 0, kms.mode.hdisplay, kms.mode.vdisplay, GL_RGBA,
                 GL_UNSIGNED_BYTE, kms.bufs[back_buf_idx].map);

    // 4. Flip
    drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                    kms.bufs[back_buf_idx].fb_id, 0, 0, 0, kms.mode.hdisplay,
                    kms.mode.vdisplay, 0, 0, kms.mode.hdisplay << 16,
                    kms.mode.vdisplay << 16);

    back_buf_idx = (back_buf_idx + 1) % 2;
  }

  cleanup();
  return 0;
}