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
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
#include <drm/drm_fourcc.h>

#define VIDEO_W 800
#define VIDEO_H 600


void update_video_frame(GLuint tex_id);
int open_video(const char *filename);
// int open_video(const char *filename, AVFormatContext *fmt_ctx, AVCodecContext *dec_ctx);

void upload_yuv_textures(AVFrame *frame, GLuint texY, GLuint texU, GLuint texV);
void update_yuv_video_frame(GLuint texY, GLuint texU, GLuint texV);
void upload_plane(GLuint texID, int width, int height, int linesize,
                  uint8_t *data);
int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type);
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts);

double pts_to_seconds(int64_t pts);
static AVDRMFrameDescriptor *get_drm_desc(AVFrame *frame);


// Maps a DRM frame to an EGLImage for zero-copy access
// void update_video_frame_egl(GLuint texID);
void update_video_frame_egl(GLuint texY, GLuint texUV);
EGLImageKHR create_eglimage_plane(EGLDisplay disp, AVFrame *frame, int plane_index, int format);
