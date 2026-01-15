#include "main.h"
#include "gl.h"
#include "utils.h"
#include <time.h>

// FFmpeg Global State
AVFormatContext *fmt_ctx = NULL;
AVCodecContext *dec_ctx = NULL;
struct SwsContext *sws_ctx = NULL;

AVFrame *frame = NULL;
AVFrame *frame_yuv_clean = NULL;
AVPacket *pkt = NULL;

AVRational video_time_base;
double video_start_time = 0.0;
int64_t first_pts = AV_NOPTS_VALUE;

int video_stream_idx = -1;

// Position (x,y)   Texcoord (u,v)
GLfloat quad[] = {
    -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 0.0f,
};

// HW_ACCEL
AVBufferRef *hw_device_ctx = NULL;
enum AVPixelFormat hw_pix_fmt;

// EGL ZERO-COPY
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

typedef struct {
  EGLImageKHR image_y;
  EGLImageKHR image_uv;
} NV12_EGLImages;

NV12_EGLImages create_split_egl_images(EGLDisplay display,
                                       const AVFrame *frame) {
  NV12_EGLImages result = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};

  if (frame->format != AV_PIX_FMT_DRM_PRIME)
    return result;

  const AVDRMFrameDescriptor *desc =
      (const AVDRMFrameDescriptor *)frame->data[0];

  // --- Y PLANE SETUP ---
  int layer_y = 0;
  int plane_y = 0;
  int obj_y_idx = desc->layers[layer_y].planes[plane_y].object_index;
  int fd_y = desc->objects[obj_y_idx].fd;
  int offset_y = desc->layers[layer_y].planes[plane_y].offset;
  int stride_y = desc->layers[layer_y].planes[plane_y].pitch;
  uint64_t modifier_y = desc->objects[obj_y_idx].format_modifier;

  EGLint attribs_y[] = {
      EGL_WIDTH, frame->width, EGL_HEIGHT, frame->height,
      EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8, EGL_DMA_BUF_PLANE0_FD_EXT, fd_y,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset_y, EGL_DMA_BUF_PLANE0_PITCH_EXT,
      stride_y,
      // PASS MODIFIERS!
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(modifier_y & 0xFFFFFFFF),
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(modifier_y >> 32), EGL_NONE};

  result.image_y = eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT, NULL, attribs_y);

  // --- UV PLANE SETUP ---
  // Handle case where UV is in same layer (NV12 standard) or different layer
  int layer_uv = (desc->nb_layers > 1) ? 1 : 0;
  int plane_uv = (desc->nb_layers > 1) ? 0 : 1;
  int obj_uv_idx = desc->layers[layer_uv].planes[plane_uv].object_index;

  int fd_uv = desc->objects[obj_uv_idx].fd;
  int offset_uv = desc->layers[layer_uv].planes[plane_uv].offset;
  int stride_uv = desc->layers[layer_uv].planes[plane_uv].pitch;
  uint64_t modifier_uv = desc->objects[obj_uv_idx].format_modifier;

  EGLint attribs_uv[] = {
      EGL_WIDTH, frame->width / 2, EGL_HEIGHT, frame->height / 2,
      EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88, EGL_DMA_BUF_PLANE0_FD_EXT,
      fd_uv, EGL_DMA_BUF_PLANE0_OFFSET_EXT, offset_uv,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, stride_uv,
      // PASS MODIFIERS!
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(modifier_uv & 0xFFFFFFFF),
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(modifier_uv >> 32),
      EGL_NONE};

  result.image_uv = eglCreateImageKHR(display, EGL_NO_CONTEXT,
                                      EGL_LINUX_DMA_BUF_EXT, NULL, attribs_uv);

  return result;
}

