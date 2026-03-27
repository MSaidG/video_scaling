#include "gst/gstbin.h"
#include <fcntl.h>
#include <linux/fb.h>
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
#include <drm_fourcc.h> // Still needed for DRM_FORMAT_NV12 in DMA-BUF import

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

// --- CONFIG ---
#define VIDEO_COUNT 4
char *VIDEO_FILES[VIDEO_COUNT] = {"earth1.mp4", "zoo.mp4", "sea.mp4",
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
typedef struct fbdev_window {
  unsigned short width;
  unsigned short height;
} fbdev_window;

typedef struct {
  GstElement *pipeline;
  GstElement *appsink;
  GstBus *bus;

  pthread_mutex_t lock;
  GstSample *new_sample;
  GstSample *active_sample;

  int width;
  int height;
  int is_new_frame_ready;

  GLuint tex_id;
  EGLImageKHR egl_img;

  struct {
    long total_upload_us;
    int frame_count;
    long min_us;
    long max_us;
  } perf;
} GstVid;

// Replaced kms struct with fb_state
struct {
  int fd;
  struct fb_var_screeninfo vinfo;
  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;
  GLuint prog;
  GLuint vbo;
} fb_state;

typedef struct {
  long frame_start_us;
  long eos_check_us;
  long texture_upload_us[VIDEO_COUNT];
  long gl_draw_us[VIDEO_COUNT];
  long total_upload_us;
  long total_draw_us;
  long plane_set_us; // Kept for struct compatibility, represents swap time now
  long total_frame_us;
  int frame_count;
  int video_upload_counts[VIDEO_COUNT];
  long avg_frame_us;
  long avg_upload_us;
  long avg_draw_us;
  long avg_plane_us;
  long avg_eos_us;
  long min_frame_us;
  long max_frame_us;
  long min_upload_us;
  long max_upload_us;
  long min_draw_us;
  long max_draw_us;
  long min_plane_us;
  long max_plane_us;
} PerfStats;

PerfStats perf = {0};
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

const char *fs_src = "#extension GL_OES_EGL_image_external : require\n"
                     "precision mediump float;\n"
                     "varying vec2 v_tex;\n"
                     "uniform samplerExternalOES tex_ext;\n"
                     "void main() {\n"
                     "  gl_FragColor = texture2D(tex_ext, v_tex);\n"
                     "}\n";

// --- HELPERS ---
void handle_sigint(int sig) { running = 0; }

long get_diff_us(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000000 +
         (end.tv_nsec - start.tv_nsec) / 1000;
}

void init_perf_stats() {
  perf.min_frame_us = 999999;
  perf.max_frame_us = 0;
  perf.min_upload_us = 999999;
  perf.max_upload_us = 0;
  perf.min_draw_us = 999999;
  perf.max_draw_us = 0;
  perf.min_plane_us = 999999;
  perf.max_plane_us = 0;

  for (int i = 0; i < VIDEO_COUNT; i++) {
    videos[i].perf.min_us = 999999;
    videos[i].perf.max_us = 0;
  }
}

void print_perf_summary() {
  printf("\n\n=== PERFORMANCE SUMMARY ===\n");
  printf("Frame timing (averaged over %d frames):\n", perf.frame_count);
  long avg_frame_us = perf.frame_count > 0 ? perf.avg_frame_us : 0;
  long avg_eos_us = perf.frame_count > 0 ? perf.avg_eos_us : 0;
  long avg_upload_us = perf.frame_count > 0 ? perf.avg_upload_us : 0;
  long avg_draw_us = perf.frame_count > 0 ? perf.avg_draw_us : 0;
  long avg_plane_us = perf.frame_count > 0 ? perf.avg_plane_us : 0;
  printf("  Total frame:    %5ld us (min: %ld, max: %ld)\n", avg_frame_us,
         perf.min_frame_us < 999999 ? perf.min_frame_us : 0, perf.max_frame_us);
  printf("  Texture upload: %5ld us (min: %ld, max: %ld)\n", avg_upload_us,
         perf.min_upload_us < 999999 ? perf.min_upload_us : 0,
         perf.max_upload_us);
  printf("  GL drawing:     %5ld us (min: %ld, max: %ld)\n", avg_draw_us,
         perf.min_draw_us < 999999 ? perf.min_draw_us : 0, perf.max_draw_us);
  printf("===========================\n\n");
}

void print_second_stats(struct timespec second_start,
                        struct timespec second_end, int frames_this_second,
                        long total_upload_us, long total_draw_us,
                        long total_plane_us, long upload_counts[VIDEO_COUNT],
                        long draw_counts[VIDEO_COUNT]) {
  long second_duration = get_diff_us(second_start, second_end);
  float fps = (frames_this_second * 1000000.0f) / second_duration;
  printf("\n=== Stats for last 1 second ===\n");
  printf("Frames: %d  |  FPS: %.1f\n", frames_this_second, fps);
  printf("================================\n");
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

static GstPadProbeReturn allocation_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                             gpointer user_data) {
  GstQuery *query = GST_PAD_PROBE_INFO_QUERY(info);
  if (GST_QUERY_TYPE(query) == GST_QUERY_ALLOCATION) {
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
  }
  return GST_PAD_PROBE_OK;
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

int init_gstreamer_pipeline(GstVid *vid, const char *filename) {
  pthread_mutex_init(&vid->lock, NULL);
  vid->tex_id = 0;
  vid->egl_img = NULL;
  vid->new_sample = NULL;
  vid->active_sample = NULL;
  vid->is_new_frame_ready = 0;

  vid->perf.total_upload_us = 0;
  vid->perf.frame_count = 0;
  vid->perf.min_us = 999999;
  vid->perf.max_us = 0;

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

  GstPad *sinkpad = gst_element_get_static_pad(vid->appsink, "sink");
  gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                    allocation_probe_cb, NULL, NULL);
  gst_object_unref(sinkpad);

  GstAppSinkCallbacks callbacks = {0};
  callbacks.new_sample = on_new_sample;
  gst_app_sink_set_callbacks(GST_APP_SINK(vid->appsink), &callbacks, vid, NULL);

  gst_element_set_state(vid->pipeline, GST_STATE_PLAYING);
  vid->bus = gst_element_get_bus(vid->pipeline);

  return 0;
}

void update_texture_gpu(GstVid *vid, int video_idx) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

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

  GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
  if (!gst_is_dmabuf_memory(mem)) {
    fprintf(stderr, "Error: GStreamer buffer is NOT a DMA-BUF memory block.\n");
    gst_sample_unref(sample);
    return;
  }
  int fd = gst_dmabuf_memory_get_fd(mem);

  int pitch, uv_offset;
  GstVideoMeta *vmeta = gst_buffer_get_video_meta(buffer);

  if (vmeta) {
    pitch = vmeta->stride[0];
    uv_offset = vmeta->offset[1];
  } else {
    pitch = GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 0);
    uv_offset = GST_VIDEO_INFO_PLANE_OFFSET(&vinfo, 1);
  }

  if (vid->egl_img) {
    eglDestroyImageKHR(fb_state.egl_disp, vid->egl_img);
  }

  EGLint attribs[] = {EGL_WIDTH,
                      vid->width,
                      EGL_HEIGHT,
                      vid->height,
                      EGL_LINUX_DRM_FOURCC_EXT,
                      DRM_FORMAT_NV12,
                      EGL_DMA_BUF_PLANE0_FD_EXT,
                      fd,
                      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                      0,
                      EGL_DMA_BUF_PLANE0_PITCH_EXT,
                      pitch,
                      EGL_DMA_BUF_PLANE1_FD_EXT,
                      fd,
                      EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                      uv_offset,
                      EGL_DMA_BUF_PLANE1_PITCH_EXT,
                      pitch,
                      EGL_NONE};

  vid->egl_img = eglCreateImageKHR(fb_state.egl_disp, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, NULL, attribs);

  // ADD THIS CHECK:
  if (vid->egl_img == EGL_NO_IMAGE_KHR) {
    EGLint err = eglGetError();
    fprintf(
        stderr,
        "EGL Error: Failed to import DMA-BUF on video %d (Error code: 0x%X)\n",
        video_idx, err);
  }

  if (!vid->tex_id) {
    glGenTextures(1, &vid->tex_id);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, vid->tex_id);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,
                    GL_CLAMP_TO_EDGE);
  }

  glBindTexture(GL_TEXTURE_EXTERNAL_OES, vid->tex_id);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES,
                               (GLeglImageOES)vid->egl_img);

  if (vid->active_sample) {
    gst_sample_unref(vid->active_sample);
  }
  vid->active_sample = sample;

  clock_gettime(CLOCK_MONOTONIC, &t1);
  long upload_us = get_diff_us(t0, t1);
  vid->perf.total_upload_us += upload_us;
  vid->perf.frame_count++;
  if (upload_us < vid->perf.min_us)
    vid->perf.min_us = upload_us;
  if (upload_us > vid->perf.max_us)
    vid->perf.max_us = upload_us;
  perf.texture_upload_us[video_idx] = upload_us;
  perf.video_upload_counts[video_idx]++;
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
  glBindBuffer(GL_ARRAY_BUFFER, fb_state.vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
}

