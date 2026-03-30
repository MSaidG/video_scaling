#include "gst/gstbin.h"
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <termios.h> 
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

// --- CONFIG ---
#define TOTAL_VIDEOS 8
char *VIDEO_FILES[TOTAL_VIDEOS] = {"earth1.mp4", "camel.mp4",    "meerkat_.mp4",
                                   "muskox_.mp4",  "nature_bbc_.mp4", "penguin_.mp4",
                                   "squirrel_.mp4",    "wild_hamster_.mp4"};

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
  uint32_t handle, stride, size, fb_id;
  int prime_fd;
  EGLImageKHR egl_img;
  GLuint tex_id, fbo_id;
} DumbBuffer;

typedef struct {
  int is_hdmi;
  int fd;
  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc, *saved_crtc;
  uint32_t crtc_index, plane_id, plane_primary_id;

  DumbBuffer bufs[2];
  int back_buf;

  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;

  GLuint prog, vbo;

  // --- ROUTING MATRIX CONFIG ---
  pthread_mutex_t route_lock;
  int num_active_videos;
  int active_video_indices[4];
} DisplayOutput;

typedef struct {
  GstElement *pipeline, *appsink;
  GstBus *bus;
  pthread_mutex_t lock;
  GstSample *new_sample;
  int width, height;
} GstVid;

typedef struct {
  DisplayOutput *disp;
  const char *name;
} RenderThreadCtx;

// --- GLOBALS ---
DisplayOutput disp_dp = {0};
DisplayOutput disp_hdmi = {0};
GstVid videos[TOTAL_VIDEOS];
volatile sig_atomic_t running = 1;

// --- SHADERS & GEOMETRY ---
const char *vs_src =
    "attribute vec4 a_pos; attribute vec2 a_tex; varying vec2 v_tex;\n"
    "void main() { gl_Position = a_pos; v_tex = a_tex; }\n";

const char *fs_src_dp =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float; varying vec2 v_tex; uniform samplerExternalOES "
    "tex_ext;\n"
    "void main() { gl_FragColor = texture2D(tex_ext, v_tex); }\n";

const char *fs_src_hdmi = "#extension GL_OES_EGL_image_external : require\n"
                          "precision mediump float; varying vec2 v_tex; "
                          "uniform samplerExternalOES tex_ext;\n"
                          "void main() { vec4 rgb = texture2D(tex_ext, v_tex); "
                          "gl_FragColor = vec4(rgb.b, rgb.g, rgb.r, 1.0); }\n";

const GLfloat single_verts[6 * 4] = {
    -1.0f, 1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f,
    1.0f,  1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f};

// NEW: 2-Video Split Screen Geometry
const GLfloat split_verts[2 * 6 * 4] = {
    // Left Video: x=-1 to 0
    -1.0f, 1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
    0.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f,
    // Right Video: x=0 to 1
    0.0f, 1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    1.0f, 1.0f, 1.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f};

const GLfloat grid_verts[4 * 6 * 4] = {
    // Quad 0 (Draw 1st): Target Top-Left
    -1.0f,  0.0f,  0.0f, 1.0f,  -1.0f, -1.0f,  0.0f, 0.0f,   0.0f,  0.0f,  1.0f, 1.0f,
     0.0f,  0.0f,  1.0f, 1.0f,  -1.0f, -1.0f,  0.0f, 0.0f,   0.0f, -1.0f,  1.0f, 0.0f,

    // Quad 1 (Draw 2nd): Target Top-Right
     0.0f,  0.0f,  0.0f, 1.0f,   0.0f, -1.0f,  0.0f, 0.0f,   1.0f,  0.0f,  1.0f, 1.0f,
     1.0f,  0.0f,  1.0f, 1.0f,   0.0f, -1.0f,  0.0f, 0.0f,   1.0f, -1.0f,  1.0f, 0.0f,

    // Quad 2 (Draw 3rd): Target Bottom-Left
    -1.0f,  1.0f,  0.0f, 1.0f,  -1.0f,  0.0f,  0.0f, 0.0f,   0.0f,  1.0f,  1.0f, 1.0f,
     0.0f,  1.0f,  1.0f, 1.0f,  -1.0f,  0.0f,  0.0f, 0.0f,   0.0f,  0.0f,  1.0f, 0.0f,

    // Quad 3 (Draw 4th): Target Bottom-Right
     0.0f,  1.0f,  0.0f, 1.0f,   0.0f,  0.0f,  0.0f, 0.0f,   1.0f,  1.0f,  1.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 1.0f,   0.0f,  0.0f,  0.0f, 0.0f,   1.0f,  0.0f,  1.0f, 0.0f
};

