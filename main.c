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
#include <xf86drm.h>
#include <xf86drmMode.h>

#define VID_W 1920
#define VID_H 1080
#define RAW_FILE "nv12_1080p60.yuv"

// --- YUV TO RGB SHADER ---
const char *vs_src = "attribute vec4 a_pos;    \n"
                     "attribute vec2 a_tex;    \n"
                     "varying vec2 v_tex;      \n"
                     "void main() {            \n"
                     "   gl_Position = a_pos;  \n"
                     "   v_tex = a_tex;        \n"
                     "}                        \n";

const char *fs_src = "precision mediump float;             \n"
                     "varying vec2 v_tex;                  \n"
                     "uniform sampler2D tex_y;             \n" // Texture Unit 0
                     "uniform sampler2D tex_uv;            \n" // Texture Unit 1
                     "void main() {                        \n"
                     "  float y = texture2D(tex_y, v_tex).r;          \n"
                     "  vec4 uv_raw = texture2D(tex_uv, v_tex);       \n"
                     // NV12 Logic: U is in Luminance (r), V is in Alpha (a)
                     "  float u = uv_raw.r - 0.5;                     \n"
                     "  float v = uv_raw.a - 0.5;                     \n"
                     // BT.601 Conversion
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
  // Camera/Video Resources
  int cam_fd;
  size_t frame_size;
  unsigned char *frame_buffer; // RAM buffer for current frame
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

// --- VIDEO INPUT ---
int init_raw_input() {
  kms.cam_fd = open(RAW_FILE, O_RDONLY);
  if (kms.cam_fd < 0) {
    fprintf(stderr, "Error: Cannot open %s\n", RAW_FILE);
    return -1;
  }

  // NV12 Size = W * H * 1.5
  kms.frame_size = VID_W * VID_H * 3 / 2;
  kms.frame_buffer = malloc(kms.frame_size);
  if (!kms.frame_buffer) {
    fprintf(stderr, "OOM: Cannot allocate frame buffer\n");
    return -1;
  }

  printf("Opened Raw File. Frame Size: %zu bytes\n", kms.frame_size);

  // 1. Create Y Texture (Luminance)
  glGenTextures(1, &kms.tex_y);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, kms.tex_y);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  // 2. Create UV Texture (Luminance Alpha)
  glGenTextures(1, &kms.tex_uv);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, kms.tex_uv);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  return 0;
}

// Read next frame from disk and upload to GPU
void update_texture() {
  // Read from file
  ssize_t ret = read(kms.cam_fd, kms.frame_buffer, kms.frame_size);
  if (ret != kms.frame_size) {
    // Loop video: Seek to beginning
    lseek(kms.cam_fd, 0, SEEK_SET);
    read(kms.cam_fd, kms.frame_buffer, kms.frame_size);
  }

  unsigned char *y_plane = kms.frame_buffer;
  unsigned char *uv_plane = kms.frame_buffer + (VID_W * VID_H);

  // Upload Y (Unit 0)
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, kms.tex_y);
  // Note: GL_LUMINANCE is deprecated in modern GL but valid in GLES2
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, VID_W, VID_H, 0, GL_LUMINANCE,
               GL_UNSIGNED_BYTE, y_plane);

  // Upload UV (Unit 1)
  // Resolution is W/2 x H/2. Format is Lum/Alpha (2 bytes per pixel)
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, kms.tex_uv);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, VID_W / 2, VID_H / 2, 0,
               GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv_plane);
}

// --- INIT GLES ---
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

  // Pass-through Quad (No flip needed if video is naturally right-side up)
  // If video is upside down, swap the V coordinates (0.0 <-> 1.0)
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
  GLint loc_y = glGetUniformLocation(kms.prog, "tex_y");
  GLint loc_uv = glGetUniformLocation(kms.prog, "tex_uv");

  glEnableVertexAttribArray(loc_pos);
  glEnableVertexAttribArray(loc_tex);
  glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)0);
  glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)(2 * sizeof(float)));

  // Set Uniforms to match Texture Units
  glUniform1i(loc_y, 0);  // tex_y  -> Unit 0
  glUniform1i(loc_uv, 1); // tex_uv -> Unit 1

  return 0;
}

void cleanup() {
  printf("\nCleaning up...\n");
  if (kms.frame_buffer)
    free(kms.frame_buffer);
  if (kms.cam_fd >= 0)
    close(kms.cam_fd);

  // Standard Cleanup...
  if (kms.prog)
    glDeleteProgram(kms.prog);
  if (kms.tex_y)
    glDeleteTextures(1, &kms.tex_y);
  if (kms.tex_uv)
    glDeleteTextures(1, &kms.tex_uv);
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
  if (!kms.connector)
    return -1;

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
  if (init_raw_input() < 0) {
    cleanup();
    return -1;
  }
  if (init_gles_objects() < 0) {
    cleanup();
    return -1;
  }

  printf("Setup Complete. Playing '%s'...\n", RAW_FILE);

  // Plane Setup
  drmModeSetPlane(kms.fd, kms.plane_overlay_id, kms.crtc->crtc_id, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0);
  drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                  kms.bufs[0].fb_id, 0, 0, 0, kms.mode.hdisplay,
                  kms.mode.vdisplay, 0, 0, kms.mode.hdisplay << 16,
                  kms.mode.vdisplay << 16);

  int back_buf_idx = 1;

  double last_time = get_time_sec();
  int frames = 0;

  while (running) {
    // 1. Upload Next Frame from Disk to GPU
    update_texture();

    // 2. Render Quad
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();

    // 3. Read Pixels (GPU -> CPU)
    // Note: If Red/Blue are swapped, change to GL_BGRA_EXT
    glReadPixels(0, 0, kms.mode.hdisplay, kms.mode.vdisplay, GL_RGBA,
                 GL_UNSIGNED_BYTE, kms.bufs[back_buf_idx].map);

    // 4. Flip
    drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                    kms.bufs[back_buf_idx].fb_id, 0, 0, 0, kms.mode.hdisplay,
                    kms.mode.vdisplay, 0, 0, kms.mode.hdisplay << 16,
                    kms.mode.vdisplay << 16);

    back_buf_idx = (back_buf_idx + 1) % 2;

    // FPS Calculation
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