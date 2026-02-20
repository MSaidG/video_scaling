#include "gst/gstbin.h"
#include <fcntl.h>
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

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

// --- CONFIG ---
#define VIDEO_COUNT 4
const char *VIDEO_FILES[VIDEO_COUNT] = {"earth1.mp4", "zoo.mp4", "sea.mp4",
                                        "world.mp4"};

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
  GstElement *pipeline;
  GstElement *appsink;
  GstBus *bus;

  pthread_mutex_t lock;
  GstSample *new_sample;

  int width;
  int height;
  int is_new_frame_ready;

  GLuint tex_y;
  GLuint tex_uv;
} GstVid;

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
GstVid videos[VIDEO_COUNT];
volatile sig_atomic_t running = 1;

// --- LAYOUT SYSTEM ---
typedef struct {
  float x, y, w, h;
} Rect;

const Rect default_rects[4] = {
    {-1.0f, -1.0f, 1.0f, 1.0f}, // TL (Video 0)
    {0.0f, -1.0f, 1.0f, 1.0f},  // TR (Video 1)
    {-1.0f, 0.0f, 1.0f, 1.0f},  // BL (Video 2)
    {0.0f, 0.0f, 1.0f, 1.0f}    // BR (Video 3)
};

// --- SHADERS ---
const char *vs_src = "attribute vec4 a_pos;\n"
                     "attribute vec2 a_tex;\n"
                     "varying vec2 v_tex;\n"
                     "void main() {\n"
                     "   gl_Position = a_pos;\n"
                     "   v_tex = a_tex;\n"
                     "}\n";

const char *fs_src = "precision mediump float;\n"
                     "varying vec2 v_tex;\n"
                     "uniform sampler2D tex_y;\n"
                     "uniform sampler2D tex_uv;\n"
                     "vec3 yuv2rgb(float y, float u, float v) {\n"
                     "  float r = y + 1.402 * v;\n"
                     "  float g = y - 0.344 * u - 0.714 * v;\n"
                     "  float b = y + 1.772 * u;\n"
                     "  return vec3(r, g, b);\n"
                     "}\n"
                     "void main() {\n"
                     "  float y = texture2D(tex_y, v_tex).r;\n"
                     "  vec4 uv = texture2D(tex_uv, v_tex);\n"
                     "  float u = uv.r - 0.5;\n"
                     "  float v = uv.a - 0.5;\n"
                     "  gl_FragColor = vec4(yuv2rgb(y, u, v), 1.0);\n"
                     "}\n";

// --- HELPERS ---
void handle_sigint(int sig) { running = 0; }

long get_diff_us(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000000 +
         (end.tv_nsec - start.tv_nsec) / 1000;
}

// --- GSTREAMER CALLBACKS ---
static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
  GstVid *vid = (GstVid *)user_data;
  GstSample *sample = gst_app_sink_pull_sample(appsink);

  if (sample) {
    pthread_mutex_lock(&vid->lock);
    if (vid->new_sample) {
      gst_sample_unref(vid->new_sample);
    }
    vid->new_sample = sample;
    vid->is_new_frame_ready = 1;
    pthread_mutex_unlock(&vid->lock);
    return GST_FLOW_OK;
  }
  return GST_FLOW_ERROR;
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

int init_gstreamer_pipeline(GstVid *vid, const char *filename) {
  pthread_mutex_init(&vid->lock, NULL);
  vid->tex_y = 0;
  vid->tex_uv = 0;
  vid->new_sample = NULL;
  vid->is_new_frame_ready = 0;

  char pipeline_str[512];
  snprintf(pipeline_str, sizeof(pipeline_str),
           "filesrc location=%s ! qtdemux ! h264parse ! omxh264dec ! "
           "video/x-raw,format=NV12 ! appsink name=mysink sync=true drop=true "
           "max-buffers=1",
           filename);

  GError *err = NULL;
  vid->pipeline = gst_parse_launch(pipeline_str, &err);
  if (err) {
    fprintf(stderr, "GStreamer Error for %s: %s\n", filename, err->message);
    g_error_free(err);
    return -1;
  }

  vid->appsink = gst_bin_get_by_name(GST_BIN(vid->pipeline), "mysink");

  GstAppSinkCallbacks callbacks = {0};
  callbacks.new_sample = on_new_sample;
  gst_app_sink_set_callbacks(GST_APP_SINK(vid->appsink), &callbacks, vid, NULL);

  gst_element_set_state(vid->pipeline, GST_STATE_PLAYING);
  vid->bus = gst_element_get_bus(vid->pipeline);

  return 0;
}

