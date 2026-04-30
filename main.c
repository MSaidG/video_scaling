#include "gst/gstbin.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
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

#include <gst/allocators/gstdmabuf.h> // REQUIRED FOR DMA-BUF
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
typedef struct {
  uint32_t handle;
  uint32_t stride;
  uint32_t size;
  uint32_t fb_id;
  int prime_fd;
  EGLImageKHR egl_img;
  GLuint tex_id;
  GLuint fbo_id;
  void *cpu_map; // For debugging
} DumbBuffer;

typedef struct {
  GstElement *pipeline;
  GstElement *appsink;
  GstBus *bus;

  pthread_mutex_t lock;
  GstSample *new_sample;
  GstSample *active_sample; // ADDED: Buffer lifecycle management

  int width;
  int height;
  int is_new_frame_ready;
  int frame_count;

  GLuint tex_id;       // Replaces tex_y and tex_uv
  EGLImageKHR egl_img; // Holds current frame DMA-BUF mapping

  // Per-video performance stats
  struct {
    long total_upload_us;
    int frame_count;
    long min_us;
    long max_us;
  } perf;
} GstVid;

struct {
  int fd;
  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;
  uint32_t crtc_index;
  uint32_t plane_id;
  DumbBuffer bufs[2];
  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;
  GLuint prog;
  GLuint vbo;

  // Shader locations
  int stride;

  // Debug
  int frame_count;
  int show_test_pattern;

  // CRTC saved state for restoration
  drmModeCrtc *saved_crtc;

} kms;

typedef struct {
  long eos_check_us;
  long texture_upload_us[VIDEO_COUNT];
  long gl_draw_us[VIDEO_COUNT];
  long total_upload_us;
  long total_draw_us;
  long plane_set_us;
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

// --- GLOBALS ---
GstVid videos[VIDEO_COUNT];
volatile sig_atomic_t running = 1;
int enable_anim = 0;
int pip_mode = 0;

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

// UPDATED: Zero-Copy shader with debug logic and R/B swap preserved
const char *fs_src = "#extension GL_OES_EGL_image_external : require\n"
                     "precision mediump float;\n"
                     "varying vec2 v_tex;\n"
                     "uniform samplerExternalOES tex_ext;\n"
                     "uniform int debug_mode;\n"
                     "void main() {\n"
                     "    vec4 rgb = texture2D(tex_ext, v_tex);\n"
                     "    // Swap red and blue for BGR framebuffer\n"
                     "    gl_FragColor = vec4(rgb.b, rgb.g, rgb.r, 1.0);\n"
                     "}\n";

// --- HELPERS ---
void handle_sigint(int sig) { running = 0; }

void check_egl_error(const char *msg) {
  EGLint error = eglGetError();
  if (error != EGL_SUCCESS) {
    printf("EGL error at %s: 0x%x\n", msg, error);
  }
}

void check_gl_error(const char *msg) {
  GLenum error = glGetError();
  if (error != GL_NO_ERROR) {
    printf("GL error at %s: 0x%x\n", msg, error);
  }
}

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
    videos[i].perf.total_upload_us = 0;
    videos[i].perf.frame_count = 0;
  }
}

void print_perf_summary() {
  // Keeping your exact original debug logs
  printf("\n\n=== PERFORMANCE SUMMARY ===\n");
  printf("Frame timing (averaged over %d frames):\n", perf.frame_count);
  printf("  Total frame:    %5ld us (min: %ld, max: %ld)\n", perf.avg_frame_us,
         perf.min_frame_us, perf.max_frame_us);
  printf("  EOS check:      %5ld us\n", perf.avg_eos_us);
  printf("  Texture upload: %5ld us (min: %ld, max: %ld)\n", perf.avg_upload_us,
         perf.min_upload_us, perf.max_upload_us);
  printf("  GL drawing:     %5ld us (min: %ld, max: %ld)\n", perf.avg_draw_us,
         perf.min_draw_us, perf.max_draw_us);
  printf("  Plane set:      %5ld us (min: %ld, max: %ld)\n", perf.avg_plane_us,
         perf.min_plane_us, perf.max_plane_us);
  printf("===========================\n\n");
}

