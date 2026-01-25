#include "main.h"
#include "gl.h"
#include "utils.h"
#include <GLES2/gl2.h>

#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <signal.h>  // For catching Ctrl+C
#include <termios.h> // For raw terminal mode
#include <time.h>    // For replacing glfw_get_time

// Position (x,y)   Texcoord (u,v)
GLfloat quad[] = {
    -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 0.0f,
};

struct {
  int fd;
  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;
  struct gbm_device *gbm_dev;
  struct gbm_surface *gbm_surf;
  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;
  uint32_t current_fb_id;
  struct gbm_bo *current_bo;
} kms;

struct termios orig_termios;
volatile sig_atomic_t running = 1;

void restore_terminal() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }

void enable_raw_mode() {
  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(restore_terminal); // Auto-restore on exit

  struct termios raw = orig_termios;
  // Disable Echo (printing keys) and Canonical mode (waiting for Enter)
  raw.c_lflag &= ~(ECHO | ICANON);
  // Set read timeout to 0 (Non-blocking read)
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Signal handler for Ctrl+C
void handle_sigint(int sig) { running = 0; }

// Replacement for get_time_sec() using standard Linux clock
double get_time_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// int init_kms_zero_copy() {
//   // 1. Open DRM Device
//   kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);

//   // 2. Find a connected Connector (HDMI/DP)
//   drmModeRes *resources = drmModeGetResources(kms.fd);
//   for (int i = 0; i < resources->count_connectors; i++) {
//     drmModeConnector *conn =
//         drmModeGetConnector(kms.fd, resources->connectors[i]);
//     if (conn->connection == DRM_MODE_CONNECTED) {
//       kms.connector = conn;
//       break;
//     }
//     drmModeFreeConnector(conn);
//   }
//   // Pick the preferred resolution (mode)
//   kms.mode = kms.connector->modes[0];

//   // 3. Find a CRTC (The hardware scanner that reads the buffer)
//   // (Simplified: assuming the first encoder maps to a valid CRTC)
//   drmModeEncoder *enc = drmModeGetEncoder(kms.fd, kms.connector->encoder_id);
//   kms.crtc = drmModeGetCrtc(kms.fd, enc->crtc_id);

//   // 4. Initialize GBM (The buffer manager)
//   kms.gbm_dev = gbm_create_device(kms.fd);

//   // Create a surface OpenGL can draw to.
//   // GBM_FORMAT_XRGB8888 is the standard "Screen" format.
//   // GBM_BO_USE_SCANOUT means "The display controller can read this directly"
//   // GBM_BO_USE_RENDERING means "OpenGL can write to this"
//   kms.gbm_surf = gbm_surface_create(kms.gbm_dev, kms.mode.hdisplay,
//                                     kms.mode.vdisplay, GBM_FORMAT_XRGB8888,
//                                     GBM_BO_USE_SCANOUT |
//                                     GBM_BO_USE_RENDERING);

//   // 5. Initialize EGL on top of GBM
//   kms.egl_disp =
//       eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, kms.gbm_dev, NULL);
//   eglInitialize(kms.egl_disp, NULL, NULL);

//   eglBindAPI(EGL_OPENGL_ES_API);
//   EGLint attribs[] = {EGL_RED_SIZE,  8, EGL_GREEN_SIZE, 8,
//                       EGL_BLUE_SIZE, 8, EGL_NONE};
//   EGLConfig config;
//   EGLint num_config;
//   eglChooseConfig(kms.egl_disp, attribs, &config, 1, &num_config);

//   kms.egl_ctx =
//       eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT,
//                        (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
//   kms.egl_surf = eglCreateWindowSurface(
//       kms.egl_disp, config, (EGLNativeWindowType)kms.gbm_surf, NULL);

//   eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);

//   return 0;
// }

int init_kms_zero_copy() {
  // 1. Open DRM Device (Try card0, then card1)
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0) {
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if (kms.fd < 0) {
      fprintf(stderr, "Error: Could not open /dev/dri/card0 or card1\n");
      return -1;
    }
  }

  // 2. Setup KMS (Connector, CRTC, Mode)
  drmModeRes *resources = drmModeGetResources(kms.fd);
  if (!resources)
    return -1;

  for (int i = 0; i < resources->count_connectors; i++) {
    drmModeConnector *conn =
        drmModeGetConnector(kms.fd, resources->connectors[i]);
    if (conn->connection == DRM_MODE_CONNECTED) {
      kms.connector = conn;
      break;
    }
    drmModeFreeConnector(conn);
  }
  if (!kms.connector) {
    fprintf(stderr, "Error: No connected monitor found.\n");
    return -1;
  }

  kms.mode = kms.connector->modes[0];
  printf("Selected Mode: %dx%d @ %dHz\n", kms.mode.hdisplay, kms.mode.vdisplay,
         kms.mode.vrefresh);

  drmModeEncoder *enc = drmModeGetEncoder(kms.fd, kms.connector->encoders[0]);
  if (enc && enc->crtc_id) {
    kms.crtc = drmModeGetCrtc(kms.fd, enc->crtc_id);
  } else {
    // Fallback if encoder not active
    kms.crtc = drmModeGetCrtc(kms.fd, resources->crtcs[0]);
  }
  if (enc)
    drmModeFreeEncoder(enc);

  // 3. Setup GBM
  kms.gbm_dev = gbm_create_device(kms.fd);

  // IMPORTANT CHANGE: Use ARGB8888 (Most compatible with EGL)
  uint32_t gbm_format = GBM_FORMAT_ARGB8888;

  kms.gbm_surf =
      gbm_surface_create(kms.gbm_dev, kms.mode.hdisplay, kms.mode.vdisplay,
                         gbm_format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!kms.gbm_surf) {
    fprintf(stderr, "Error: GBM Surface creation failed.\n");
    return -1;
  }

  // 4. Setup EGL
  kms.egl_disp =
      eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, kms.gbm_dev, NULL);
  eglInitialize(kms.egl_disp, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);

  // --- CRITICAL FIX: FIND MATCHING CONFIG ---
  // We don't just ask for "Red=8", we look for a config that matches our GBM
  // Format
  EGLConfig config;
  EGLint num_configs;

  if (!eglGetConfigs(kms.egl_disp, NULL, 0, &num_configs)) {
    fprintf(stderr, "Error: eglGetConfigs failed.\n");
    return -1;
  }

  EGLConfig *configs = malloc(num_configs * sizeof(EGLConfig));
  eglGetConfigs(kms.egl_disp, configs, num_configs, &num_configs);

  int found_config = 0;
  for (int i = 0; i < num_configs; i++) {
    EGLint id;
    // Check if this config handles the format (GBM_FORMAT_ARGB8888) we used
    eglGetConfigAttrib(kms.egl_disp, configs[i], EGL_NATIVE_VISUAL_ID, &id);
    if (id == gbm_format) {
      config = configs[i];
      found_config = 1;
      break;
    }
  }
  free(configs);

  if (!found_config) {
    fprintf(stderr,
            "Error: Could not find EGL config matching GBM format 0x%x\n",
            gbm_format);
    return -1;
  }
  // ------------------------------------------

  EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT, ctx_attribs);

  kms.egl_surf = eglCreateWindowSurface(
      kms.egl_disp, config, (EGLNativeWindowType)kms.gbm_surf, NULL);

  if (kms.egl_surf == EGL_NO_SURFACE) {
    fprintf(stderr, "Error: eglCreateWindowSurface failed (EGL Error: 0x%x)\n",
            eglGetError());
    return -1;
  }

  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);

  printf("KMS/GBM/EGL Initialized!\n");
  return 0;
}

