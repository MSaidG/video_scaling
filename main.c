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
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// --- CONFIG ---
#define VIDEO_COUNT 4
#define NUM_TEST_FRAMES 100

// Screen / Grid Config
#define SCREEN_W 1920.0f
#define SCREEN_H 1080.0f

// --- ANIMATION STEPS (6 Steps) ---
// Video 1 (TL Slot) - Scales both width and height proportionally
const float VID1_W[6] = {160.0f, 320.0f, 480.0f, 640.0f, 800.0f, 960.0f};
const float VID1_H[6] = {90.0f, 180.0f, 270.0f, 360.0f, 450.0f, 540.0f};

// Video 2 (TR Slot) - Width is static, height scales
const float VID2_W[6] = {960.0f, 960.0f, 960.0f, 960.0f, 960.0f, 960.0f};
const float VID2_H[6] = {90.0f, 180.0f, 270.0f, 360.0f, 450.0f, 540.0f};

// Video 3 (BL Slot) - Width scales, height is static
const float VID3_W[6] = {160.0f, 320.0f, 480.0f, 640.0f, 800.0f, 960.0f};
const float VID3_H[6] = {540.0f, 540.0f, 540.0f, 540.0f, 540.0f, 540.0f};

// Video 4 (BR Slot) - Completely static
const float VID4_W = 960.0f;
const float VID4_H = 540.0f;

// Video Sources
#define RAW_FILE_1 "nv12_480p60.yuv"
#define VID_1_W 640
#define VID_1_H 480

#define RAW_FILE_2 "nv12_1080p60.yuv"
#define VID_2_W 1920
#define VID_2_H 1080

#define RAW_FILE_3 "nv12_720p30.yuv"
#define VID_3_W 1280
#define VID_3_H 720

#define RAW_FILE_4 "nv12_300x300p30.yuv"
#define VID_4_W 300
#define VID_4_H 300

// --- EXTENSIONS ---
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
  unsigned char *data;
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
  DumbBuffer bufs[2];
  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;
  GLuint prog;
  GLuint vbo;
} kms;

// --- GLOBALS ---
VideoSource videos[VIDEO_COUNT];
volatile sig_atomic_t running = 1;

// --- LAYOUT SYSTEM ---
volatile int layout[4] = {0, 1, 2, 3}; // Maps Screen Slot -> Video Index

typedef struct {
  float x, y, w, h;
} Rect;

volatile Rect slot_rects[4];

// --- SHADERS (4 Video Support) ---
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

const char *fs_src = "precision mediump float;\n"
                     "varying vec2 v_tex;\n"
                     "varying float v_vid;\n"
                     "uniform sampler2D ty0; uniform sampler2D tu0;\n"
                     "uniform sampler2D ty1; uniform sampler2D tu1;\n"
                     "uniform sampler2D ty2; uniform sampler2D tu2;\n"
                     "uniform sampler2D ty3; uniform sampler2D tu3;\n"

                     "vec3 yuv2rgb(float y, float u, float v) {\n"
                     "  float r = y + 1.402 * v;\n"
                     "  float g = y - 0.344 * u - 0.714 * v;\n"
                     "  float b = y + 1.772 * u;\n"
                     "  return vec3(r, g, b);\n"
                     "}\n"

                     "void main() {\n"
                     "  float y, u, v;\n"
                     "  if (v_vid < 0.5) {\n"
                     "    y = texture2D(ty0, v_tex).r;\n"
                     "    vec4 uv = texture2D(tu0, v_tex);\n"
                     "    u = uv.r - 0.5; v = uv.a - 0.5;\n"
                     "  } else if (v_vid < 1.5) {\n"
                     "    y = texture2D(ty1, v_tex).r;\n"
                     "    vec4 uv = texture2D(tu1, v_tex);\n"
                     "    u = uv.r - 0.5; v = uv.a - 0.5;\n"
                     "  } else if (v_vid < 2.5) {\n"
                     "    y = texture2D(ty2, v_tex).r;\n"
                     "    vec4 uv = texture2D(tu2, v_tex);\n"
                     "    u = uv.r - 0.5; v = uv.a - 0.5;\n"
                     "  } else {\n"
                     "    y = texture2D(ty3, v_tex).r;\n"
                     "    vec4 uv = texture2D(tu3, v_tex);\n"
                     "    u = uv.r - 0.5; v = uv.a - 0.5;\n"
                     "  }\n"
                     "  gl_FragColor = vec4(yuv2rgb(y, u, v), 1.0);\n"
                     "}\n";