// --- HELPERS ---
void handle_sigint(int sig) { running = 0; }
long get_diff_us(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000000 +
         (end.tv_nsec - start.tv_nsec) / 1000;
}
void make_current(DisplayOutput *disp) {
  eglMakeCurrent(disp->egl_disp, disp->egl_surf, disp->egl_surf, disp->egl_ctx);
}

// --- GSTREAMER ---
static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
  GstVid *vid = (GstVid *)user_data;
  GstSample *sample = gst_app_sink_pull_sample(appsink);
  if (sample) {
    pthread_mutex_lock(&vid->lock);
    if (vid->new_sample)
      gst_sample_unref(vid->new_sample);
    vid->new_sample = sample;
    pthread_mutex_unlock(&vid->lock);
    return GST_FLOW_OK;
  }
  return GST_FLOW_ERROR;
}

static GstPadProbeReturn allocation_probe_cb(GstPad *pad, GstPadProbeInfo *info,
                                             gpointer user_data) {
  GstQuery *query = GST_PAD_PROBE_INFO_QUERY(info);
  if (GST_QUERY_TYPE(query) == GST_QUERY_ALLOCATION)
    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
  return GST_PAD_PROBE_OK;
}

int init_gstreamer_pipeline(GstVid *vid, const char *filename, int index) {
  pthread_mutex_init(&vid->lock, NULL);
  vid->new_sample = NULL;

  char pipeline_str[512];
  snprintf(pipeline_str, sizeof(pipeline_str),
           "filesrc location=%s ! qtdemux ! h264parse ! omxh264dec ! "
           "video/x-raw,format=NV12 ! appsink name=mysink%d sync=true "
           "drop=false max-buffers=2",
           filename, index);

  GError *err = NULL;
  vid->pipeline = gst_parse_launch(pipeline_str, &err);
  if (err)
    return -1;

  char sink_name[32];
  snprintf(sink_name, sizeof(sink_name), "mysink%d", index);
  vid->appsink = gst_bin_get_by_name(GST_BIN(vid->pipeline), sink_name);

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

// --- HARDWARE ABSTRACTION ---
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

uint32_t find_suitable_plane(int fd, uint32_t crtc_id, uint32_t crtc_index) {
  drmModePlaneRes *plane_res = drmModeGetPlaneResources(fd);
  if (!plane_res)
    return 0;
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
      drmModeFreePlane(plane);
      drmModeFreePlaneResources(plane_res);
      return plane_id;
    }
    drmModeFreePlane(plane);
  }
  drmModeFreePlaneResources(plane_res);
  return 0;
}