int main(int argc, char **argv) {

  GLFWwindow *window = initGLFW(VIDEO_W, VIDEO_H);
  if (window == NULL)
    return -1;

  // Initialize EGL Extension functions
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

  if (open_video("videos/1080p_.mp4") < 0) {
    fprintf(stderr, "Error: Exiting because video couldnt be opened.\n");
    return -1;
  }
  char *vs_code = read_file("shader.vs");
  char *fs_code = read_file("shader.fs");

  GLuint program;
  if (vs_code && fs_code) {
    program = create_program(vs_code, fs_code);
    free(vs_code);
    free(fs_code);
  } else {
    fprintf(stderr, "Shaders couldnt opened!");
    return -1;
  }

  GLint aPos = glGetAttribLocation(program, "aPos");
  GLint aTex = glGetAttribLocation(program, "aTex");
  GLint locTex = glGetUniformLocation(program, "uTexture");

  GLuint texY, texUV;
  glGenTextures(1, &texY);
  glBindTexture(GL_TEXTURE_2D, texY);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenTextures(1, &texUV);
  glBindTexture(GL_TEXTURE_2D, texUV);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  while (!glfwWindowShouldClose(window)) {

    update_video_frame_egl(texY, texUV);

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texY);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texUV);

    glUniform1i(glGetUniformLocation(program, "uTextureY"), 0);  // Tex Unit 0
    glUniform1i(glGetUniformLocation(program, "uTextureUV"), 1); // Tex Unit 1

    glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
    glVertexAttribPointer(aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          quad + 2);
    glEnableVertexAttribArray(aPos);
    glEnableVertexAttribArray(aTex);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glfwSwapBuffers(window);
    glfwPollEvents();
  }

  glfwTerminate();
  return 0;
}

void update_video_frame_egl(GLuint texY, GLuint texUV) {

  static AVFrame *current_drm_frame = NULL;

  while (1) {
    int ret = av_read_frame(fmt_ctx, pkt);

    // Handle End of File (Loop video)
    if (ret == AVERROR_EOF) {
      av_seek_frame(fmt_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
      avcodec_flush_buffers(dec_ctx);
      first_pts = AV_NOPTS_VALUE;
      video_start_time = glfwGetTime();
      continue; // Try reading again from start
    }

    if (ret < 0)
      break; // Error or other issue

    if (pkt->stream_index != video_stream_idx) {
      av_packet_unref(pkt);
      continue;
    }

    if (avcodec_send_packet(dec_ctx, pkt) < 0) {
      av_packet_unref(pkt);
      continue;
    }

    if (avcodec_receive_frame(dec_ctx, frame) == 0) {

      /* EXPECT VAAPI */
      if (frame->format != AV_PIX_FMT_VAAPI) {
        fprintf(stderr, "Unexpected frame format: %d\n", frame->format);
        av_frame_unref(frame);
        av_packet_unref(pkt);
        return;
      }

      /* MAP VAAPI → DRM_PRIME */
      AVFrame *drm_frame = av_frame_alloc();
      drm_frame->format = AV_PIX_FMT_DRM_PRIME;

      if (av_hwframe_map(drm_frame, frame, AV_HWFRAME_MAP_READ) < 0) {
        fprintf(stderr, "av_hwframe_map failed\n");
        av_frame_free(&drm_frame);
        av_frame_unref(frame);
        av_packet_unref(pkt);
        return;
      }

      // printf("Mapped format: %s\n", av_get_pix_fmt_name(drm_frame->format));

      if (drm_frame->format != AV_PIX_FMT_DRM_PRIME) {
        fprintf(stderr, "Mapped frame is not DRM_PRIME\n");
        av_frame_free(&drm_frame);
        av_frame_unref(frame);
        av_packet_unref(pkt);
        return;
      }

      // -------- Frame timing --------
      if (first_pts == AV_NOPTS_VALUE) {
        first_pts = frame->pts;
        video_start_time = glfwGetTime();
      }

      double frame_time = pts_to_seconds(frame->pts - first_pts);
      double now = glfwGetTime() - video_start_time;
      double delay = frame_time - now;

      if (delay > 0.0) {
        struct timespec ts;
        ts.tv_sec = (time_t)delay;
        ts.tv_nsec = (long)((delay - ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
      }
      // -----------------------------

      EGLDisplay disp = eglGetCurrentDisplay();
      NV12_EGLImages images = create_split_egl_images(disp, drm_frame);
      // 1. Destroy OLD EGL Images
      static EGLImageKHR last_imgY = EGL_NO_IMAGE_KHR;
      static EGLImageKHR last_imgUV = EGL_NO_IMAGE_KHR;
      if (last_imgY != EGL_NO_IMAGE_KHR)
        eglDestroyImageKHR(disp, last_imgY);
      if (last_imgUV != EGL_NO_IMAGE_KHR)
        eglDestroyImageKHR(disp, last_imgUV);

      // 2. Free the OLD frame (now that we are done displaying it)
      if (current_drm_frame) {
        av_frame_free(&current_drm_frame);
      }

      // 3. Update Textures with NEW images
      if (images.image_y != EGL_NO_IMAGE_KHR) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texY);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, images.image_y);
        last_imgY = images.image_y;
      }

      if (images.image_uv != EGL_NO_IMAGE_KHR) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texUV);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, images.image_uv);
        last_imgUV = images.image_uv;
      }

      // 4. Save the NEW frame so we don't free it yet!
      current_drm_frame = drm_frame;

      // Clean up the CPU-side reference, but KEEP current_drm_frame alive
      av_frame_unref(frame);
      av_packet_unref(pkt);
      return;
    }

    av_packet_unref(pkt);
  }
}

