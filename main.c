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
  void *cpu_map; // For debugging
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
  int frame_count;

  GLuint tex_y;
  GLuint tex_uv;

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
  // Frame timing breakdown
  long eos_check_us;
  long texture_upload_us[VIDEO_COUNT];
  long gl_draw_us[VIDEO_COUNT];
  long total_upload_us;
  long total_draw_us;
  long plane_set_us;
  long total_frame_us;

  // Per-frame tracking
  int frame_count;
  int video_upload_counts[VIDEO_COUNT];

  // Rolling averages
  long avg_frame_us;
  long avg_upload_us;
  long avg_draw_us;
  long avg_plane_us;
  long avg_eos_us;

  // Min/Max tracking
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
                     "uniform int debug_mode;\n"
                     "vec3 yuv2rgb(float y, float u, float v) {\n"
                     "  float r = y + 1.402 * v;\n"
                     "  float g = y - 0.344 * u - 0.714 * v;\n"
                     "  float b = y + 1.772 * u;\n"
                     "  return vec3(r, g, b);\n"
                     "}\n"
                     "void main() {\n"
                     "  if (debug_mode == 1) {\n"
                     "    // Test pattern - color bars\n"
                     "    if (v_tex.x < 0.25) {\n"
                     "      gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
                     "    } else if (v_tex.x < 0.5) {\n"
                     "      gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n"
                     "    } else if (v_tex.x < 0.75) {\n"
                     "      gl_FragColor = vec4(0.0, 0.0, 1.0, 1.0);\n"
                     "    } else {\n"
                     "      gl_FragColor = vec4(1.0, 1.0, 1.0, 1.0);\n"
                     "    }\n"
                     "  } else {\n"
                     "    float y = texture2D(tex_y, v_tex).r;\n"
                     "    vec4 uv = texture2D(tex_uv, v_tex);\n"
                     "    float u = uv.r - 0.5;\n"
                     "    float v = uv.a - 0.5;\n"
                     "    vec3 rgb = yuv2rgb(y, u, v);\n"
                     "    // Swap red and blue for BGR framebuffer\n"
                     "    gl_FragColor = vec4(rgb.b, rgb.g, rgb.r, 1.0);\n"
                     "  }\n"
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

  printf("\nPer-video texture upload times:\n");
  for (int i = 0; i < VIDEO_COUNT; i++) {
    if (videos[i].perf.frame_count > 0) {
      long avg = videos[i].perf.total_upload_us / videos[i].perf.frame_count;
      printf("  Video %d: avg=%5ld us, min=%ld, max=%ld (frames: %d)\n", i, avg,
             videos[i].perf.min_us, videos[i].perf.max_us,
             videos[i].perf.frame_count);
    } else {
      printf("  Video %d: no frames processed\n", i);
    }
  }

  printf("\nBreakdown by operation:\n");
  printf("  EOS check:      %.1f%% of frame time\n",
         (perf.avg_eos_us * 100.0) / perf.avg_frame_us);
  printf("  Texture upload: %.1f%% of frame time\n",
         (perf.avg_upload_us * 100.0) / perf.avg_frame_us);
  printf("  GL drawing:     %.1f%% of frame time\n",
         (perf.avg_draw_us * 100.0) / perf.avg_frame_us);
  printf("  Plane set:      %.1f%% of frame time\n",
         (perf.avg_plane_us * 100.0) / perf.avg_frame_us);
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
  printf("Frame time breakdown (avg per frame):\n");
  printf("  EOS Check:      %5ld us\n",
         frames_this_second ? total_eos_us / frames_this_second : 0);
  printf("  Texture Upload: %5ld us\n",
         frames_this_second ? total_upload_us / frames_this_second : 0);
  printf("  GL Drawing:     %5ld us\n",
         frames_this_second ? total_draw_us / frames_this_second : 0);
  printf("  Plane Set:      %5ld us\n",
         frames_this_second ? total_plane_us / frames_this_second : 0);

  printf("\nPer-video uploads (avg per frame):\n");
  for (int i = 0; i < VIDEO_COUNT; i++) {
    if (upload_counts[i] > 0) {
      printf("  Video %d: %5ld us (drawn %ld times)\n", i,
             upload_counts[i] / frames_this_second, draw_counts[i]);
    }
  }
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

    // Check if plane supports our format
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

  // Map for CPU access (debugging)
  struct drm_mode_map_dumb map_req = {.handle = buf->handle};
  if (ioctl(kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) == 0) {
    // buf->cpu_map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED,
    // kms.fd, map_req.offset);
    // if (buf->cpu_map == MAP_FAILED) {
    // buf->cpu_map = NULL;
    // }
  }

  // Try different formats - from your modetest output, the plane supports:
  // XB24 (DRM_FORMAT_XBGR8888), XB30, XVUY, VU24, XV30, NV16, NV12, etc.
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

  // Export prime fd
  struct drm_prime_handle prime = {0};
  prime.handle = buf->handle;
  prime.flags = DRM_CLOEXEC | DRM_RDWR;
  if (ioctl(kms.fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) < 0) {
    perror("DRM_IOCTL_PRIME_HANDLE_TO_FD");
    return -1;
  }
  buf->prime_fd = prime.fd;

  // Create EGL image - must match the format used in drmModeAddFB2
  // For XBGR8888, the fourcc code is DRM_FORMAT_XBGR8888
  EGLint attribs[] = {EGL_WIDTH,
                      kms.mode.hdisplay,
                      EGL_HEIGHT,
                      kms.mode.vdisplay,
                      EGL_LINUX_DRM_FOURCC_EXT,
                      DRM_FORMAT_XBGR8888, // Match the format used above
                      EGL_DMA_BUF_PLANE0_FD_EXT,
                      buf->prime_fd,
                      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                      0,
                      EGL_DMA_BUF_PLANE0_PITCH_EXT,
                      buf->stride,
                      EGL_NONE};

  // For debugging, print the attributes
  printf("Creating EGLImage with: fourcc=XBGR8888, fd=%d, stride=%u\n",
         buf->prime_fd, buf->stride);

  buf->egl_img = eglCreateImageKHR(kms.egl_disp, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, NULL, attribs);

  if (!buf->egl_img) {
    EGLint error = eglGetError();
    printf("Failed to create EGLImage. EGL error: 0x%x\n", error);

    // Try alternative fourcc
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

  // Create texture from EGLImage
  glGenTextures(1, &buf->tex_id);
  glBindTexture(GL_TEXTURE_2D, buf->tex_id);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, buf->egl_img);
  check_gl_error("glEGLImageTargetTexture2DOES");

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  // Create FBO
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
  vid->tex_y = 0;
  vid->tex_uv = 0;
  vid->new_sample = NULL;
  vid->is_new_frame_ready = 0;
  vid->frame_count = 0;

  // Initialize per-video perf stats
  vid->perf.total_upload_us = 0;
  vid->perf.frame_count = 0;
  vid->perf.min_us = 999999;
  vid->perf.max_us = 0;

  char pipeline_str[512];
  snprintf(
      pipeline_str, sizeof(pipeline_str),
      "filesrc location=%s ! qtdemux ! h264parse ! omxh264dec ! "
      "video/x-raw,format=NV12 ! appsink name=mysink%d sync=true drop=true "
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

  GstAppSinkCallbacks callbacks = {0};
  callbacks.new_sample = on_new_sample;
  gst_app_sink_set_callbacks(GST_APP_SINK(vid->appsink), &callbacks, vid, NULL);

  gst_element_set_state(vid->pipeline, GST_STATE_PLAYING);
  vid->bus = gst_element_get_bus(vid->pipeline);

  printf("GStreamer pipeline %d initialized for %s\n", index, filename);

  return 0;
}

