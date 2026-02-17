#include <fcntl.h>
#include <ncurses.h>
#include <pthread.h>
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
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// --- CONFIG ---
#define VIDEO_COUNT 2
#define NUM_TEST_FRAMES 100

// Video Configs
#define RAW_FILE_1 "nv12_480p60.yuv"
#define VID_1_W 640
#define VID_1_H 480

#define RAW_FILE_2 "nv12_1080p60.yuv"
#define VID_2_W 1920
#define VID_2_H 1080

// --- EXTENSION DEFINITIONS ---
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

// --- STRUCTURES ---
typedef struct {
  uint32_t handle;
  uint32_t stride;
  uint32_t size;
  uint32_t fb_id;
  int prime_fd;
  EGLImageKHR egl_img;
  GLuint tex_id;
  GLuint fbo_id;
} DumbBuffer;

typedef struct {
  const char *filename;
  unsigned char *data; // RAM Cache
  size_t frame_size;
  size_t total_size;
  int total_frames;
  int curr_frame_idx;
  GLuint tex_y;
  GLuint tex_uv;
  int width;
  int height;
} VideoSource;

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
  EGLSurface egl_surf;

  GLuint prog;
  GLuint vbo;
} kms;

// --- GLOBALS ---
VideoSource videos[VIDEO_COUNT];
volatile sig_atomic_t running = 1;

// Layout State: 0 = Horizontal, 1 = Vertical
volatile int current_layout = 0;
volatile int layout_dirty = 1;

// --- SHADERS ---
const char *vs_src = "attribute vec4 a_pos;\n"
                     "attribute vec2 a_tex;\n"
                     "attribute float a_vid;\n"
                     "varying vec2 v_tex;\n"
                     "varying float v_vid;\n"
                     "void main() {\n"
                     "   gl_Position = a_pos;\n"
                     "   v_tex = a_tex;\n"
                     "   v_vid = a_vid;\n"
                     "}\n";

const char *fs_src =
    "precision mediump float;\n"
    "varying vec2 v_tex;\n"
    "varying float v_vid;\n"
    "uniform sampler2D ty0; uniform sampler2D tu0;\n" // Video 0
    "uniform sampler2D ty1; uniform sampler2D tu1;\n" // Video 1

    "vec3 yuv2rgb(float y, float u, float v) {\n"
    "  float r = y + 1.402 * v;\n"
    "  float g = y - 0.344 * u - 0.714 * v;\n"
    "  float b = y + 1.772 * u;\n"
    "  return vec3(r, g, b);\n"
    "}\n"

    "void main() {\n"
    "  float y, u, v;\n"
    "  if (v_vid < 0.5) {\n" // Video 0
    "    y = texture2D(ty0, v_tex).r;\n"
    "    vec4 uv = texture2D(tu0, v_tex);\n"
    "    u = uv.r - 0.5; v = uv.a - 0.5;\n"
    "  } else {\n" // Video 1
    "    y = texture2D(ty1, v_tex).r;\n"
    "    vec4 uv = texture2D(tu1, v_tex);\n"
    "    u = uv.r - 0.5; v = uv.a - 0.5;\n"
    "  }\n"
    "  gl_FragColor = vec4(yuv2rgb(y, u, v), 1.0);\n"
    "}\n";

// --- UTILS ---
void handle_sigint(int sig) { running = 0; }

long get_diff_us(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000000 +
         (end.tv_nsec - start.tv_nsec) / 1000;
}