int init_display(DisplayOutput *disp, const char *primary_node,
                 const char *fallback_node, int is_hdmi) {
  pthread_mutex_init(&disp->route_lock, NULL);
  disp->is_hdmi = is_hdmi;
  disp->fd = open(primary_node, O_RDWR | O_CLOEXEC);
  if (disp->fd < 0)
    disp->fd = open(fallback_node, O_RDWR | O_CLOEXEC);
  if (disp->fd < 0)
    return -1;

  drmModeRes *res = drmModeGetResources(disp->fd);
  if (!res)
    return -1;

  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *c = drmModeGetConnector(disp->fd, res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      disp->connector = c;
      break;
    }
    if (c)
      drmModeFreeConnector(c);
  }
  if (!disp->connector)
    return -1;

  disp->mode = disp->connector->modes[0];
  disp->crtc = drmModeGetCrtc(disp->fd, res->crtcs[0]);
  disp->saved_crtc = drmModeGetCrtc(disp->fd, disp->crtc->crtc_id);

  if (is_hdmi) {
    disp->crtc_index = -1;
    for (int i = 0; i < res->count_crtcs; i++) {
      if (res->crtcs[i] == disp->crtc->crtc_id) {
        disp->crtc_index = i;
        break;
      }
    }
    if (disp->crtc_index >= 0)
      disp->plane_id =
          find_suitable_plane(disp->fd, disp->crtc->crtc_id, disp->crtc_index);
  } else {
    disp->plane_primary_id = 39;
    disp->plane_id = 41;
  }
  drmModeFreeResources(res);

  disp->egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(disp->egl_disp, NULL, NULL)) {
    disp->egl_disp = eglGetDisplay((EGLNativeDisplayType)disp->fd);
    eglInitialize(disp->egl_disp, NULL, NULL);
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
  eglChooseConfig(disp->egl_disp, attribs, &config, 1, &num);
  disp->egl_surf = eglCreatePbufferSurface(
      disp->egl_disp, config,
      (EGLint[]){EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE});
  disp->egl_ctx =
      eglCreateContext(disp->egl_disp, config, EGL_NO_CONTEXT,
                       (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});

  make_current(disp);
  load_egl_extensions();

  disp->prog = glCreateProgram();
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_src, NULL);
  glCompileShader(vs);
  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  const char *fs_source = is_hdmi ? fs_src_hdmi : fs_src_dp;
  glShaderSource(fs, 1, &fs_source, NULL);
  glCompileShader(fs);
  glAttachShader(disp->prog, vs);
  glAttachShader(disp->prog, fs);
  glLinkProgram(disp->prog);
  glUseProgram(disp->prog);

  glGenBuffers(1, &disp->vbo);
  glBindBuffer(GL_ARRAY_BUFFER, disp->vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(grid_verts), NULL, GL_DYNAMIC_DRAW);

  GLint loc_pos = glGetAttribLocation(disp->prog, "a_pos");
  GLint loc_tex = glGetAttribLocation(disp->prog, "a_tex");
  int stride = 4 * sizeof(float);
  glEnableVertexAttribArray(loc_pos);
  glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
  glEnableVertexAttribArray(loc_tex);
  glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, stride,
                        (void *)(2 * sizeof(float)));

  glUniform1i(glGetUniformLocation(disp->prog, "tex_ext"), 0);
  disp->back_buf = 0;
  return 0;
}