// --- HELPERS ---
void handle_sigint(int sig) { running = 0; }

long get_diff_us(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000000 +
         (end.tv_nsec - start.tv_nsec) / 1000;
}

void set_layout_step(int step) {
  // Prevent out of bounds (0 to 5)
  step = step % 6;

  // TL Slot (Slot 0): Pinned to Top-Left corner (-1.0, -1.0)
  slot_rects[0].x = -1.0f;
  slot_rects[0].y = -1.0f;
  slot_rects[0].w = (VID1_W[step] / SCREEN_W) * 2.0f;
  slot_rects[0].h = (VID1_H[step] / SCREEN_H) * 2.0f;

  // TR Slot (Slot 1): Pinned to Top-Mid edge (0.0, -1.0)
  slot_rects[1].x = 0.0f;
  slot_rects[1].y = -1.0f;
  slot_rects[1].w = (VID2_W[step] / SCREEN_W) * 2.0f;
  slot_rects[1].h = (VID2_H[step] / SCREEN_H) * 2.0f;

  // BL Slot (Slot 2): Pinned to Mid-Left edge (-1.0, 0.0)
  slot_rects[2].x = -1.0f;
  slot_rects[2].y = 0.0f;
  slot_rects[2].w = (VID3_W[step] / SCREEN_W) * 2.0f;
  slot_rects[2].h = (VID3_H[step] / SCREEN_H) * 2.0f;

  // BR Slot (Slot 3): Static, Pinned to Center (0.0, 0.0)
  slot_rects[3].x = 0.0f;
  slot_rects[3].y = 0.0f;
  slot_rects[3].w = (VID4_W / SCREEN_W) * 2.0f;
  slot_rects[3].h = (VID4_H / SCREEN_H) * 2.0f;
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
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, buf->egl_img);

  glGenFramebuffers(1, &buf->fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         buf->tex_id, 0);
  return 0;
}

