#pragma once

#define GLFW_INCLUDE_ES2
#include <GLFW/glfw3.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define VIDEO_W 640
#define VIDEO_H 480


void framebuffer_size_callback(GLFWwindow *window, int width, int height);
GLuint create_program(const char *vs, const char *fs);
void update_video_frame(GLuint tex_id);
int open_video(const char *filename);
char *read_file(const char *filename);
void upload_yuv_textures(AVFrame *frame, GLuint texY, GLuint texU, GLuint texV);
void update_yuv_video_frame(GLuint texY, GLuint texU, GLuint texV);
void upload_plane(GLuint texID, int width, int height, int linesize,
                  uint8_t *data);
int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type);
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts);