// Add this flag to your KMS struct or global
int waiting_for_flip = 0;

// This function is called by the Kernel when the screen update is actually done
static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void *data) {
  int *waiting = (int *)data;
  *waiting = 0; // We are no longer waiting!
}

// Helper to turn a GBM buffer into a DRM Framebuffer ID
uint32_t get_fb_for_bo(struct gbm_bo *bo) {
  uint32_t fb_id;
  // If we already made an FB for this specific buffer, return it (cache it in
  // userData) For simplicity here, we create a new FB every frame (not optimal
  // but easier to read)

  uint32_t handle = gbm_bo_get_handle(bo).u32;
  uint32_t stride = gbm_bo_get_stride(bo);
  uint32_t width = gbm_bo_get_width(bo);
  uint32_t height = gbm_bo_get_height(bo);

  drmModeAddFB(kms.fd, width, height, 24, 32, stride, handle, &fb_id);
  return fb_id;
}

// void swap_buffers_kms() {
//   // 1. Tell OpenGL to finish rendering
//   eglSwapBuffers(kms.egl_disp, kms.egl_surf);

//   // 2. Lock the front buffer (the one GL just finished)
//   struct gbm_bo *next_bo = gbm_surface_lock_front_buffer(kms.gbm_surf);
//   uint32_t next_fb_id = get_fb_for_bo(next_bo);

