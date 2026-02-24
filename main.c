#include "glib.h"
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

// --- STRUCTURES ---
typedef struct {
  uint32_t handle;
  uint32_t stride;
  uint32_t size;
  uint32_t fb_id;
  int prime_fd;
  GLuint tex_id;
  GLuint fbo_id;
  void *cpu_map; // CPU pointer to the DRM 24-bit memory
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
  uint32_t plane_id;
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
int create_dumb_buffer_fbo(DumbBuffer *buf) {
  // --- 1. DRM: 24-bit HDMI Hardware Buffer ---
  struct drm_mode_create_dumb create_req = {0};
  create_req.width = kms.mode.hdisplay;
  create_req.height = kms.mode.vdisplay;
  create_req.bpp = 24; // Strict 24-bit for Ambarella HDMI
  ioctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);

  buf->handle = create_req.handle;
  buf->stride = create_req.pitch;
  buf->size = create_req.size;

  uint32_t handles[4] = {buf->handle};
  uint32_t pitches[4] = {buf->stride};
  uint32_t offsets[4] = {0};

  drmModeAddFB2(kms.fd, kms.mode.hdisplay, kms.mode.vdisplay,
                DRM_FORMAT_RGB888, handles, pitches, offsets, &buf->fb_id, 0);

  // Map the 24-bit DRM buffer to CPU memory
  struct drm_mode_map_dumb map_req = {.handle = buf->handle};
  ioctl(kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req);
  buf->cpu_map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, kms.fd,
                      map_req.offset);

  if (buf->cpu_map == MAP_FAILED) {
      printf("Failed to map DRM buffer!\n");
      return -1;
  }

  // --- 2. EGL: Standard 32-bit Off-screen FBO ---
  glGenTextures(1, &buf->tex_id);
  glBindTexture(GL_TEXTURE_2D, buf->tex_id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  // Standard RGBA texture for the GPU to render into
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kms.mode.hdisplay, kms.mode.vdisplay,
               0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

  glGenFramebuffers(1, &buf->fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         buf->tex_id, 0);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    printf("FBO incomplete!\n");
    return -1;
  } else {
    printf("FBO & DRM Buffers perfectly decoupled and created!\n");
  }
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
    return; 
  }

  GstSample *sample = vid->new_sample;
  vid->new_sample = NULL;
  vid->is_new_frame_ready = 0;
  pthread_mutex_unlock(&vid->lock);

  if (!sample)
    return;

  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstCaps *caps = gst_sample_get_caps(sample);
  GstVideoInfo vinfo;
  gst_video_info_from_caps(&vinfo, caps);

  vid->width = vinfo.width;
  vid->height = vinfo.height;

  GstMapInfo map;
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    gst_sample_unref(sample);
    return;
  }

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

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, vid->tex_y);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, pitch, vid->height, 0,
               GL_LUMINANCE, GL_UNSIGNED_BYTE, map.data);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, vid->tex_uv);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, pitch / 2, vid->height / 2,
               0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, map.data + uv_offset);

  gst_buffer_unmap(buffer, &map);
  gst_sample_unref(sample);
}