void print_second_stats(struct timespec second_start,
                        struct timespec second_end, int frames_this_second,
                        long total_eos_us, long total_upload_us,
                        long total_draw_us, long total_plane_us,
                        long upload_counts[VIDEO_COUNT],
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
    vid->frame_count++;
    pthread_mutex_unlock(&vid->lock);
    return GST_FLOW_OK;
  }
  return GST_FLOW_ERROR;
}

// ADDED: Allocation probe to fix memory alignment (green lines fix)
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

  if (!eglCreateImageKHR || !glEGLImageTargetTexture2DOES) {
    printf("Failed to load EGL extensions\n");
    return -1;
  }
  printf("EGL extensions loaded successfully\n");
  return 0;
}

uint32_t find_suitable_plane(int fd, uint32_t crtc_id, uint32_t crtc_index) {
  // [Unchanged: Kept your specific HDMI plane logic]
  drmModePlaneRes *plane_res = drmModeGetPlaneResources(fd);
  if (!plane_res) {
    printf("Failed to get plane resources\n");
    return 0;
  }

  printf("Searching for plane with crtc_index %u\n", crtc_index);

  for (uint32_t i = 0; i < plane_res->count_planes; i++) {
    drmModePlane *plane = drmModeGetPlane(fd, plane_res->planes[i]);
    if (!plane)
      continue;

    bool format_ok = false;
    for (uint32_t j = 0; j < plane->count_formats; j++) {
      if (plane->formats[j] == DRM_FORMAT_XRGB8888) {
        format_ok = true;
        break;
      }
    }

    if (format_ok && (plane->possible_crtcs & (1 << crtc_index))) {
      uint32_t plane_id = plane->plane_id;
      printf("Found suitable plane ID: %u (formats: %d)\n", plane_id,
             plane->count_formats);
      drmModeFreePlane(plane);
      drmModeFreePlaneResources(plane_res);
      return plane_id;
    }
    drmModeFreePlane(plane);
  }
  drmModeFreePlaneResources(plane_res);
  return 0;
}