//   // 3. Schedule the Page Flip (Show this frame on VSYNC)
//   drmModePageFlip(kms.fd, kms.crtc->crtc_id, next_fb_id,
//                   DRM_MODE_PAGE_FLIP_EVENT, NULL);

//   // 4. Cleanup old buffer (The previous frame)
//   if (kms.current_bo) {
//     gbm_surface_release_buffer(kms.gbm_surf, kms.current_bo);
//     // Note: In real code, you should also remove the old FB_ID using
//     // drmModeRmFB
//   }

//   kms.current_bo = next_bo;
//   kms.current_fb_id = next_fb_id;

//   // Note: To make this robust, you actually need to wait for the PageFlip
//   Event
//   // here or else you will run too fast and tear.
// }

void swap_buffers_kms() {
  // 1. Finish GL Rendering
  if (!eglSwapBuffers(kms.egl_disp, kms.egl_surf)) {
    return;
  }

  // 2. Lock the newly rendered buffer
  struct gbm_bo *next_bo = gbm_surface_lock_front_buffer(kms.gbm_surf);
  if (!next_bo)
    return;

  uint32_t next_fb_id = get_fb_for_bo(next_bo);

  // 3. Flip to screen AND Request an Event
  // We pass '&waiting_for_flip' as the 'user_data'
  int ret = drmModePageFlip(kms.fd, kms.crtc->crtc_id, next_fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);

  if (ret) {
    fprintf(stderr, "Page Flip failed: %d\n", ret);
    gbm_surface_release_buffer(kms.gbm_surf, next_bo);
    return;
  }

  // Mark that we are now waiting
  waiting_for_flip = 1;

  // 4. Cleanup previous buffer
  if (kms.current_bo) {
    gbm_surface_release_buffer(kms.gbm_surf, kms.current_bo);
  }

  kms.current_bo = next_bo;
  kms.current_fb_id = next_fb_id;
}

PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

int is_paused = 0;               // 0 = Playing, 1 = Paused
double total_pause_offset = 0.0; // Total time spent paused
double last_pause_start = 0.0;   // Timestamp when the current pause started

int main(int argc, char **argv) {

  // 1. Setup Input & Signals
  signal(SIGINT, handle_sigint); // Catch Ctrl+C
  enable_raw_mode();             // Turn off buffering

  if (init_kms_zero_copy() != 0) {
    fprintf(stderr, "Critical Error during KMS init. Exiting.\n");
    return -1;
  }

  eglCreateImageKHR =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  eglDestroyImageKHR =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");

  glEGLImageTargetTexture2DOES =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
          "glEGLImageTargetTexture2DOES");
  if (!glEGLImageTargetTexture2DOES) {
    fprintf(stderr,
            "Error: Driver does not support glEGLImageTargetTexture2DOES\n");
    return -1;
  }

  char *vs_code = read_file("shader.vs"); // MUST UPDATE THIS FILE
  char *fs_code = read_file("shader.fs");
  GLuint program = create_program(vs_code, fs_code);
  free(vs_code);
  free(fs_code);

  GLint aPos = glGetAttribLocation(program, "aPos");
  GLint aTex = glGetAttribLocation(program, "aTex");

  VideoPlayer players[4];
  const char *files[] = {"videos/animals.mp4", "videos/earth.mp4",
                         "videos/galaxy.mp4", "videos/ocean.mp4"};

  for (int i = 0; i < 4; i++) {
    // You can use the same file 4 times for testing if you want
    if (init_player(&players[i], files[i], i) < 0) {
      fprintf(stderr, "Failed to init player %d\n", i);
    }
  }

  glEnableVertexAttribArray(aPos);
  glEnableVertexAttribArray(aTex);
  glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
  glVertexAttribPointer(aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        quad + 2);
  glUseProgram(program);
  glUniform1i(glGetUniformLocation(program, "uTextureY"), 0);
  glUniform1i(glGetUniformLocation(program, "uTextureUV"), 1);

  // --- SETUP DRM EVENT CONTEXT ---
  drmEventContext evctx = {0};
  evctx.version = 2;
  evctx.page_flip_handler = page_flip_handler;

  // Setup file descriptor set for select()
  fd_set fds;

  while (running) {

    // --- VSYNC WAIT LOGIC ---
    // If we submitted a flip, we MUST wait for it to finish before drawing
    // again.
    while (waiting_for_flip) {
      FD_ZERO(&fds);
      FD_SET(kms.fd, &fds);

      // Wait until the KMS file descriptor is readable (Event arrived)
      int ret = select(kms.fd + 1, &fds, NULL, NULL, NULL);
      if (ret < 0) {
        // If interrupted by Ctrl+C, break
        if (errno == EINTR)
          break;
        fprintf(stderr, "Select error: %d\n", errno);
        break;
      } else if (ret > 0) {
        // Process the event (Calls page_flip_handler, sets waiting_for_flip =
        // 0)
        drmHandleEvent(kms.fd, &evctx);
      }
    }
    // ------------------------

    for (int i = 0; i < 4; i++) {
      update_player(&players[i]);
    }

    for (int i = 0; i < 4; i++) {
      render_player(&players[i], program);
    }

    swap_buffers_kms();
  }

  return 0;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts) {
  for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
    if (*p == AV_PIX_FMT_VAAPI) {
      return *p;
    }
  }

  fprintf(stderr, "VAAPI not supported by decoder\n");
  return AV_PIX_FMT_NONE;
}