void cleanup() {
  printf("\n--- Cleaning Up ---\n");
  print_perf_summary();

  for (int i = 0; i < VIDEO_COUNT; i++) {
    if (videos[i].pipeline) {
      gst_element_set_state(videos[i].pipeline, GST_STATE_NULL);
      gst_object_unref(videos[i].pipeline);
    }
    if (videos[i].bus)
      gst_object_unref(videos[i].bus);
    if (videos[i].new_sample)
      gst_sample_unref(videos[i].new_sample);
    if (videos[i].active_sample)
      gst_sample_unref(videos[i].active_sample);

    if (videos[i].tex_id)
      glDeleteTextures(1, &videos[i].tex_id);
    if (videos[i].egl_img)
      eglDestroyImageKHR(fb_state.egl_disp, videos[i].egl_img);

    pthread_mutex_destroy(&videos[i].lock);
  }

  if (fb_state.prog)
    glDeleteProgram(fb_state.prog);
  if (fb_state.vbo)
    glDeleteBuffers(1, &fb_state.vbo);

  if (fb_state.egl_disp != EGL_NO_DISPLAY) {
    eglMakeCurrent(fb_state.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    const char *exts = eglQueryString(fb_state.egl_disp, EGL_EXTENSIONS);
    printf("EGL Extensions: %s\n", exts);
    if (!strstr(exts, "EGL_EXT_image_dma_buf_import")) {
      printf("\nCRITICAL WARNING: The Mali fbdev driver does NOT support "
             "DMA-BUF import.\n");
    }
    if (fb_state.egl_surf != EGL_NO_SURFACE) {
      eglDestroySurface(fb_state.egl_disp, fb_state.egl_surf);
    }
    if (fb_state.egl_ctx != EGL_NO_CONTEXT) {
      eglDestroyContext(fb_state.egl_disp, fb_state.egl_ctx);
    }
    eglTerminate(fb_state.egl_disp);
  }

  if (fb_state.fd >= 0)
    close(fb_state.fd);

  printf("Done.\n");
}

int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);
  gst_init(&argc, &argv);

  if (argc > 1) {
    if (strcmp(argv[1], "all") == 0) {
      for (int i = 0; i < VIDEO_COUNT; i++) {
        VIDEO_FILES[i] = "earth1.mp4";
      }
      printf("Variable changed to 1 (all mode)\n");
    } else {
      printf("Unknown argument: %s\n", argv[1]);
    }
  }

  // --- 1. OPEN FRAMEBUFFER ---
  fb_state.fd = open("/dev/fb0", O_RDWR);
  if (fb_state.fd < 0) {
    perror("Error opening /dev/fb0");
    return -1;
  }

  if (ioctl(fb_state.fd, FBIOGET_VSCREENINFO, &fb_state.vinfo) == -1) {
    perror("Error reading variable information");
    close(fb_state.fd);
    return -1;
  }

  // --- 2. SETUP EGL ---
  fb_state.egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  eglInitialize(fb_state.egl_disp, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);

  EGLConfig config;
  EGLint num;
  EGLint attribs[] = {EGL_SURFACE_TYPE,
                      EGL_WINDOW_BIT,
                      EGL_RED_SIZE,
                      8,
                      EGL_GREEN_SIZE,
                      8,
                      EGL_BLUE_SIZE,
                      8,
                      EGL_RENDERABLE_TYPE,
                      EGL_OPENGL_ES2_BIT,
                      EGL_NONE};
  eglChooseConfig(fb_state.egl_disp, attribs, &config, 1, &num);

  static fbdev_window native_window;
  native_window.width = fb_state.vinfo.xres;
  native_window.height = fb_state.vinfo.yres;

  fb_state.egl_surf = eglCreateWindowSurface(
      fb_state.egl_disp, config, (EGLNativeWindowType)&native_window, NULL);

  fb_state.egl_ctx =
      eglCreateContext(fb_state.egl_disp, config, EGL_NO_CONTEXT,
                       (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});

  eglMakeCurrent(fb_state.egl_disp, fb_state.egl_surf, fb_state.egl_surf,
                 fb_state.egl_ctx);
  load_egl_extensions();

  // --- 3. INIT GSTREAMER PIPELINES ---
  for (int i = 0; i < VIDEO_COUNT; i++) {
    if (init_gstreamer_pipeline(&videos[i], VIDEO_FILES[i]) < 0) {
      cleanup();
      return -1;
    }
  }

  // --- 4. SETUP OPENGL SHADERS ---
  fb_state.prog = glCreateProgram();
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_src, NULL);
  glCompileShader(vs);
  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_src, NULL);
  glCompileShader(fs);
  glAttachShader(fb_state.prog, vs);
  glAttachShader(fb_state.prog, fs);
  glLinkProgram(fb_state.prog);
  glUseProgram(fb_state.prog);

  glGenBuffers(1, &fb_state.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, fb_state.vbo);
  glBufferData(GL_ARRAY_BUFFER, 4 * 6 * 4 * sizeof(float), NULL,
               GL_DYNAMIC_DRAW);

  int current_anim_step = 0;
  update_geometry(current_anim_step);

  GLint loc_pos = glGetAttribLocation(fb_state.prog, "a_pos");
  GLint loc_tex = glGetAttribLocation(fb_state.prog, "a_tex");
  int stride = 4 * sizeof(float);
  glEnableVertexAttribArray(loc_pos);
  glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
  glEnableVertexAttribArray(loc_tex);
  glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, stride,
                        (void *)(2 * sizeof(float)));

  glUniform1i(glGetUniformLocation(fb_state.prog, "tex_ext"), 0);

  init_perf_stats();

  printf(
      "Running 4x Zero-Copy DMA-BUF Video on fbdev... Press Ctrl+C to exit.\n");

  struct timespec anim_t0, anim_t1;
  clock_gettime(CLOCK_MONOTONIC, &anim_t0);
  const double ANIM_STEP_SEC = 2.0;

  struct timespec second_start, second_end, frame_start, eos_done, upload_done,
      draw_done, swap_done;
  clock_gettime(CLOCK_MONOTONIC, &second_start);

  int frames_this_second = 0;
  long second_total_eos = 0, second_total_upload = 0, second_total_draw = 0,
       second_total_swap = 0;
  long second_upload_counts[VIDEO_COUNT] = {0},
       second_draw_counts[VIDEO_COUNT] = {0};

  while (running) {
    clock_gettime(CLOCK_MONOTONIC, &frame_start);

    for (int i = 0; i < VIDEO_COUNT; i++) {
      perf.texture_upload_us[i] = 0;
      perf.gl_draw_us[i] = 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &anim_t1);
    double elapsed = (anim_t1.tv_sec - anim_t0.tv_sec) +
                     (anim_t1.tv_nsec - anim_t0.tv_nsec) / 1e9;
    if (elapsed >= ANIM_STEP_SEC) {
      current_anim_step = (current_anim_step + 1) % 6;
      update_geometry(current_anim_step);
      anim_t0 = anim_t1;
    }

    // Bind to default window surface instead of FBO
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fb_state.vinfo.xres, fb_state.vinfo.yres);

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
    }
    clock_gettime(CLOCK_MONOTONIC, &eos_done);

    for (int i = 0; i < VIDEO_COUNT; i++) {
      struct timespec upload_start, upload_end;
      clock_gettime(CLOCK_MONOTONIC, &upload_start);
      update_texture_gpu(&videos[i], i);
      clock_gettime(CLOCK_MONOTONIC, &upload_end);
      perf.texture_upload_us[i] = get_diff_us(upload_start, upload_end);
    }
    clock_gettime(CLOCK_MONOTONIC, &upload_done);

    // ZERO COPY DRAW PHASE
    for (int i = 0; i < VIDEO_COUNT; i++) {
      struct timespec draw_start, draw_end;
      clock_gettime(CLOCK_MONOTONIC, &draw_start);

      if (videos[i].tex_id) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, videos[i].tex_id);
        glDrawArrays(GL_TRIANGLES, i * 6, 6);
      }

      clock_gettime(CLOCK_MONOTONIC, &draw_end);
      perf.gl_draw_us[i] = get_diff_us(draw_start, draw_end);
    }
    clock_gettime(CLOCK_MONOTONIC, &draw_done);

    // Swap EGL buffers instead of DRM plane flip
    eglSwapBuffers(fb_state.egl_disp, fb_state.egl_surf);
    clock_gettime(CLOCK_MONOTONIC, &swap_done);

    perf.frame_count++;
    frames_this_second++;
    second_total_eos += get_diff_us(frame_start, eos_done);
    second_total_upload += get_diff_us(eos_done, upload_done);
    second_total_draw += get_diff_us(upload_done, draw_done);
    second_total_swap += get_diff_us(draw_done, swap_done);

    for (int i = 0; i < VIDEO_COUNT; i++) {
      second_upload_counts[i] += perf.texture_upload_us[i];
      second_draw_counts[i] += (perf.gl_draw_us[i] > 0) ? 1 : 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &second_end);
    if (get_diff_us(second_start, second_end) >= 1000000) {
      print_second_stats(second_start, second_end, frames_this_second,
                         second_total_upload, second_total_draw,
                         second_total_swap, second_upload_counts,
                         second_draw_counts);

      clock_gettime(CLOCK_MONOTONIC, &second_start);
      frames_this_second = 0;
      second_total_eos = 0;
      second_total_upload = 0;
      second_total_draw = 0;
      second_total_swap = 0;
      memset(second_upload_counts, 0, sizeof(second_upload_counts));
      memset(second_draw_counts, 0, sizeof(second_draw_counts));
    }
  }

  cleanup();
  return 0;
}