int create_dumb_buffer_fbo(DumbBuffer *buf) {
  // [Unchanged: Kept your complex HDMI format fallbacks and drmModeAddFB2
  // logic]
  memset(buf, 0, sizeof(DumbBuffer));
  buf->prime_fd = -1;

  struct drm_mode_create_dumb create_req = {0};
  create_req.width = kms.mode.hdisplay;
  create_req.height = kms.mode.vdisplay;
  create_req.bpp = 32;
  create_req.flags = 0;

  if (ioctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0) {
    perror("DRM_IOCTL_MODE_CREATE_DUMB");
    return -1;
  }

  buf->handle = create_req.handle;
  buf->stride = create_req.pitch;
  buf->size = create_req.size;

  printf("Created dumb buffer: handle=%u, stride=%u, size=%u\n", buf->handle,
         buf->stride, buf->size);

  uint32_t formats_to_try[] = {
      DRM_FORMAT_XBGR8888, // XB24 in modetest
      DRM_FORMAT_BGRX8888,
      DRM_FORMAT_XRGB8888,
      DRM_FORMAT_ARGB8888,
  };

  const char *format_names[] = {
      "XBGR8888 (XB24)",
      "BGRX8888",
      "XRGB8888",
      "ARGB8888",
  };

  int fb_added = 0;
  for (int f = 0; f < 4; f++) {
    uint32_t handles[4] = {buf->handle};
    uint32_t pitches[4] = {buf->stride};
    uint32_t offsets[4] = {0};

    if (drmModeAddFB2(kms.fd, kms.mode.hdisplay, kms.mode.vdisplay,
                      formats_to_try[f], handles, pitches, offsets, &buf->fb_id,
                      0) == 0) {
      printf("Added FB with ID: %u using format %s\n", buf->fb_id,
             format_names[f]);
      fb_added = 1;
      break;
    }
  }

  if (!fb_added) {
    printf("Failed to add FB with any format\n");
    return -1;
  }

  struct drm_prime_handle prime = {0};
  prime.handle = buf->handle;
  prime.flags = DRM_CLOEXEC | DRM_RDWR;
  if (ioctl(kms.fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
    perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
    return -1;
  }
  buf->prime_fd = prime.fd;

  EGLint attribs[] = {EGL_WIDTH,
                      kms.mode.hdisplay,
                      EGL_HEIGHT,
                      kms.mode.vdisplay,
                      EGL_LINUX_DRM_FOURCC_EXT,
                      DRM_FORMAT_XBGR8888,
                      EGL_DMA_BUF_PLANE0_FD_EXT,
                      buf->prime_fd,
                      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                      0,
                      EGL_DMA_BUF_PLANE0_PITCH_EXT,
                      buf->stride,
                      EGL_NONE};

  printf("Creating EGLImage with: fourcc=XBGR8888, fd=%d, stride=%u\n",
         buf->prime_fd, buf->stride);
  buf->egl_img = eglCreateImageKHR(kms.egl_disp, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, NULL, attribs);

  if (!buf->egl_img) {
    EGLint error = eglGetError();
    printf("Failed to create EGLImage. EGL error: 0x%x\n", error);
    printf("Trying ARGB8888 instead...\n");
    EGLint attribs2[] = {EGL_WIDTH,
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
                                     EGL_LINUX_DMA_BUF_EXT, NULL, attribs2);
    if (!buf->egl_img) {
      error = eglGetError();
      printf("Still failed with ARGB8888. EGL error: 0x%x\n", error);
      return -1;
    }
  }

  printf("EGLImage created successfully\n");

  glGenTextures(1, &buf->tex_id);
  glBindTexture(GL_TEXTURE_2D, buf->tex_id);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, buf->egl_img);
  check_gl_error("glEGLImageTargetTexture2DOES");

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glGenFramebuffers(1, &buf->fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         buf->tex_id, 0);
  check_gl_error("glFramebufferTexture2D");

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    printf("FBO incomplete: 0x%x\n", status);
    return -1;
  }
  printf("FBO created successfully\n");

  return 0;
}

int init_gstreamer_pipeline(GstVid *vid, const char *filename, int index) {
  pthread_mutex_init(&vid->lock, NULL);
  vid->tex_id = 0;     // UPDATED
  vid->egl_img = NULL; // UPDATED
  vid->new_sample = NULL;
  vid->active_sample = NULL; // UPDATED
  vid->is_new_frame_ready = 0;
  vid->frame_count = 0;

  vid->perf.total_upload_us = 0;
  vid->perf.frame_count = 0;
  vid->perf.min_us = 999999;
  vid->perf.max_us = 0;

  char pipeline_str[512];
  // UPDATED: max-buffers=6 and drop=false to fix decoder pool starvation
  snprintf(
      pipeline_str, sizeof(pipeline_str),
      "filesrc location=%s ! qtdemux ! h264parse ! omxh264dec ! "
      "video/x-raw,format=NV12 ! appsink name=mysink%d sync=true drop=false "
      "max-buffers=1",
      filename, index);

  GError *err = NULL;
  vid->pipeline = gst_parse_launch(pipeline_str, &err);
  if (err) {
    fprintf(stderr, "GStreamer Error for %s: %s\n", filename, err->message);
    g_error_free(err);
    return -1;
  }

  char sink_name[32];
  snprintf(sink_name, sizeof(sink_name), "mysink%d", index);
  vid->appsink = gst_bin_get_by_name(GST_BIN(vid->pipeline), sink_name);

  if (!vid->appsink) {
    fprintf(stderr, "Failed to get appsink from pipeline %d\n", index);
    return -1;
  }

  // ADDED: Probe allocation attachment
  GstPad *sinkpad = gst_element_get_static_pad(vid->appsink, "sink");
  gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM,
                    allocation_probe_cb, NULL, NULL);
  gst_object_unref(sinkpad);

  GstAppSinkCallbacks callbacks = {0};
  callbacks.new_sample = on_new_sample;
  gst_app_sink_set_callbacks(GST_APP_SINK(vid->appsink), &callbacks, vid, NULL);

  gst_element_set_state(vid->pipeline, GST_STATE_PLAYING);
  vid->bus = gst_element_get_bus(vid->pipeline);

  printf("GStreamer pipeline %d initialized for %s\n", index, filename);

  return 0;
}