void update_texture_cpu(GstVid *vid, int video_idx) {
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
  check_gl_error("Y texture update");

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, vid->tex_uv);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, pitch / 2, vid->height / 2,
               0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, map.data + uv_offset);
  check_gl_error("UV texture update");

  gst_buffer_unmap(buffer, &map);
  gst_sample_unref(sample);

  // Update per-video performance stats
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long upload_us = get_diff_us(t0, t1);

  vid->perf.total_upload_us += upload_us;
  vid->perf.frame_count++;
  if (upload_us < vid->perf.min_us)
    vid->perf.min_us = upload_us;
  if (upload_us > vid->perf.max_us)
    vid->perf.max_us = upload_us;

  // Update global texture upload time for this video
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

  // Print performance summary before cleanup
  print_perf_summary();

  // Restore original CRTC state
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

int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);

  gst_init(&argc, &argv);

  kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0) {
    kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  }
  if (kms.fd < 0) {
    perror("Failed to open DRM device");
    return -1;
  }

  // Save original CRTC state
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

  // Save original CRTC state
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

  // Create and setup shader program
  kms.prog = glCreateProgram();
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_src, NULL);
  glCompileShader(vs);

  // Check vertex shader compilation
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

  // Create VBO
  glGenBuffers(1, &kms.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, kms.vbo);
  glBufferData(GL_ARRAY_BUFFER, 4 * 6 * 4 * sizeof(float), NULL,
               GL_DYNAMIC_DRAW);

  // Set texture uniforms
  glUniform1i(glGetUniformLocation(kms.prog, "tex_y"), 0);
  glUniform1i(glGetUniformLocation(kms.prog, "tex_uv"), 1);

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

  int back_buf = 1; // Start rendering to buffer 1 (buffer 0 is on screen)
  struct timespec anim_t0, anim_t1;
  printf("\nStarting main loop...\n");
  printf("First 60 frames will show test pattern, then videos\n");

  clock_gettime(CLOCK_MONOTONIC, &anim_t0);
  const double ANIM_STEP_SEC = 2.0;

  // Initialize both buffers with something visible
  for (int i = 0; i < 2; i++) {
    glBindFramebuffer(GL_FRAMEBUFFER, kms.bufs[i].fbo_id);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f); // Blue
    glClear(GL_COLOR_BUFFER_BIT);
  }
  glFinish();

  // Performance stats variables
  init_perf_stats();
  struct timespec second_start, second_end, frame_start, eos_done, upload_done,
      draw_done, plane_done;
  clock_gettime(CLOCK_MONOTONIC, &second_start);

  int frames_this_second = 0;
  long second_total_eos = 0;
  long second_total_upload = 0;
  long second_total_draw = 0;
  long second_total_plane = 0;
  long second_upload_counts[VIDEO_COUNT] = {0};
  long second_draw_counts[VIDEO_COUNT] = {0};

  while (running) {
    clock_gettime(CLOCK_MONOTONIC, &frame_start);

    // Reset per-frame timing arrays
    for (int i = 0; i < VIDEO_COUNT; i++) {
      perf.texture_upload_us[i] = 0;
      perf.gl_draw_us[i] = 0;
    }

    // Animation update
    clock_gettime(CLOCK_MONOTONIC, &anim_t1);
    double elapsed = (anim_t1.tv_sec - anim_t0.tv_sec) +
                     (anim_t1.tv_nsec - anim_t0.tv_nsec) / 1e9;

    if (elapsed >= ANIM_STEP_SEC) {
      current_anim_step = (current_anim_step + 1) % 6;
      update_geometry(current_anim_step);
      anim_t0 = anim_t1;
    }

    // Render to back buffer
    glBindFramebuffer(GL_FRAMEBUFFER, kms.bufs[back_buf].fbo_id);
    glViewport(0, 0, kms.mode.hdisplay, kms.mode.vdisplay);

    // glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    // glClear(GL_COLOR_BUFFER_BIT);

    // EOS check phase
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

    // Texture upload phase
    for (int i = 0; i < VIDEO_COUNT; i++) {
      struct timespec upload_start, upload_end;
      clock_gettime(CLOCK_MONOTONIC, &upload_start);

      update_texture_cpu(&videos[i], i);

      clock_gettime(CLOCK_MONOTONIC, &upload_end);
      perf.texture_upload_us[i] = get_diff_us(upload_start, upload_end);
    }
    clock_gettime(CLOCK_MONOTONIC, &upload_done);

    // Draw phase
    for (int i = 0; i < VIDEO_COUNT; i++) {
      struct timespec draw_start, draw_end;
      clock_gettime(CLOCK_MONOTONIC, &draw_start);

      if (videos[i].tex_y) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, videos[i].tex_y);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, videos[i].tex_uv);

        glDrawArrays(GL_TRIANGLES, i * 6, 6);
      }

      clock_gettime(CLOCK_MONOTONIC, &draw_end);
      perf.gl_draw_us[i] = get_diff_us(draw_start, draw_end);
    }
    clock_gettime(CLOCK_MONOTONIC, &draw_done);

    // Plane set
    drmModeSetPlane(kms.fd, kms.plane_id, kms.crtc->crtc_id,
                    kms.bufs[back_buf].fb_id, 0, 0, 0, kms.mode.hdisplay,
                    kms.mode.vdisplay, 0, 0, kms.mode.hdisplay << 16,
                    kms.mode.vdisplay << 16);
    drmModePageFlip(kms.fd, kms.crtc->crtc_id, kms.bufs[back_buf].fb_id,
                    DRM_MODE_PAGE_FLIP_EVENT, NULL);
    clock_gettime(CLOCK_MONOTONIC, &plane_done);

    back_buf = !back_buf;

    // Update performance stats
    perf.frame_count++;
    frames_this_second++;
    second_total_eos += get_diff_us(frame_start, eos_done);
    second_total_upload += get_diff_us(eos_done, upload_done);
    second_total_draw += get_diff_us(upload_done, draw_done);
    second_total_plane += get_diff_us(draw_done, plane_done);

    for (int i = 0; i < VIDEO_COUNT; i++) {
      second_upload_counts[i] += perf.texture_upload_us[i];
      second_draw_counts[i] += (perf.gl_draw_us[i] > 0) ? 1 : 0;
    }

    // Check if 1 second has passed
    clock_gettime(CLOCK_MONOTONIC, &second_end);
    if (get_diff_us(second_start, second_end) >= 1000000) {
      print_second_stats(second_start, second_end, frames_this_second,
                         second_total_eos, second_total_upload,
                         second_total_draw, second_total_plane,
                         second_upload_counts, second_draw_counts);

      // Update rolling averages
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

      // Reset for next second
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