double pts_to_seconds(int64_t pts, AVRational time_base) {
  return pts * av_q2d(time_base);
}

static AVDRMFrameDescriptor *get_drm_desc(AVFrame *frame) {
  return (AVDRMFrameDescriptor *)frame->data[0];
}

NV12_EGLImages create_split_egl_images(EGLDisplay display,
                                       const AVFrame *frame) {
  NV12_EGLImages result = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};

  if (frame->format != AV_PIX_FMT_DRM_PRIME)
    return result;

  const AVDRMFrameDescriptor *desc =
      (const AVDRMFrameDescriptor *)frame->data[0];

  int layer_y = 0;
  int plane_y = 0;
  int obj_y_idx = desc->layers[layer_y].planes[plane_y].object_index;
  int fd_y = desc->objects[obj_y_idx].fd;
  int offset_y = desc->layers[layer_y].planes[plane_y].offset;
  int stride_y = desc->layers[layer_y].planes[plane_y].pitch;
  uint64_t modifier_y = desc->objects[obj_y_idx].format_modifier;

  EGLint attribs_y[] = {EGL_WIDTH,
                        frame->width,
                        EGL_HEIGHT,
                        frame->height,
                        EGL_LINUX_DRM_FOURCC_EXT,
                        DRM_FORMAT_R8,
                        EGL_DMA_BUF_PLANE0_FD_EXT,
                        fd_y,
                        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                        offset_y,
                        EGL_DMA_BUF_PLANE0_PITCH_EXT,
                        stride_y,
                        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                        (EGLint)(modifier_y & 0xFFFFFFFF),
                        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                        (EGLint)(modifier_y >> 32),
                        EGL_NONE};

  result.image_y = eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT, NULL, attribs_y);

  int layer_uv = (desc->nb_layers > 1) ? 1 : 0;
  int plane_uv = (desc->nb_layers > 1) ? 0 : 1;
  int obj_uv_idx = desc->layers[layer_uv].planes[plane_uv].object_index;

  int fd_uv = desc->objects[obj_uv_idx].fd;
  int offset_uv = desc->layers[layer_uv].planes[plane_uv].offset;
  int stride_uv = desc->layers[layer_uv].planes[plane_uv].pitch;
  uint64_t modifier_uv = desc->objects[obj_uv_idx].format_modifier;

  EGLint attribs_uv[] = {EGL_WIDTH,
                         frame->width / 2,
                         EGL_HEIGHT,
                         frame->height / 2,
                         EGL_LINUX_DRM_FOURCC_EXT,
                         DRM_FORMAT_GR88,
                         EGL_DMA_BUF_PLANE0_FD_EXT,
                         fd_uv,
                         EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                         offset_uv,
                         EGL_DMA_BUF_PLANE0_PITCH_EXT,
                         stride_uv,
                         EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                         (EGLint)(modifier_uv & 0xFFFFFFFF),
                         EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
                         (EGLint)(modifier_uv >> 32),
                         EGL_NONE};

  result.image_uv = eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                      EGL_LINUX_DMA_BUF_EXT, NULL, attribs_uv);

  return result;
}

void calculate_transform(int id, float *m) {
  for (int i = 0; i < 16; i++)
    m[i] = 0.0f;
  m[0] = 1.0f;
  m[5] = 1.0f;
  m[10] = 1.0f;
  m[15] = 1.0f;

  m[0] = 0.5f;
  m[5] = 0.5f;

  float tx = (id % 2 == 0) ? -0.5f : 0.5f;
  float ty = (id < 2) ? 0.5f : -0.5f;

  m[12] = tx;
  m[13] = ty;
}