void update_texture_cpu(GstVid *vid) {
  pthread_mutex_lock(&vid->lock);
  if (!vid->is_new_frame_ready) {
    pthread_mutex_unlock(&vid->lock);
    return; // No new frame, keep drawing the existing GPU texture
  }

  // Grab the new sample and clear the flag
  GstSample *sample = vid->new_sample;
  vid->new_sample = NULL;
  vid->is_new_frame_ready = 0;
  pthread_mutex_unlock(&vid->lock);

  if (!sample)
    return;

  // Extract Buffer and Metadata
  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstCaps *caps = gst_sample_get_caps(sample);
  GstVideoInfo vinfo;
  gst_video_info_from_caps(&vinfo, caps);

  vid->width = vinfo.width;
  vid->height = vinfo.height;

  // Map the GStreamer memory to CPU space
  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return;
  }

  // Generate textures on first run
  if (!vid->tex_y) {
    glGenTextures(1, &vid->tex_y);
    glBindTexture(GL_TEXTURE_2D, vid->tex_y);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &vid->tex_uv);
    glBindTexture(GL_TEXTURE_2D, vid->tex_uv);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  int pitch = GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 0);
  int uv_offset = GST_VIDEO_INFO_PLANE_OFFSET(&vinfo, 1);

  // Upload Y Plane to GPU VRAM
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, vid->tex_y);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, pitch, vid->height, 0,
               GL_LUMINANCE, GL_UNSIGNED_BYTE, map.data);

  // Upload UV Plane to GPU VRAM
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, vid->tex_uv);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, pitch / 2, vid->height / 2,
               0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, map.data + uv_offset);

  // Clean up
  gst_buffer_unmap(buffer, &map);

  // THE FIX: Immediately return the sample to the GStreamer pool!
  gst_sample_unref(sample);
}
void update_geometry() {
  GLfloat verts[4 * 6 * 4];
  int idx = 0;

  // We load the coordinates for all 4 quads into the VBO at once
  for (int i = 0; i < 4; i++) {
    Rect r = default_rects[i];

    // Tri 1
    verts[idx++] = r.x;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 0.0f;
    verts[idx++] = 1.0f;
    verts[idx++] = r.x;
    verts[idx++] = r.y;
    verts[idx++] = 0.0f;
    verts[idx++] = 0.0f;
    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 1.0f;
    verts[idx++] = 1.0f;

    // Tri 2
    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 1.0f;
    verts[idx++] = 1.0f;
    verts[idx++] = r.x;
    verts[idx++] = r.y;
    verts[idx++] = 0.0f;
    verts[idx++] = 0.0f;
    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y;
    verts[idx++] = 1.0f;
    verts[idx++] = 0.0f;
  }

  glBindBuffer(GL_ARRAY_BUFFER, kms.vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
}

void cleanup() {
  printf("\n--- Cleaning Up ---\n");

  for (int i = 0; i < VIDEO_COUNT; i++) {
    if (videos[i].pipeline) {
      gst_element_set_state(videos[i].pipeline, GST_STATE_NULL);
      gst_object_unref(videos[i].pipeline);
    }
    if (videos[i].bus)
      gst_object_unref(videos[i].bus);
    if (videos[i].new_sample)
      gst_sample_unref(videos[i].new_sample);
    if (videos[i].tex_y)
      glDeleteTextures(1, &videos[i].tex_y);
    if (videos[i].tex_uv)
      glDeleteTextures(1, &videos[i].tex_uv);
    pthread_mutex_destroy(&videos[i].lock);
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

int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);

  gst_init(&argc, &argv);

  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);

  drmModeRes *res = drmModeGetResources(kms.fd);
  kms.connector = drmModeGetConnector(kms.fd, res->connectors[0]);
  kms.mode = kms.connector->modes[0];
  kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[0]);
  drmModeFreeResources(res);

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

  // Init all 4 GStreamer pipelines
  for (int i = 0; i < VIDEO_COUNT; i++) {
    if (init_gstreamer_pipeline(&videos[i], VIDEO_FILES[i]) < 0) {
      cleanup();
      return -1;
    }
  }

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
  glBufferData(GL_ARRAY_BUFFER, 4 * 6 * 4 * sizeof(float), NULL,
               GL_DYNAMIC_DRAW);
  update_geometry();

  GLint loc_pos = glGetAttribLocation(kms.prog, "a_pos");
  GLint loc_tex = glGetAttribLocation(kms.prog, "a_tex");
  int stride = 4 * sizeof(float);
  glEnableVertexAttribArray(loc_pos);
  glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
  glEnableVertexAttribArray(loc_tex);
  glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, stride,
                        (void *)(2 * sizeof(float)));

  glUniform1i(glGetUniformLocation(kms.prog, "tex_y"), 0);
  glUniform1i(glGetUniformLocation(kms.prog, "tex_uv"), 1);

  drmModeSetPlane(kms.fd, kms.plane_overlay_id, kms.crtc->crtc_id, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0);

  int back_buf = 0;
  struct timespec t0, t1;
  long total_us = 0;
  int count = 0;

  printf("Running 4x GStreamer CPU Copy... Press Ctrl+C to exit.\n");

  while (running) {
    clock_gettime(CLOCK_MONOTONIC, &t0);

    glBindFramebuffer(GL_FRAMEBUFFER, kms.bufs[back_buf].fbo_id);
    glViewport(0, 0, kms.mode.hdisplay, kms.mode.vdisplay);

    glClearColor(0.2f, 0.2f, 0.8f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw each video in its respective quadrant
    for (int i = 0; i < VIDEO_COUNT; i++) {

      // --- ADD THESE 6 LINES TO LOOP THE VIDEO ---
      GstMessage *msg = gst_bus_pop_filtered(videos[i].bus, GST_MESSAGE_EOS);
      if (msg) {
        // Rewind to 0 nanoseconds
        gst_element_seek_simple(videos[i].pipeline, GST_FORMAT_TIME,
                                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                0);
        gst_message_unref(msg);
      }
      // -------------------------------------------

      update_texture_cpu(&videos[i]);

      if (videos[i].tex_y) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, videos[i].tex_y);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, videos[i].tex_uv);

        // Draw 6 vertices (2 triangles) per quadrant.
        // 0 offset for Video 0, 6 offset for Video 1, etc.
        glDrawArrays(GL_TRIANGLES, i * 6, 6);
      }
    }

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

  cleanup();
  return 0;
}