// --- CLEANUP ---
void cleanup() {
  printf("\n--- Cleaning Up ---\n");
  for (int i = 0; i < VIDEO_COUNT; i++) {
    if (videos[i].tex_y)
      glDeleteTextures(1, &videos[i].tex_y);
    if (videos[i].tex_uv)
      glDeleteTextures(1, &videos[i].tex_uv);
    if (videos[i].data)
      free(videos[i].data);
  }
  if (kms.prog)
    glDeleteProgram(kms.prog);
  if (kms.vbo)
    glDeleteBuffers(1, &kms.vbo);

  for (int i = 0; i < 2; i++) {
    if (kms.bufs[i].fbo_id)
      glDeleteFramebuffers(1, &kms.bufs[i].fbo_id);
    if (kms.bufs[i].tex_id)
      glDeleteTextures(1, &kms.bufs[i].tex_id);
    if (kms.bufs[i].egl_img && eglDestroyImageKHR)
      eglDestroyImageKHR(kms.egl_disp, kms.bufs[i].egl_img);
    if (kms.bufs[i].prime_fd >= 0)
      close(kms.bufs[i].prime_fd);
    if (kms.bufs[i].fb_id)
      drmModeRmFB(kms.fd, kms.bufs[i].fb_id);
    if (kms.bufs[i].handle) {
      struct drm_mode_destroy_dumb destroy_req = {.handle = kms.bufs[i].handle};
      ioctl(kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    }
  }
  if (kms.egl_disp != EGL_NO_DISPLAY) {
    eglMakeCurrent(kms.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    eglTerminate(kms.egl_disp);
  }
  if (kms.crtc)
    drmModeFreeCrtc(kms.crtc);
  if (kms.connector)
    drmModeFreeConnector(kms.connector);
  if (kms.fd >= 0)
    close(kms.fd);
  printf("Done.\n");
}

// --- INPUT THREAD ---
void *input_thread(void *arg) {
  initscr();
  cbreak();
  noecho();
  nodelay(stdscr, TRUE);
  keypad(stdscr, TRUE);
  while (running) {
    int ch = getch();
    if (ch == 'q')
      running = 0;
    else if (ch == 'h') {
      current_layout = 0;
      layout_dirty = 1;
    } else if (ch == 'v') {
      current_layout = 1;
      layout_dirty = 1;
    }
    usleep(10000);
  }
  endwin();
  return NULL;
}

// --- SETUP FUNCTIONS ---
int load_egl_extensions() {
  eglCreateImageKHR =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  eglDestroyImageKHR =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  glEGLImageTargetTexture2DOES =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
          "glEGLImageTargetTexture2DOES");
  return (eglCreateImageKHR && glEGLImageTargetTexture2DOES) ? 0 : -1;
}

int create_dumb_buffer_fbo(DumbBuffer *buf) {
  struct drm_mode_create_dumb create_req = {0};
  create_req.width = kms.mode.hdisplay;
  create_req.height = kms.mode.vdisplay;
  create_req.bpp = 32;
  ioctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);

  buf->handle = create_req.handle;
  buf->stride = create_req.pitch;
  buf->size = create_req.size;

  drmModeAddFB(kms.fd, kms.mode.hdisplay, kms.mode.vdisplay, 24, 32,
               buf->stride, buf->handle, &buf->fb_id);

  struct drm_prime_handle prime = {0};
  prime.handle = buf->handle;
  prime.flags = DRM_CLOEXEC | DRM_RDWR;
  ioctl(kms.fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
  buf->prime_fd = prime.fd;

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

  glGenTextures(1, &buf->tex_id);
  glBindTexture(GL_TEXTURE_2D, buf->tex_id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, buf->egl_img);

  glGenFramebuffers(1, &buf->fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         buf->tex_id, 0);
  return 0;
}

// Init Video (Proper sizing logic)
int init_video_source(VideoSource *v, const char *filename, int width,
                      int height) {
  v->filename = filename;
  v->width = width;
  v->height = height;
  v->curr_frame_idx = 0;
  v->total_frames = NUM_TEST_FRAMES;

  v->frame_size = width * height * 3 / 2;
  v->total_size = v->frame_size * NUM_TEST_FRAMES;

  // Allocate RAM Cache
  v->data = malloc(v->total_size);
  if (!v->data) {
    fprintf(stderr, "Failed to allocate memory for %s\n", filename);
    return -1;
  }

  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    free(v->data);
    return -1;
  }

  printf("Loading %s (%zu MB)... ", filename, v->total_size / 1024 / 1024);
  read(fd, v->data, v->total_size); // Load all frames
  close(fd);
  printf("Done.\n");

  // Create Textures
  glGenTextures(1, &v->tex_y);
  glBindTexture(GL_TEXTURE_2D, v->tex_y);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenTextures(1, &v->tex_uv);
  glBindTexture(GL_TEXTURE_2D, v->tex_uv);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  return 0;
}