int open_video(const char *filename) {
  avformat_open_input(&fmt_ctx, filename, NULL, NULL);
  avformat_find_stream_info(fmt_ctx, NULL);

  for (int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_idx = i;
      break;
    }
  }

  video_time_base = fmt_ctx->streams[video_stream_idx]->time_base;

  const AVCodec *codec = avcodec_find_decoder(
      fmt_ctx->streams[video_stream_idx]->codecpar->codec_id);
  dec_ctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(dec_ctx,
                                fmt_ctx->streams[video_stream_idx]->codecpar);

  video_time_base = fmt_ctx->streams[video_stream_idx]->time_base;
  dec_ctx->get_format = get_hw_format;

  // Initialize VAAPI Device
  av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
  dec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

  avcodec_open2(dec_ctx, codec, NULL);
  frame = av_frame_alloc();
  pkt = av_packet_alloc();
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

int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type) {
  int err = 0;
  // For Fedora, AV_HWDEVICE_TYPE_VAAPI. For Embedded, AV_HWDEVICE_TYPE_DRM.
  if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type, NULL, NULL, 0)) < 0) {
    return err;
  }
  ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
  return 0;
}

double pts_to_seconds(int64_t pts) { return pts * av_q2d(video_time_base); }

static AVDRMFrameDescriptor *get_drm_desc(AVFrame *frame) {
  return (AVDRMFrameDescriptor *)frame->data[0];
}

EGLImageKHR create_eglimage_plane(EGLDisplay disp, AVFrame *frame,
                                  int plane_index, int format) {
  AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
  AVDRMLayerDescriptor *layer = &desc->layers[0];

  EGLint attribs[64]; // Increased size to be safe
  int k = 0;

  // 1. Calculate Dimensions
  int width =
      frame->width; // (plane_index == 0) ? frame->width : frame->width / 2;
  int height =
      frame->height; // (plane_index == 0) ? frame->height : frame->height / 2;

  attribs[k++] = EGL_WIDTH;
  attribs[k++] = width;
  attribs[k++] = EGL_HEIGHT;
  attribs[k++] = height;
  attribs[k++] = EGL_LINUX_DRM_FOURCC_EXT;
  attribs[k++] = format;

  // 2. Get Object Info (FD and Modifier)
  int obj_idx = layer->planes[0].object_index;
  int fd = desc->objects[obj_idx].fd;
  uint64_t modifier = desc->objects[obj_idx].format_modifier; // <--- CRITICAL

  attribs[k++] = EGL_DMA_BUF_PLANE0_FD_EXT;
  attribs[k++] = fd;
  attribs[k++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
  attribs[k++] = layer->planes[0].offset;
  attribs[k++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
  attribs[k++] = layer->planes[0].pitch;

  // 3. APPLY MODIFIER (Fixes "Failed to create EGLImage")
  // If the driver provided a modifier (e.g. I915_Y_TILED), we MUST pass it.
  if (modifier != DRM_FORMAT_MOD_INVALID) {
    attribs[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attribs[k++] = modifier & 0xFFFFFFFF;
    attribs[k++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attribs[k++] = modifier >> 32;
  }

  attribs[k++] = EGL_NONE;

  return eglCreateImageKHR(disp, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                           attribs);

  if (plane_index == 0) {
  }
}