int create_buffer(DisplayOutput *disp, DumbBuffer *buf) {
  make_current(disp);
  struct drm_mode_create_dumb create_req = {0};
  create_req.width = disp->mode.hdisplay;
  create_req.height = disp->mode.vdisplay;
  create_req.bpp = 32;
  ioctl(disp->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);
  buf->handle = create_req.handle;
  buf->stride = create_req.pitch;
  buf->size = create_req.size;

  if (disp->is_hdmi) {
    uint32_t formats_to_try[] = {DRM_FORMAT_XBGR8888, DRM_FORMAT_BGRX8888,
                                 DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
    int fb_added = 0;
    for (int f = 0; f < 4; f++) {
      uint32_t handles[4] = {buf->handle};
      uint32_t pitches[4] = {buf->stride};
      uint32_t offsets[4] = {0};
      if (drmModeAddFB2(disp->fd, disp->mode.hdisplay, disp->mode.vdisplay,
                        formats_to_try[f], handles, pitches, offsets,
                        &buf->fb_id, 0) == 0) {
        fb_added = 1;
        break;
      }
    }
    if (!fb_added)
      return -1;
  } else {
    drmModeAddFB(disp->fd, disp->mode.hdisplay, disp->mode.vdisplay, 24, 32,
                 buf->stride, buf->handle, &buf->fb_id);
  }

  struct drm_prime_handle prime = {.handle = buf->handle,
                                   .flags = DRM_CLOEXEC | DRM_RDWR};
  ioctl(disp->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
  buf->prime_fd = prime.fd;

  uint32_t format = disp->is_hdmi ? DRM_FORMAT_XBGR8888 : DRM_FORMAT_ARGB8888;
  EGLint attribs[] = {EGL_WIDTH,
                      disp->mode.hdisplay,
                      EGL_HEIGHT,
                      disp->mode.vdisplay,
                      EGL_LINUX_DRM_FOURCC_EXT,
                      format,
                      EGL_DMA_BUF_PLANE0_FD_EXT,
                      buf->prime_fd,
                      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                      0,
                      EGL_DMA_BUF_PLANE0_PITCH_EXT,
                      buf->stride,
                      EGL_NONE};
  buf->egl_img = eglCreateImageKHR(disp->egl_disp, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, NULL, attribs);

  if (!buf->egl_img && disp->is_hdmi) {
    EGLint attribs2[] = {EGL_WIDTH,
                         disp->mode.hdisplay,
                         EGL_HEIGHT,
                         disp->mode.vdisplay,
                         EGL_LINUX_DRM_FOURCC_EXT,
                         DRM_FORMAT_ARGB8888,
                         EGL_DMA_BUF_PLANE0_FD_EXT,
                         buf->prime_fd,
                         EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                         0,
                         EGL_DMA_BUF_PLANE0_PITCH_EXT,
                         buf->stride,
                         EGL_NONE};
    buf->egl_img = eglCreateImageKHR(disp->egl_disp, EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT, NULL, attribs2);
  }

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

// Map the DMA-BUF from the GStreamer pool into the specific Display's EGL
// context
GstSample *route_video_to_display(DisplayOutput *disp, GstVid *vid,
                                  GLuint *out_tex_id, EGLImageKHR *out_egl_img,
                                  GstSample *current_local_sample) {
  GstSample *sample = NULL;

  pthread_mutex_lock(&vid->lock);
  if (vid->new_sample && vid->new_sample != current_local_sample) {
    sample = gst_sample_ref(vid->new_sample);
  }
  pthread_mutex_unlock(&vid->lock);

  if (!sample)
    return NULL;

  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstCaps *caps = gst_sample_get_caps(sample);
  GstVideoInfo vinfo;
  gst_video_info_from_caps(&vinfo, caps);

  GstMemory *mem = gst_buffer_peek_memory(buffer, 0);
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

  if (*out_egl_img)
    eglDestroyImageKHR(disp->egl_disp, *out_egl_img);

  EGLint attribs[] = {EGL_WIDTH,
                      vinfo.width,
                      EGL_HEIGHT,
                      vinfo.height,
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
  *out_egl_img = eglCreateImageKHR(disp->egl_disp, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, NULL, attribs);

  if (!*out_tex_id) {
    glGenTextures(1, out_tex_id);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, *out_tex_id);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S,
                    GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T,
                    GL_CLAMP_TO_EDGE);
  }

  glBindTexture(GL_TEXTURE_EXTERNAL_OES, *out_tex_id);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES,
                               (GLeglImageOES)*out_egl_img);

  return sample;
}

// --- STANDARD C INPUT THREAD (No ncurses needed) ---
void print_menu(int target_mon, int *q_vids, int q_count) {
  // ANSI escape code to clear screen and move cursor to top-left
  printf("\033[H\033[J");
  printf("=================================================\n");
  printf("   HARDWARE VIDEO MATRIX ROUTER (8 CHANNELS)     \n");
  printf("=================================================\n");
  printf(" CONTROLS:\n");
  printf("  [1] Select DP Monitor    [2] Select HDMI Monitor\n");
  printf("  [1-8] Add Videos (Max 4) [ENTER] Apply Layout\n");
  printf("  [0] Reset All to Default [Q] Quit\n");
  printf("-------------------------------------------------\n");

  if (target_mon == -1) {
    printf(" STATUS: Idle. Awaiting Target Monitor (Press 1 or 2).\n");
  } else {
    printf(" TARGET: Monitor %d (%s)\n", target_mon + 1,
           target_mon == 0 ? "DP" : "HDMI");
    printf(" QUEUE:  [ ");
    for (int i = 0; i < q_count; i++) {
      printf("Vid_%d ", q_vids[i] + 1);
    }
    printf("]\n");
    if (q_count == 4)
      printf("\n --> 4 Videos Reached! Auto-Applying Grid...\n");
    else
      printf("\n Press Enter to apply, or keep adding videos.\n");
  }
  fflush(stdout); // Force print to screen immediately
}

void *input_thread(void *arg) {
  // Set terminal to non-canonical, non-echo mode (so you don't have to press
  // enter for numbers)
  struct termios oldt, newt;
  tcgetattr(STDIN_FILENO, &oldt);
  newt = oldt;
  newt.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSANOW, &newt);

  // Make standard input non-blocking
  int oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
  fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

  int target_mon = -1; // -1: Idle, 0: DP, 1: HDMI
  int q_vids[4];
  int q_count = 0;

  print_menu(target_mon, q_vids, q_count);

  while (running) {
    int ch = getchar();
    if (ch != EOF) {
      if (ch == 'q' || ch == 'Q') {
        running = 0;
        break;
      }

      // Global Reset
      if (ch == '0') {
        pthread_mutex_lock(&disp_dp.route_lock);
        disp_dp.num_active_videos = 1;
        disp_dp.active_video_indices[0] = 0;
        pthread_mutex_unlock(&disp_dp.route_lock);

        pthread_mutex_lock(&disp_hdmi.route_lock);
        disp_hdmi.num_active_videos = 1;
        disp_hdmi.active_video_indices[0] = 1;
        pthread_mutex_unlock(&disp_hdmi.route_lock);

        target_mon = -1;
        q_count = 0;
        print_menu(target_mon, q_vids, q_count);
        continue;
      }

      // Target Selection (Idle State)
      if (target_mon == -1) {
        if (ch == '1' || ch == '2') {
          target_mon = ch - '1';
          q_vids[0] = (target_mon == 0) ? 0 : 1; // Default base video
          q_count = 1;
          print_menu(target_mon, q_vids, q_count);
        }
      }
      // Adding State
      else {
        // Number pressed
        if (ch >= '1' && ch <= '8') {
          int vid_idx = ch - '1';

          // Check for Duplicates
          int is_duplicate = 0;
          for (int i = 0; i < q_count; i++) {
            if (q_vids[i] == vid_idx)
              is_duplicate = 1;
          }

          if (!is_duplicate) {
            q_vids[q_count++] = vid_idx;
            print_menu(target_mon, q_vids, q_count);

            // Rule 2: Auto-Apply if we hit 4
            if (q_count == 4) {
              usleep(500000); // Give user half a second to see the text
              DisplayOutput *d = (target_mon == 0) ? &disp_dp : &disp_hdmi;
              pthread_mutex_lock(&d->route_lock);
              d->num_active_videos = 4;
              for (int i = 0; i < 4; i++)
                d->active_video_indices[i] = q_vids[i];
              pthread_mutex_unlock(&d->route_lock);

              target_mon = -1;
              q_count = 0;
              print_menu(target_mon, q_vids, q_count);
            }
          }
        }
        // Enter pressed
        else if (ch == '\n' || ch == '\r') {
          if (q_count == 1) {
            // Added nothing, reset back to idle
            target_mon = -1;
            q_count = 0;
            print_menu(target_mon, q_vids, q_count);
          } else {
            // If user queued 3 total videos, drop the last one to fallback to
            // split-screen (2)
            if (q_count == 3) {
              q_count = 2;
            }

            DisplayOutput *d = (target_mon == 0) ? &disp_dp : &disp_hdmi;
            pthread_mutex_lock(&d->route_lock);
            d->num_active_videos = q_count;
            for (int i = 0; i < q_count; i++)
              d->active_video_indices[i] = q_vids[i];
            pthread_mutex_unlock(&d->route_lock);

            target_mon = -1;
            q_count = 0;
            print_menu(target_mon, q_vids, q_count);
          }
        }
      }
    }
    usleep(10000); // 10ms poll
  }

  // Cleanly restore terminal to normal behavior before exiting
  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  fcntl(STDIN_FILENO, F_SETFL, oldf);

  return NULL;
}