// Init Video
int init_video_source(VideoSource *v, const char *filename, int width,
                      int height) {
  v->filename = filename;
  v->width = width;
  v->height = height;
  v->curr_frame_idx = 0;
  v->total_frames = NUM_TEST_FRAMES;

  v->frame_size = width * height * 3 / 2;
  v->total_size = v->frame_size * NUM_TEST_FRAMES;

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
  read(fd, v->data, v->total_size);
  close(fd);
  printf("Done.\n");

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
  GLfloat verts[4 * 6 * 5];
  int idx = 0;

  for (int i = 0; i < 4; i++) {
    Rect r = slot_rects[i];
    float id = (float)layout[i]; 

    // Tri 1
    verts[idx++] = r.x;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 0.0f;
    verts[idx++] = 1.0f;
    verts[idx++] = id;

    verts[idx++] = r.x;
    verts[idx++] = r.y;
    verts[idx++] = 0.0f;
    verts[idx++] = 0.0f; 
    verts[idx++] = id;

    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 1.0f;
    verts[idx++] = 1.0f;
    verts[idx++] = id;

    // Tri 2
    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 1.0f;
    verts[idx++] = 1.0f; 
    verts[idx++] = id;

    verts[idx++] = r.x;
    verts[idx++] = r.y;
    verts[idx++] = 0.0f;
    verts[idx++] = 0.0f; 
    verts[idx++] = id;

    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y;
    verts[idx++] = 1.0f;
    verts[idx++] = 0.0f; 
    verts[idx++] = id;
  }

  glBindBuffer(GL_ARRAY_BUFFER, kms.vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
}

int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);

  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);

  drmModeRes *res = drmModeGetResources(kms.fd);
  kms.connector = drmModeGetConnector(kms.fd, res->connectors[0]);
  kms.mode = kms.connector->modes[0];
  kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[0]);
  drmModeFreeResources(res);

  // STATIC PLANES 
  kms.plane_primary_id = 39;
  kms.plane_overlay_id = 41;

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

  // --- INIT 4 VIDEOS ---
  if (init_video_source(&videos[0], RAW_FILE_1, VID_1_W, VID_1_H) < 0) {
    cleanup();
    return -1;
  }
  if (init_video_source(&videos[1], RAW_FILE_2, VID_2_W, VID_2_H) < 0) {
    cleanup();
    return -1;
  }
  if (init_video_source(&videos[2], RAW_FILE_3, VID_3_W, VID_3_H) < 0) {
    cleanup();
    return -1;
  }
  if (init_video_source(&videos[3], RAW_FILE_4, VID_4_W, VID_4_H) < 0) {
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
  glBufferData(GL_ARRAY_BUFFER, 4 * 6 * 5 * sizeof(float), NULL,
               GL_DYNAMIC_DRAW);
               
  // Start on step 0
  set_layout_step(0);
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

  // Bind all 8 Texture Units (4 Videos x 2 Planes)
  glUniform1i(glGetUniformLocation(kms.prog, "ty0"), 0);
  glUniform1i(glGetUniformLocation(kms.prog, "tu0"), 1);
  glUniform1i(glGetUniformLocation(kms.prog, "ty1"), 2);
  glUniform1i(glGetUniformLocation(kms.prog, "tu1"), 3);
  glUniform1i(glGetUniformLocation(kms.prog, "ty2"), 4);
  glUniform1i(glGetUniformLocation(kms.prog, "tu2"), 5);
  glUniform1i(glGetUniformLocation(kms.prog, "ty3"), 6);
  glUniform1i(glGetUniformLocation(kms.prog, "tu3"), 7);

  // Disable Console
  drmModeSetPlane(kms.fd, kms.plane_overlay_id, kms.crtc->crtc_id, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0);

  int back_buf = 0;
  struct timespec t0, t1;
  long total_us = 0;
  int count = 0;
  int current_step = -1;

  printf("Running 6-Step Animated Layout... Press Ctrl+C to exit.\n");

  while (running) {
    clock_gettime(CLOCK_MONOTONIC, &t0);

    // --- Check if we need to advance to the next step ---
    int step = (t0.tv_sec / 3) % 6; // This will cycle 0, 1, 2, 3, 4, 5 every second
    if (step != current_step) {
      current_step = step;
      set_layout_step(current_step);
      update_geometry();
    }

    // Upload 4 Videos
    upload_video_frame(&videos[layout[0]], 0);
    upload_video_frame(&videos[layout[1]], 2);
    upload_video_frame(&videos[layout[2]], 4);
    upload_video_frame(&videos[layout[3]], 6);

    glBindFramebuffer(GL_FRAMEBUFFER, kms.bufs[back_buf].fbo_id);
    glViewport(0, 0, kms.mode.hdisplay, kms.mode.vdisplay);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f); // Clear to Black
    glClear(GL_COLOR_BUFFER_BIT);

    glDrawArrays(GL_TRIANGLES, 0, 24); // 4 Quads

    drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                    kms.bufs[back_buf].fb_id, 0, 0, 0, kms.mode.hdisplay,
                    kms.mode.vdisplay, 0, 0, kms.mode.hdisplay << 16,
                    kms.mode.vdisplay << 16);

    back_buf = !back_buf;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    total_us += get_diff_us(t0, t1);
    count++;
    if (count >= 60) {
      printf("FPS: %ld (Step %d/6)\r\n", 1000000 / (total_us / 60), current_step + 1);
      total_us = 0;
      count = 0;
    }
  }

  cleanup();
  return 0;
}