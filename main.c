#define GLFW_INCLUDE_ES2
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#define VIDEO_W 640
#define VIDEO_H 480

// FFmpeg Global State
AVFormatContext *fmt_ctx = NULL;
AVCodecContext *dec_ctx = NULL;
struct SwsContext *sws_ctx = NULL;
AVFrame *frame = NULL;
AVFrame *frame_rgb = NULL;
AVPacket *pkt = NULL;
int video_stream_idx = -1;
uint8_t *buffer = NULL;

void framebuffer_size_callback(GLFWwindow *window, int width, int height);

const char *vs_src = "attribute vec2 aPos;\n"
                     "attribute vec2 aTex;\n"
                     "varying vec2 vTex;\n"
                     "void main() {\n"
                     "  vTex = aTex;\n"
                     "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
                     "}\n";

const char *fs_src = "precision mediump float;\n"
                     "varying vec2 vTex;\n"
                     "uniform sampler2D uTex;\n"
                     "void main() {\n"
                     "  gl_FragColor = texture2D(uTex, vTex);\n"
                     "}\n";

// Position (x,y)   Texcoord (u,v)
GLfloat quad[] = {
    -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 0.0f,
};

GLuint compile_shader(GLenum type, const char *src) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, NULL);
  glCompileShader(shader);

  GLint ok;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(shader, 512, NULL, log);
    printf("Shader compile error: %s\n", log);
  }
  return shader;
}

GLuint create_program(const char *vs, const char *fs) {
  GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
  GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);

  GLuint program = glCreateProgram();
  glAttachShader(program, v);
  glAttachShader(program, f);
  glLinkProgram(program);

  GLint ok;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetProgramInfoLog(program, 512, NULL, log);
    printf("Program link error: %s\n", log);
  }

  glDeleteShader(v);
  glDeleteShader(f);
  return program;
}

unsigned char *create_test_frame_data() {
  unsigned char *frame_data = malloc(VIDEO_W * VIDEO_H * 3);

  for (int y = 0; y < VIDEO_H; y++) {
    for (int x = 0; x < VIDEO_W; x++) {
      int i = (y * VIDEO_W + x) * 3;
      frame_data[i + 0] = x % 256;       // R
      frame_data[i + 1] = y % 256;       // G
      frame_data[i + 2] = (x + y) % 256; // B
    }
  }

  return frame_data;
}

int open_video(const char *filename) {

  fmt_ctx = NULL;

  if (avformat_open_input(&fmt_ctx, filename, NULL, NULL) < 0) {
    fprintf(stderr, "Error: Could not open file %s\n", filename);
    return -1;
  }
  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
    fprintf(stderr, "Error: Could not find stream information\n");
    return -1;
  }

  // Find video stream
  video_stream_idx = -1;
  for (int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_idx = i;
      break;
    }
  }

  if (video_stream_idx == -1) {
    fprintf(stderr, "Error: Could not find a video stream\n");
    return -1;
  }

  const AVCodec *codec = avcodec_find_decoder(
      fmt_ctx->streams[video_stream_idx]->codecpar->codec_id);
  dec_ctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(dec_ctx,
                                fmt_ctx->streams[video_stream_idx]->codecpar);
  avcodec_open2(dec_ctx, codec, NULL);

  // Prepare RGB frame for OpenGL
  frame = av_frame_alloc();
  frame_rgb = av_frame_alloc();
  int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, dec_ctx->width,
                                          dec_ctx->height, 1);
  buffer = (uint8_t *)av_malloc(numBytes * sizeof(uint8_t));
  av_image_fill_arrays(frame_rgb->data, frame_rgb->linesize, buffer,
                       AV_PIX_FMT_RGB24, dec_ctx->width, dec_ctx->height, 1);

  // Setup Scaler: Video Native Format -> RGB24
  sws_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                           dec_ctx->width, dec_ctx->height, AV_PIX_FMT_RGB24,
                           SWS_BILINEAR, NULL, NULL, NULL);
  pkt = av_packet_alloc();
  return 0;
}

void update_video_frame(GLuint tex_id) {
  if (av_read_frame(fmt_ctx, pkt) >= 0) {
    if (pkt->stream_index == video_stream_idx) {
      avcodec_send_packet(dec_ctx, pkt);
      if (avcodec_receive_frame(dec_ctx, frame) == 0) {
        // Convert YUV (or other) to RGB
        sws_scale(sws_ctx, (uint8_t const *const *)frame->data, frame->linesize,
                  0, dec_ctx->height, frame_rgb->data, frame_rgb->linesize);

        // Push new pixels to OpenGL
        glBindTexture(GL_TEXTURE_2D, tex_id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dec_ctx->width, dec_ctx->height,
                        GL_RGB, GL_UNSIGNED_BYTE, frame_rgb->data[0]);
      }
    }
    av_packet_unref(pkt);
  }
}

int main(int argc, char **argv) {

  glfwInit();
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

  GLFWwindow *window = glfwCreateWindow(800, 600, "SCALING", NULL, NULL);
  if (window == NULL) {
    printf("Failed to create GLFW window\n");
    glfwTerminate();
    return -1;
  }

  glfwMakeContextCurrent(window);

  glViewport(0, 0, 800, 600);
  glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);

  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (open_video("zoo.mp4") < 0) {
    fprintf(stderr, "Error: Exiting because video couldnt be opened.\n");
    return -1;
  }
  // Pre-allocate the texture memory once
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, VIDEO_W, VIDEO_H, 0, GL_RGB,
               GL_UNSIGNED_BYTE, NULL);
  // Example: 640x480 RGB frame
  // glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, VIDEO_W, VIDEO_H, 0, GL_RGB,
  //  GL_UNSIGNED_BYTE,
  //  create_test_frame_data()); // later from decoder

  GLuint program = create_program(vs_src, fs_src);

  GLint aPos = glGetAttribLocation(program, "aPos");
  GLint aTex = glGetAttribLocation(program, "aTex");

  glEnableVertexAttribArray(aPos);
  glEnableVertexAttribArray(aTex);

  glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);

  glVertexAttribPointer(aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        quad + 2);

  while (!glfwWindowShouldClose(window)) {

    update_video_frame(tex);

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program);
    glBindTexture(GL_TEXTURE_2D, tex);
    /* bind attributes, draw quad */
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glfwSwapBuffers(window);
    glfwPollEvents();
  }

  glfwTerminate();
  return 0;
}

void framebuffer_size_callback(GLFWwindow *window, int width, int height) {
  glViewport(0, 0, width, height);
}