// --- RENDER THREAD ---
void *render_loop_thread(void *arg) {
  RenderThreadCtx *ctx = (RenderThreadCtx *)arg;
  make_current(ctx->disp);

  GLuint local_textures[TOTAL_VIDEOS] = {0};
  EGLImageKHR local_egl_images[TOTAL_VIDEOS] = {0};
  GstSample *local_active_samples[TOTAL_VIDEOS] = {NULL};

  while (running) {
    for (int i = 0; i < TOTAL_VIDEOS; i++) {
      GstMessage *msg = gst_bus_pop_filtered(videos[i].bus, GST_MESSAGE_EOS);
      if (msg) {
        gst_element_seek_simple(videos[i].pipeline, GST_FORMAT_TIME,
                                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                0);
        gst_message_unref(msg);
      }
    }

    pthread_mutex_lock(&ctx->disp->route_lock);
    int num_vids = ctx->disp->num_active_videos;
    int active_vids[4];
    for (int i = 0; i < num_vids; i++)
      active_vids[i] = ctx->disp->active_video_indices[i];
    pthread_mutex_unlock(&ctx->disp->route_lock);

    for (int i = 0; i < num_vids; i++) {
      int global_vid_idx = active_vids[i];
      GstSample *new_samp = route_video_to_display(
          ctx->disp, &videos[global_vid_idx], &local_textures[global_vid_idx],
          &local_egl_images[global_vid_idx],
          local_active_samples[global_vid_idx]);
      if (new_samp) {
        if (local_active_samples[global_vid_idx])
          gst_sample_unref(local_active_samples[global_vid_idx]);
        local_active_samples[global_vid_idx] = new_samp;
      }
    }

    glBindFramebuffer(GL_FRAMEBUFFER,
                      ctx->disp->bufs[ctx->disp->back_buf].fbo_id);
    glViewport(0, 0, ctx->disp->mode.hdisplay, ctx->disp->mode.vdisplay);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindBuffer(GL_ARRAY_BUFFER, ctx->disp->vbo);
    // NEW: Select the correct geometry based on the active queue
    if (num_vids == 1) {
      glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(single_verts), single_verts);
    } else if (num_vids == 2) {
      glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(split_verts), split_verts);
    } else {
      glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(grid_verts), grid_verts);
    }

    GLint loc_pos = glGetAttribLocation(ctx->disp->prog, "a_pos");
    GLint loc_tex = glGetAttribLocation(ctx->disp->prog, "a_tex");
    int stride = 4 * sizeof(float);
    glEnableVertexAttribArray(loc_pos);
    glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
    glEnableVertexAttribArray(loc_tex);
    glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, stride,
                          (void *)(2 * sizeof(float)));

    for (int i = 0; i < num_vids; i++) {
      int global_vid_idx = active_vids[i];
      if (local_textures[global_vid_idx]) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, local_textures[global_vid_idx]);
        glDrawArrays(GL_TRIANGLES, i * 6, 6);
      }
    }
    glFinish();

    if (ctx->disp->is_hdmi) {
      drmModeSetPlane(
          ctx->disp->fd, ctx->disp->plane_id, ctx->disp->crtc->crtc_id,
          ctx->disp->bufs[ctx->disp->back_buf].fb_id, 0, 0, 0,
          ctx->disp->mode.hdisplay, ctx->disp->mode.vdisplay, 0, 0,
          ctx->disp->mode.hdisplay << 16, ctx->disp->mode.vdisplay << 16);
      drmModePageFlip(ctx->disp->fd, ctx->disp->crtc->crtc_id,
                      ctx->disp->bufs[ctx->disp->back_buf].fb_id,
                      DRM_MODE_PAGE_FLIP_EVENT, NULL);
    } else {
      drmModeSetPlane(
          ctx->disp->fd, ctx->disp->plane_primary_id, ctx->disp->crtc->crtc_id,
          ctx->disp->bufs[ctx->disp->back_buf].fb_id, 0, 0, 0,
          ctx->disp->mode.hdisplay, ctx->disp->mode.vdisplay, 0, 0,
          ctx->disp->mode.hdisplay << 16, ctx->disp->mode.vdisplay << 16);
    }
    ctx->disp->back_buf = !ctx->disp->back_buf;
  }

  for (int i = 0; i < TOTAL_VIDEOS; i++) {
    if (local_active_samples[i])
      gst_sample_unref(local_active_samples[i]);
  }
  return NULL;
}

