#pragma once

#define GLFW_INCLUDE_ES2
#include <GLFW/glfw3.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> // Required for GL_TEXTURE_EXTERNAL_OES
#include <drm/drm_fourcc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>

#define VIDEO_W 800
#define VIDEO_H 600

typedef struct {
  int id; // 0 to 3

  // FFmpeg State
  AVFormatContext *fmt_ctx;
  AVCodecContext *dec_ctx;
  AVBufferRef *hw_device_ctx;
  int video_stream_idx;
  AVRational video_time_base;

  // Playback State
  double start_time; // Wall clock time when video started
  int64_t first_pts; // PTS of the first frame
  AVFrame *frame;
  AVPacket *pkt;
  AVFrame *current_drm_frame; // Keep ref to currently displayed frame

  // OpenGL/EGL State
  GLuint texY;
  GLuint texUV;
  EGLImageKHR image_y;
  EGLImageKHR image_uv;

  // Position (2x2 Grid)
  float transform[16];

} VideoPlayer;

typedef struct {
  EGLImageKHR image_y;
  EGLImageKHR image_uv;
} NV12_EGLImages;



int init_player(VideoPlayer *vp, const char *filename, int id);
void update_player(VideoPlayer *vp);
void render_player(VideoPlayer *vp, GLuint program);
void calculate_transform(int id, float *m);
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts);