// UPDATED: Zero copy GPU function mapped over your CPU function
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

  // Read actual hardware strides from VideoMeta
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
    eglDestroyImageKHR(kms.egl_disp, vid->egl_img);
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

  vid->egl_img = eglCreateImageKHR(kms.egl_disp, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, NULL, attribs);

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
  check_gl_error("glEGLImageTargetTexture2DOES Update");

  // Buffer Lifecycle
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

  Rect rects[4];

  if (pip_mode) {
    // Video 0: Fullscreen background
    rects[0] = (Rect){-1.0f, -1.0f, 2.0f, 2.0f};
    
    // Video 1: PiP window in Top-Right
    // Width and Height are 0.6 (30% of the 2.0 total screen size).
    // Starting X and Y at 0.35f means it ends at 0.95f, 
    // leaving a 0.05f margin from the right and top edges.
    rects[1] = (Rect){0.35f, 0.35f, 0.6f, 0.6f};
    
    // Videos 2 & 3: Hidden (Zero dimension triangles are dropped by the GPU)
    rects[2] = (Rect){0.0f, 0.0f, 0.0f, 0.0f};
    rects[3] = (Rect){0.0f, 0.0f, 0.0f, 0.0f};
  } else {
    // Original Grid/Animation Logic
    float m = (step + 1) / 6.0f;
    rects[0] = (Rect){-1.0f, -1.0f, m, m};
    rects[1] = (Rect){0.0f, -1.0f, 1.0f, m};
    rects[2] = (Rect){-1.0f, 0.0f, m, 1.0f};
    rects[3] = (Rect){0.0f, 0.0f, 1.0f, 1.0f};
  }

  // The rest of the vertex mapping remains untouched
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
  print_perf_summary();

  if (kms.saved_crtc) {
    drmModeSetCrtc(kms.fd, kms.saved_crtc->crtc_id, kms.saved_crtc->buffer_id,
                   kms.saved_crtc->x, kms.saved_crtc->y,
                   &kms.connector->connector_id, 1, &kms.saved_crtc->mode);
    drmModeFreeCrtc(kms.saved_crtc);
  }

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
      gst_sample_unref(videos[i].active_sample); // UPDATED

    if (videos[i].tex_id)
      glDeleteTextures(1, &videos[i].tex_id); // UPDATED
    if (videos[i].egl_img)
      eglDestroyImageKHR(kms.egl_disp, videos[i].egl_img); // UPDATED

    pthread_mutex_destroy(&videos[i].lock);
  }

  if (kms.prog)
    glDeleteProgram(kms.prog);
  if (kms.vbo)
    glDeleteBuffers(1, &kms.vbo);

  for (int i = 0; i < 2; i++) {
    if (kms.bufs[i].cpu_map)
      munmap(kms.bufs[i].cpu_map, kms.bufs[i].size);
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

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data) {
    int *waiting_for_flip = (int *)data;
    *waiting_for_flip = 0;
}