void update_geometry(int step) {
  GLfloat verts[4 * 6 * 4];
  int idx = 0;

  float m = (step + 1) / 6.0f;

  Rect rects[4];
  rects[0] = (Rect){-1.0f, -1.0f, m, m};
  rects[1] = (Rect){0.0f, -1.0f, 1.0f, m};
  rects[2] = (Rect){-1.0f, 0.0f, m, 1.0f};
  rects[3] = (Rect){0.0f, 0.0f, 1.0f, 1.0f};

  for (int i = 0; i < 4; i++) {
    Rect r = rects[i];

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
    if (kms.bufs[i].cpu_map && kms.bufs[i].cpu_map != MAP_FAILED)
      munmap(kms.bufs[i].cpu_map, kms.bufs[i].size);
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

  kms.fd = open("/dev/dri/by-path/platform-amba_pl@0:drm_hdmi-card",
                O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);

  drmModeRes *res = drmModeGetResources(kms.fd);

  // Find HDMI Connector (ID 38)
  kms.connector = NULL;
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *c = drmModeGetConnector(kms.fd, res->connectors[i]);
    if (c->connector_id == 38) { 
      kms.connector = c;
      break;
    }
    drmModeFreeConnector(c);
  }

  if (!kms.connector) {
    printf("Error: Could not find HDMI Connector (ID 38)!\n");
    return -1;
  }

  kms.mode = kms.connector->modes[0];

  // Find HDMI CRTC (ID 34)
  kms.crtc = NULL;
  for (int i = 0; i < res->count_crtcs; i++) {
    if (res->crtcs[i] == 34) { 
      kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[i]);
      break;
    }
  }

  if (!kms.crtc) {
    printf("Error: Could not find HDMI CRTC (ID 34)!\n");
    return -1;
  }

  drmModeFreeResources(res);

  kms.plane_id = 32; // HDMI Primary Plane

  printf("Successfully bound to HDMI -> Connector: %d, CRTC: %d, Plane: %d\n",
         kms.connector->connector_id, kms.crtc->crtc_id, kms.plane_id);

  kms.egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(kms.egl_disp, NULL, NULL)) {
    kms.egl_disp = eglGetDisplay((EGLNativeDisplayType)kms.fd);
    eglInitialize(kms.egl_disp, NULL, NULL);
  }
  eglBindAPI(EGL_OPENGL_ES_API);

  EGLConfig config;
  EGLint num;
  EGLint attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                      EGL_RED_SIZE, 8,
                      EGL_GREEN_SIZE, 8,
                      EGL_BLUE_SIZE, 8,
                      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                      EGL_NONE};
  eglChooseConfig(kms.egl_disp, attribs, &config, 1, &num);
  kms.egl_surf = eglCreatePbufferSurface(
      kms.egl_disp, config, (EGLint[]){EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE});
  kms.egl_ctx = eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT,
                       (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);

  create_dumb_buffer_fbo(&kms.bufs[0]);
  create_dumb_buffer_fbo(&kms.bufs[1]);

  // Turn on the display with Buffer 0
  int ret = drmModeSetCrtc(kms.fd, kms.crtc->crtc_id, kms.bufs[0].fb_id, 0, 0,
                           &kms.connector->connector_id, 1, &kms.mode);

  if (ret) {
    fprintf(stderr, "failed to set mode: %s\n", strerror(errno));
    cleanup();
    return -1;
  }

  printf("Display Mode Set! Entering render loop...\n");

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

  int current_anim_step = 0;
  update_geometry(current_anim_step);

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

  int back_buf = 0;
  struct timespec t0, t1;
  long total_us = 0;
  int count = 0;

  // Allocate temporary memory for extracting the GPU 32-bit FBO
  uint8_t *temp_rgba = malloc(kms.mode.hdisplay * kms.mode.vdisplay * 4);

  printf("Running 4x GStreamer CPU Bridge Copy... Press Ctrl+C to exit.\n");

  struct timespec anim_t0, anim_t1;
  clock_gettime(CLOCK_MONOTONIC, &anim_t0);
  const double ANIM_STEP_SEC = 2.0;

  while (running) {
    clock_gettime(CLOCK_MONOTONIC, &t0);

    // --- ANIMATION CHECK ---
    clock_gettime(CLOCK_MONOTONIC, &anim_t1);
    double elapsed = (anim_t1.tv_sec - anim_t0.tv_sec) +
                     (anim_t1.tv_nsec - anim_t0.tv_nsec) / 1e9;

    if (elapsed >= ANIM_STEP_SEC) {
      current_anim_step = (current_anim_step + 1) % 6; 
      update_geometry(current_anim_step);
      anim_t0 = anim_t1; 
    }

    glBindFramebuffer(GL_FRAMEBUFFER, kms.bufs[back_buf].fbo_id);
    glViewport(0, 0, kms.mode.hdisplay, kms.mode.vdisplay);

    // Bright Red background to verify it draws correctly
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    for (int i = 0; i < VIDEO_COUNT; i++) {
      GstMessage *msg = gst_bus_pop_filtered(videos[i].bus, GST_MESSAGE_EOS);
      if (msg) {
        gst_element_seek_simple(videos[i].pipeline, GST_FORMAT_TIME,
                                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                0);
        gst_message_unref(msg);
      }

      update_texture_cpu(&videos[i]);

      if (videos[i].tex_y) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, videos[i].tex_y);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, videos[i].tex_uv);

        glDrawArrays(GL_TRIANGLES, i * 6, 6);
      }
    }

    glFinish(); // Wait for the GPU to completely finish drawing the FBO

    // --- THE BRIDGE ---
    // 1. Extract 32-bit RGBA from the GPU
    glReadPixels(0, 0, kms.mode.hdisplay, kms.mode.vdisplay, GL_RGBA,
                 GL_UNSIGNED_BYTE, temp_rgba);

    int width = kms.mode.hdisplay;
    int height = kms.mode.vdisplay;
    int stride_24 = kms.bufs[back_buf].stride;
    uint8_t *dest = (uint8_t *)kms.bufs[back_buf].cpu_map;

    // 2. Flip vertically and pack into 24-bit RGB memory for DRM
    for (int y = 0; y < height; y++) {
      int gl_y = height - 1 - y; // OpenGL is bottom-up
      uint8_t *src_row = temp_rgba + (gl_y * width * 4);
      uint8_t *dst_row = dest + (y * stride_24);

      for (int x = 0; x < width; x++) {
        dst_row[x * 3 + 0] = src_row[x * 4 + 2]; // B
        dst_row[x * 3 + 1] = src_row[x * 4 + 1]; // G
        dst_row[x * 3 + 2] = src_row[x * 4 + 0]; // R
      }
    }

    // 3. Scan out the perfectly formatted 24-bit memory to HDMI
    int r = drmModeSetPlane(kms.fd, kms.plane_id, kms.crtc->crtc_id,
                            kms.bufs[back_buf].fb_id, 0, 0, 0,
                            kms.mode.hdisplay, kms.mode.vdisplay, 0, 0,
                            kms.mode.hdisplay << 16, kms.mode.vdisplay << 16);
    if (r) {
      fprintf(stderr, "drmModeSetPlane failed: %s\n", strerror(errno));
    }

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

  free(temp_rgba);
  cleanup();
  return 0;
}