// --- CLEANUP (Called at exit) ---
void cleanup_display(DisplayOutput *disp) {
  if (disp->fd < 0)
    return;
  if (disp->egl_disp != EGL_NO_DISPLAY && disp->egl_ctx != EGL_NO_CONTEXT) {
    make_current(disp);
    if (disp->prog)
      glDeleteProgram(disp->prog);
    if (disp->vbo)
      glDeleteBuffers(1, &disp->vbo);
    for (int i = 0; i < 2; i++) {
      if (disp->bufs[i].fbo_id)
        glDeleteFramebuffers(1, &disp->bufs[i].fbo_id);
      if (disp->bufs[i].tex_id)
        glDeleteTextures(1, &disp->bufs[i].tex_id);
      if (disp->bufs[i].egl_img && eglDestroyImageKHR)
        eglDestroyImageKHR(disp->egl_disp, disp->bufs[i].egl_img);
      if (disp->bufs[i].prime_fd >= 0)
        close(disp->bufs[i].prime_fd);
      if (disp->bufs[i].fb_id)
        drmModeRmFB(disp->fd, disp->bufs[i].fb_id);
      if (disp->bufs[i].handle) {
        struct drm_mode_destroy_dumb destroy_req = {.handle =
                                                        disp->bufs[i].handle};
        ioctl(disp->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
      }
    }
    eglMakeCurrent(disp->egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    eglDestroyContext(disp->egl_disp, disp->egl_ctx);
    eglDestroySurface(disp->egl_disp, disp->egl_surf);
    eglTerminate(disp->egl_disp);
  }
  if (disp->saved_crtc) {
    drmModeSetCrtc(disp->fd, disp->saved_crtc->crtc_id,
                   disp->saved_crtc->buffer_id, disp->saved_crtc->x,
                   disp->saved_crtc->y, &disp->connector->connector_id, 1,
                   &disp->saved_crtc->mode);
    drmModeFreeCrtc(disp->saved_crtc);
  }
  if (disp->crtc)
    drmModeFreeCrtc(disp->crtc);
  if (disp->connector)
    drmModeFreeConnector(disp->connector);
  close(disp->fd);
}

void cleanup() {
  for (int i = 0; i < TOTAL_VIDEOS; i++) {
    if (videos[i].pipeline) {
      gst_element_set_state(videos[i].pipeline, GST_STATE_NULL);
      gst_object_unref(videos[i].pipeline);
    }
    if (videos[i].bus)
      gst_object_unref(videos[i].bus);
    if (videos[i].new_sample)
      gst_sample_unref(videos[i].new_sample);
    pthread_mutex_destroy(&videos[i].lock);
  }
  cleanup_display(&disp_dp);
  cleanup_display(&disp_hdmi);
}

// --- MAIN LOOP ---
int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);
  gst_init(&argc, &argv);

  if (init_display(&disp_dp, "/dev/dri/card0", "/dev/dri/card1", 0) < 0)
    return -1;
  if (init_display(&disp_hdmi, "/dev/dri/card1", "/dev/dri/card0", 1) < 0)
    return -1;

  for (int i = 0; i < 2; i++) {
    create_buffer(&disp_dp, &disp_dp.bufs[i]);
    create_buffer(&disp_hdmi, &disp_hdmi.bufs[i]);
  }

  drmModeSetCrtc(disp_dp.fd, disp_dp.crtc->crtc_id, disp_dp.bufs[0].fb_id, 0, 0,
                 &disp_dp.connector->connector_id, 1, &disp_dp.mode);
  drmModeSetCrtc(disp_hdmi.fd, disp_hdmi.crtc->crtc_id, disp_hdmi.bufs[0].fb_id,
                 0, 0, &disp_hdmi.connector->connector_id, 1, &disp_hdmi.mode);
  disp_dp.back_buf = 1;
  disp_hdmi.back_buf = 1;

  disp_dp.num_active_videos = 1;
  disp_dp.active_video_indices[0] = 0;
  disp_hdmi.num_active_videos = 1;
  disp_hdmi.active_video_indices[0] = 1;

  for (int i = 0; i < TOTAL_VIDEOS; i++) {
    if (init_gstreamer_pipeline(&videos[i], VIDEO_FILES[i], i) < 0)
      return -1;
  }

  RenderThreadCtx dp_ctx = {&disp_dp, "DP"};
  RenderThreadCtx hdmi_ctx = {&disp_hdmi, "HDMI"};

  pthread_t hdmi_thread, input_tid;
  pthread_create(&hdmi_thread, NULL, render_loop_thread, &hdmi_ctx);
  pthread_create(&input_tid, NULL, input_thread, NULL);

  render_loop_thread(&dp_ctx);

  pthread_join(hdmi_thread, NULL);
  pthread_join(input_tid, NULL);

  // Ensure terminal restores cleanly
  cleanup();
  printf("\nClean exit.\n");
  return 0;
}