int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);
  gst_init(&argc, &argv);

  printf("Arguments: 'all' (4x same video), '--anim=yes', '--anim=no', or '--pip'\n");
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "all") == 0) {
      for (int j = 0; j < VIDEO_COUNT; j++) {
        VIDEO_FILES[j] = "earth1.mp4";
      }
      printf("Mode: 'all'\n");
    } else if (strcmp(argv[i], "--anim=yes") == 0) {
      enable_anim = 1;
      printf("Animation: Enabled\n");
    } else if (strcmp(argv[i], "--anim=no") == 0) {
      enable_anim = 0;
      printf("Animation: Disabled (Static 2x2 grid)\n");
    } else if (strcmp(argv[i], "--pip") == 0) {
      pip_mode = 1; 
      printf("Mode: Picture-in-Picture\n");
    } else {
      printf("Unknown argument: %s\n", argv[i]);
    }
  }

  kms.fd = open("/dev/dri/card2", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0) {
    perror("Failed to open DRM device");
    return -1;
  }

  kms.saved_crtc = NULL;
  drmModeRes *res = drmModeGetResources(kms.fd);
  if (!res) {
    printf("Failed to get DRM resources\n");
    close(kms.fd);
    return -1;
  }

  kms.connector = NULL;
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *c = drmModeGetConnector(kms.fd, res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      kms.connector = c;
      printf("Found connector %d with %d modes\n", c->connector_id,
             c->count_modes);
      break;
    }
    if (c)
      drmModeFreeConnector(c);
  }

  if (!kms.connector) {
    printf("No connected connector found\n");
    drmModeFreeResources(res);
    close(kms.fd);
    return -1;
  }

  kms.mode = kms.connector->modes[0];
  printf("Using mode: %dx%d\n", kms.mode.hdisplay, kms.mode.vdisplay);

  kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[0]);
  if (!kms.crtc) {
    printf("Failed to get CRTC\n");
    drmModeFreeConnector(kms.connector);
    drmModeFreeResources(res);
    close(kms.fd);
    return -1;
  }

  kms.saved_crtc = drmModeGetCrtc(kms.fd, kms.crtc->crtc_id);

  kms.crtc_index = -1;
  for (int i = 0; i < res->count_crtcs; i++) {
    if (res->crtcs[i] == kms.crtc->crtc_id) {
      kms.crtc_index = i;
      break;
    }
  }

  printf("CRTC ID: %u, CRTC index: %d\n", kms.crtc->crtc_id, kms.crtc_index);

  if (kms.crtc_index >= 0) {
    kms.plane_id =
        find_suitable_plane(kms.fd, kms.crtc->crtc_id, kms.crtc_index);
  }

  if (!kms.plane_id) {
    printf("Warning: Could not find suitable plane, will use CRTC only\n");
    kms.plane_id = 0;
  } else {
    printf("Using plane ID: %u for page flipping\n", kms.plane_id);
  }

  drmModeFreeResources(res);
  kms.frame_count = 0;
  kms.show_test_pattern = 1;

  kms.egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (kms.egl_disp == EGL_NO_DISPLAY) {
    kms.egl_disp = eglGetDisplay((EGLNativeDisplayType)kms.fd);
  }

  if (!eglInitialize(kms.egl_disp, NULL, NULL)) {
    printf("Failed to initialize EGL display\n");
    cleanup();
    return -1;
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

  if (!eglChooseConfig(kms.egl_disp, attribs, &config, 1, &num)) {
    printf("Failed to choose EGL config\n");
    cleanup();
    return -1;
  }

  EGLint surf_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  kms.egl_surf = eglCreatePbufferSurface(kms.egl_disp, config, surf_attribs);

  EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT, ctx_attribs);

  if (!eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx)) {
    printf("Failed to make EGL context current\n");
    cleanup();
    return -1;
  }

  printf("EGL initialized successfully\n");

  if (load_egl_extensions() < 0) {
    cleanup();
    return -1;
  }

  if (create_dumb_buffer_fbo(&kms.bufs[0]) < 0) {
    printf("Failed to create buffer 0\n");
    cleanup();
    return -1;
  }
  if (create_dumb_buffer_fbo(&kms.bufs[1]) < 0) {
    printf("Failed to create buffer 1\n");
    cleanup();
    return -1;
  }

  printf("\nSetting CRTC with buffer 0...\n");
  if (drmModeSetCrtc(kms.fd, kms.crtc->crtc_id, kms.bufs[0].fb_id, 0, 0,
                     &kms.connector->connector_id, 1, &kms.mode) < 0) {
    perror("drmModeSetCrtc");
    cleanup();
    return -1;
  }

  printf("\nInitializing GStreamer pipelines...\n");
  for (int i = 0; i < VIDEO_COUNT; i++) {
    if (init_gstreamer_pipeline(&videos[i], VIDEO_FILES[i], i) < 0) {
      cleanup();
      return -1;
    }
  }

  kms.prog = glCreateProgram();
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_src, NULL);
  glCompileShader(vs);

  GLint compiled;
  glGetShaderiv(vs, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    char log[256];
    glGetShaderInfoLog(vs, sizeof(log), NULL, log);
    printf("Vertex shader compilation failed: %s\n", log);
  }

  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_src, NULL);
  glCompileShader(fs);

  glGetShaderiv(fs, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    char log[256];
    glGetShaderInfoLog(fs, sizeof(log), NULL, log);
    printf("Fragment shader compilation failed: %s\n", log);
  }

  glAttachShader(kms.prog, vs);
  glAttachShader(kms.prog, fs);
  glLinkProgram(kms.prog);

  GLint linked;
  glGetProgramiv(kms.prog, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[256];
    glGetProgramInfoLog(kms.prog, sizeof(log), NULL, log);
    printf("Program linking failed: %s\n", log);
  }

  glUseProgram(kms.prog);

  glGenBuffers(1, &kms.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, kms.vbo);
  glBufferData(GL_ARRAY_BUFFER, 4 * 6 * 4 * sizeof(float), NULL,
               GL_DYNAMIC_DRAW);

  // UPDATED: Bind the single external texture uniform and debug_mode
  glUniform1i(glGetUniformLocation(kms.prog, "tex_ext"), 0);
  GLint debug_loc = glGetUniformLocation(kms.prog, "debug_mode");

  // Step 5 results in m=1.0f (a perfect 2x2 grid without overlap)
  int current_anim_step = enable_anim ? 0 : 5;
  update_geometry(current_anim_step);

  GLint loc_pos = glGetAttribLocation(kms.prog, "a_pos");
  GLint loc_tex = glGetAttribLocation(kms.prog, "a_tex");
  int stride = 4 * sizeof(float);
  glEnableVertexAttribArray(loc_pos);
  glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
  glEnableVertexAttribArray(loc_tex);
  glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, stride,
                        (void *)(2 * sizeof(float)));

  int back_buf = 1;
  struct timespec anim_t0, anim_t1;
  printf("\nStarting main loop...\n");
  printf("First 60 frames will show test pattern, then videos\n");

  clock_gettime(CLOCK_MONOTONIC, &anim_t0);
  const double ANIM_STEP_SEC = 2.0;

  for (int i = 0; i < 2; i++) {
    glBindFramebuffer(GL_FRAMEBUFFER, kms.bufs[i].fbo_id);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  
  glFinish();

  init_perf_stats();
  struct timespec second_start, second_end, frame_start, eos_done, upload_done,
      draw_done, plane_done;
  clock_gettime(CLOCK_MONOTONIC, &second_start);

  int frames_this_second = 0;
  long second_total_eos = 0, second_total_upload = 0, second_total_draw = 0,
       second_total_plane = 0;
  long second_upload_counts[VIDEO_COUNT] = {0},
       second_draw_counts[VIDEO_COUNT] = {0};

  while (running) {
    clock_gettime(CLOCK_MONOTONIC, &frame_start);

    for (int i = 0; i < VIDEO_COUNT; i++) {
      perf.texture_upload_us[i] = 0;
      perf.gl_draw_us[i] = 0;
    }

    if (enable_anim) {
      clock_gettime(CLOCK_MONOTONIC, &anim_t1);
      double elapsed = (anim_t1.tv_sec - anim_t0.tv_sec) +
                       (anim_t1.tv_nsec - anim_t0.tv_nsec) / 1e9;
      if (elapsed >= ANIM_STEP_SEC) {
        current_anim_step = (current_anim_step + 1) % 6;
        update_geometry(current_anim_step);
        anim_t0 = anim_t1;
      }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, kms.bufs[back_buf].fbo_id);
    glViewport(0, 0, kms.mode.hdisplay, kms.mode.vdisplay);

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

    // UPDATED: Zero Copy Draw Phase
    for (int i = 0; i < VIDEO_COUNT; i++) {
      struct timespec draw_start, draw_end;
      clock_gettime(CLOCK_MONOTONIC, &draw_start);

      // Only draw video if debug pattern is off or if texture is bound
      if (videos[i].tex_id) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, videos[i].tex_id);
        glDrawArrays(GL_TRIANGLES, i * 6, 6);
      }

      clock_gettime(CLOCK_MONOTONIC, &draw_end);
      perf.gl_draw_us[i] = get_diff_us(draw_start, draw_end);
    }
    clock_gettime(CLOCK_MONOTONIC, &draw_done);

    // UPDATED: Synchronize GPU to prevent overwriting
    glFinish();

    // --- NEW HDMI VSYNC LOGIC ---
    int waiting_for_flip = 1;
    drmEventContext evctx = {0};
    evctx.version = 2;
    evctx.page_flip_handler = page_flip_handler;

    drmModeSetPlane(kms.fd, kms.plane_id, kms.crtc->crtc_id,
                    kms.bufs[back_buf].fb_id, 0, 0, 0, kms.mode.hdisplay,
                    kms.mode.vdisplay, 0, 0, kms.mode.hdisplay << 16,
                    kms.mode.vdisplay << 16);
                    
    drmModePageFlip(kms.fd, kms.crtc->crtc_id, kms.bufs[back_buf].fb_id,
                    DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);

    // Wait for the hardware VSYNC interrupt cleanly using select()
    fd_set fds;
    while (waiting_for_flip && running) {
        FD_ZERO(&fds);
        FD_SET(kms.fd, &fds);
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 100000 }; // 100ms timeout
        int ret = select(kms.fd + 1, &fds, NULL, NULL, &timeout);
        if (ret > 0) {
            drmHandleEvent(kms.fd, &evctx); // This triggers page_flip_handler
        } else {
            break; // Timeout, prevents freezing if driver drops the event
        }
    }
    // ----------------------------

    clock_gettime(CLOCK_MONOTONIC, &plane_done);

    back_buf = !back_buf;
    
    clock_gettime(CLOCK_MONOTONIC, &plane_done);

    back_buf = !back_buf;
    kms.frame_count++;
    perf.frame_count++;
    frames_this_second++;

    // ... [Kept exact same performance stat updating logic]
    second_total_eos += get_diff_us(frame_start, eos_done);
    second_total_upload += get_diff_us(eos_done, upload_done);
    second_total_draw += get_diff_us(upload_done, draw_done);
    second_total_plane += get_diff_us(draw_done, plane_done);

    for (int i = 0; i < VIDEO_COUNT; i++) {
      second_upload_counts[i] += perf.texture_upload_us[i];
      second_draw_counts[i] += (perf.gl_draw_us[i] > 0) ? 1 : 0;
    }

    clock_gettime(CLOCK_MONOTONIC, &second_end);
    if (get_diff_us(second_start, second_end) >= 1000000) {
      print_second_stats(second_start, second_end, frames_this_second,
                         second_total_eos, second_total_upload,
                         second_total_draw, second_total_plane,
                         second_upload_counts, second_draw_counts);

      perf.avg_frame_us =
          (perf.avg_frame_us * (perf.frame_count - frames_this_second) +
           (second_total_eos + second_total_upload + second_total_draw +
            second_total_plane)) /
          perf.frame_count;
      perf.avg_eos_us =
          (perf.avg_eos_us * (perf.frame_count - frames_this_second) +
           second_total_eos) /
          perf.frame_count;
      perf.avg_upload_us =
          (perf.avg_upload_us * (perf.frame_count - frames_this_second) +
           second_total_upload) /
          perf.frame_count;
      perf.avg_draw_us =
          (perf.avg_draw_us * (perf.frame_count - frames_this_second) +
           second_total_draw) /
          perf.frame_count;
      perf.avg_plane_us =
          (perf.avg_plane_us * (perf.frame_count - frames_this_second) +
           second_total_plane) /
          perf.frame_count;

      clock_gettime(CLOCK_MONOTONIC, &second_start);
      frames_this_second = 0;
      second_total_eos = 0;
      second_total_upload = 0;
      second_total_draw = 0;
      second_total_plane = 0;
      memset(second_upload_counts, 0, sizeof(second_upload_counts));
      memset(second_draw_counts, 0, sizeof(second_draw_counts));
    }
  }

  cleanup();
  return 0;
}