// Upload Frame (Respects individual video sizes)
void upload_video_frame(VideoSource *v, int base_unit) {
  unsigned char *f = v->data + (v->curr_frame_idx * v->frame_size);
  unsigned char *uv = f + (v->width * v->height);

  glActiveTexture(GL_TEXTURE0 + base_unit);
  glBindTexture(GL_TEXTURE_2D, v->tex_y);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, v->width, v->height, 0,
               GL_LUMINANCE, GL_UNSIGNED_BYTE, f);

  glActiveTexture(GL_TEXTURE0 + base_unit + 1);
  glBindTexture(GL_TEXTURE_2D, v->tex_uv);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, v->width / 2,
               v->height / 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv);

  v->curr_frame_idx = (v->curr_frame_idx + 1) % v->total_frames;
}

void update_geometry() {
  GLfloat verts[2 * 6 * 5];
  int idx = 0;
  typedef struct {
    float x, y, w, h;
  } Rect;
  Rect r1, r2;

  if (current_layout == 0) {               // Horizontal
    r1 = (Rect){-1.0f, -1.0f, 1.0f, 2.0f}; // Left
    r2 = (Rect){0.0f, -1.0f, 1.0f, 2.0f};  // Right
  } else {                                 // Vertical
    r1 = (Rect){-1.0f, 0.0f, 2.0f, 1.0f};  // Top
    r2 = (Rect){-1.0f, -1.0f, 2.0f, 1.0f}; // Bottom
  }
  Rect rects[2] = {r1, r2};

  for (int i = 0; i < 2; i++) {
    Rect r = rects[i];
    float id = (float)i;
    // Tri 1
    verts[idx++] = r.x;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 0.0f;
    verts[idx++] = 0.0f;
    verts[idx++] = id;
    verts[idx++] = r.x;
    verts[idx++] = r.y;
    verts[idx++] = 0.0f;
    verts[idx++] = 1.0f;
    verts[idx++] = id;
    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 1.0f;
    verts[idx++] = 0.0f;
    verts[idx++] = id;
    // Tri 2
    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 1.0f;
    verts[idx++] = 0.0f;
    verts[idx++] = id;
    verts[idx++] = r.x;
    verts[idx++] = r.y;
    verts[idx++] = 0.0f;
    verts[idx++] = 1.0f;
    verts[idx++] = id;
    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y;
    verts[idx++] = 1.0f;
    verts[idx++] = 1.0f;
    verts[idx++] = id;
  }

  glBindBuffer(GL_ARRAY_BUFFER, kms.vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
  layout_dirty = 0;
}

int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);

  // --- INIT DRM ---
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);

  drmModeRes *res = drmModeGetResources(kms.fd);
  kms.connector = drmModeGetConnector(kms.fd, res->connectors[0]);
  kms.mode = kms.connector->modes[0];
  kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[0]);
  drmModeFreeResources(res);

  // STATIC PLANES (As requested)
  kms.plane_primary_id = 39;
  kms.plane_overlay_id = 41;

  // --- INIT EGL ---
  kms.egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(kms.egl_disp, NULL, NULL)) {
    kms.egl_disp = eglGetDisplay((EGLNativeDisplayType)kms.fd);
    eglInitialize(kms.egl_disp, NULL, NULL);
  }
  eglBindAPI(EGL_OPENGL_ES_API);

  EGLConfig config;
  EGLint num;
  EGLint attribs[] = {EGL_SURFACE_TYPE,
                      EGL_PBUFFER_BIT,
                      EGL_RED_SIZE,
                      8,
                      EGL_GREEN_SIZE,
                      8,
                      EGL_BLUE_SIZE,
                      8,
                      EGL_RENDERABLE_TYPE,
                      EGL_OPENGL_ES2_BIT,
                      EGL_NONE};
  eglChooseConfig(kms.egl_disp, attribs, &config, 1, &num);
  kms.egl_surf = eglCreatePbufferSurface(
      kms.egl_disp, config, (EGLint[]){EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE});
  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT,
                       (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);
  load_egl_extensions();

  create_dumb_buffer_fbo(&kms.bufs[0]);
  create_dumb_buffer_fbo(&kms.bufs[1]);

  // --- INIT VIDEOS & GRAPHICS ---
  if (init_video_source(&videos[0], RAW_FILE_1, VID_1_W, VID_1_H) < 0) {
    cleanup();
    return -1;
  }
  if (init_video_source(&videos[1], RAW_FILE_2, VID_2_W, VID_2_H) < 0) {
    cleanup();
    return -1;
  }

  // Init Shader
  kms.prog = glCreateProgram();
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_src, NULL);
  glCompileShader(vs);
  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_src, NULL);
  glCompileShader(fs);
  glAttachShader(kms.prog, vs);
  glAttachShader(kms.prog, fs);
  glLinkProgram(kms.prog);
  glUseProgram(kms.prog);

  glGenBuffers(1, &kms.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, kms.vbo);
  glBufferData(GL_ARRAY_BUFFER, 2 * 6 * 5 * sizeof(float), NULL,
               GL_DYNAMIC_DRAW);
  update_geometry();

  GLint loc_pos = glGetAttribLocation(kms.prog, "a_pos");
  GLint loc_tex = glGetAttribLocation(kms.prog, "a_tex");
  GLint loc_vid = glGetAttribLocation(kms.prog, "a_vid");
  int stride = 5 * sizeof(float);
  glEnableVertexAttribArray(loc_pos);
  glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
  glEnableVertexAttribArray(loc_tex);
  glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, stride,
                        (void *)(2 * sizeof(float)));
  glEnableVertexAttribArray(loc_vid);
  glVertexAttribPointer(loc_vid, 1, GL_FLOAT, GL_FALSE, stride,
                        (void *)(4 * sizeof(float)));

  glUniform1i(glGetUniformLocation(kms.prog, "ty0"), 0);
  glUniform1i(glGetUniformLocation(kms.prog, "tu0"), 1);
  glUniform1i(glGetUniformLocation(kms.prog, "ty1"), 2);
  glUniform1i(glGetUniformLocation(kms.prog, "tu1"), 3);

  pthread_t tid;
  pthread_create(&tid, NULL, input_thread, NULL);

  // Disable Console
  drmModeSetPlane(kms.fd, kms.plane_overlay_id, kms.crtc->crtc_id, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0);

  int back_buf = 0;
  struct timespec t0, t1;
  long total_us = 0;
  int count = 0;

  printf("Running... Press 'h', 'v', 'q'.\n");

  while (running) {
    clock_gettime(CLOCK_MONOTONIC, &t0);

    if (layout_dirty)
      update_geometry();

    // UPLOAD: Each video uploaded with its own size
    upload_video_frame(&videos[0], 0); // Video 0 -> Texture Unit 0 & 1
    upload_video_frame(&videos[1], 2); // Video 1 -> Texture Unit 2 & 3

    glBindFramebuffer(GL_FRAMEBUFFER, kms.bufs[back_buf].fbo_id);
    glViewport(0, 0, kms.mode.hdisplay, kms.mode.vdisplay);

    // Debug Color: Red
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glDrawArrays(GL_TRIANGLES, 0, 12);

    // FLIP
    drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                    kms.bufs[back_buf].fb_id, 0, 0, 0, kms.mode.hdisplay,
                    kms.mode.vdisplay, 0, 0, kms.mode.hdisplay << 16,
                    kms.mode.vdisplay << 16);

    back_buf = !back_buf;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    total_us += get_diff_us(t0, t1);
    count++;
    if (count >= 60) {
      printf("FPS: %ld\r\n", 1000000 / (total_us / 60));
      total_us = 0;
      count = 0;
    }
  }

  pthread_join(tid, NULL);
  cleanup();
  return 0;
}