int init_player(VideoPlayer *vp, const char *filename, int id) {
  memset(vp, 0, sizeof(VideoPlayer));
  vp->id = id;
  vp->first_pts = AV_NOPTS_VALUE;
  vp->image_y = EGL_NO_IMAGE_KHR;
  vp->image_uv = EGL_NO_IMAGE_KHR;

  calculate_transform(id, vp->transform);

  // FFmpeg Init
  if (avformat_open_input(&vp->fmt_ctx, filename, NULL, NULL) < 0)
    return -1;
  if (avformat_find_stream_info(vp->fmt_ctx, NULL) < 0)
    return -1;

  vp->video_stream_idx = -1;
  for (int i = 0; i < vp->fmt_ctx->nb_streams; i++) {
    if (vp->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      vp->video_stream_idx = i;
      break;
    }
  }
  if (vp->video_stream_idx == -1)
    return -1;

  AVCodecParameters *codecpar =
      vp->fmt_ctx->streams[vp->video_stream_idx]->codecpar;
  const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
  vp->dec_ctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(vp->dec_ctx, codecpar);

  vp->video_time_base = vp->fmt_ctx->streams[vp->video_stream_idx]->time_base;
  vp->dec_ctx->get_format = get_hw_format;

  av_hwdevice_ctx_create(&vp->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL,
                         0);
  vp->dec_ctx->hw_device_ctx = av_buffer_ref(vp->hw_device_ctx);

  avcodec_open2(vp->dec_ctx, codec, NULL);

  vp->frame = av_frame_alloc();
  vp->pkt = av_packet_alloc();

  // Gen Textures
  glGenTextures(1, &vp->texY);
  glBindTexture(GL_TEXTURE_2D, vp->texY);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenTextures(1, &vp->texUV);
  glBindTexture(GL_TEXTURE_2D, vp->texUV);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  return 0;
}

void update_player(VideoPlayer *vp) {

  if (is_paused)
    return;
  if (vp->frame->width == 0) {
    int got_frame = 0;

    while (!got_frame) {
      int ret = av_read_frame(vp->fmt_ctx, vp->pkt);

      if (ret == AVERROR_EOF) {
        av_seek_frame(vp->fmt_ctx, vp->video_stream_idx, 0,
                      AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(vp->dec_ctx);
        vp->first_pts = AV_NOPTS_VALUE; // Reset clock trigger
        continue;
      }
      if (ret < 0)
        return; // Error

      if (vp->pkt->stream_index == vp->video_stream_idx) {
        if (avcodec_send_packet(vp->dec_ctx, vp->pkt) == 0) {
          if (avcodec_receive_frame(vp->dec_ctx, vp->frame) == 0) {
            got_frame = 1; // Success: vp->frame is now full
          }
        }
      }
      av_packet_unref(vp->pkt);
    }
  }

  if (vp->frame->width == 0)
    return;

  if (vp->first_pts == AV_NOPTS_VALUE) {
    vp->first_pts = vp->frame->pts;
    vp->start_time = get_time_sec() - total_pause_offset;
  }

  double current_time = get_time_sec();
  double master_clock = (current_time - vp->start_time) - total_pause_offset;
  double pts_sec =
      pts_to_seconds(vp->frame->pts - vp->first_pts, vp->video_time_base);
  if (pts_sec > master_clock) {
    return;
  }

  AVFrame *drm_frame = av_frame_alloc();
  drm_frame->format = AV_PIX_FMT_DRM_PRIME;

  if (av_hwframe_map(drm_frame, vp->frame, AV_HWFRAME_MAP_READ) == 0) {

    EGLDisplay disp = eglGetCurrentDisplay();
    NV12_EGLImages images = create_split_egl_images(disp, drm_frame);

    // Cleanup Old
    if (vp->image_y != EGL_NO_IMAGE_KHR)
      eglDestroyImageKHR(disp, vp->image_y);
    if (vp->image_uv != EGL_NO_IMAGE_KHR)
      eglDestroyImageKHR(disp, vp->image_uv);
    if (vp->current_drm_frame)
      av_frame_free(&vp->current_drm_frame);

    // Assign New
    vp->image_y = images.image_y;
    vp->image_uv = images.image_uv;
    vp->current_drm_frame = drm_frame;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vp->texY);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, vp->image_y);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, vp->texUV);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, vp->image_uv);
  } else {
    av_frame_free(&drm_frame);
  }

  av_frame_unref(vp->frame);
}

void render_player(VideoPlayer *vp, GLuint program) {
  if (vp->image_y == EGL_NO_IMAGE_KHR)
    return;

  GLint locTransform = glGetUniformLocation(program, "uTransform");
  glUniformMatrix4fv(locTransform, 1, GL_FALSE, vp->transform);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, vp->texY);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, vp->texUV);

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
