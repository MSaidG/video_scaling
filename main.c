#define GLFW_INCLUDE_ES2
#include <GLFW/glfw3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
AVFrame *frame_yuv_clean = NULL;
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

GLuint create_program(const char *vs, const char *fs);
void update_video_frame(GLuint tex_id);
int open_video(const char *filename);
char *read_file(const char *filename);
void upload_yuv_textures(AVFrame *frame, GLuint texY, GLuint texU, GLuint texV);
void update_yuv_video_frame(GLuint texY, GLuint texU, GLuint texV);
void upload_plane(GLuint texID, int width, int height, int linesize, uint8_t *data);

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

  if (open_video("zoo.mp4") < 0) {
    fprintf(stderr, "Error: Exiting because video couldnt be opened.\n");
    return -1;
  }

  //
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  GLuint texY, texU, texV;
  glGenTextures(1, &texY);
  glGenTextures(1, &texU);
  glGenTextures(1, &texV);

  // Helper to init YUV textures
  GLuint texs[3] = {texY, texU, texV};
  int dims[3][2] = {{dec_ctx->width, dec_ctx->height},
                    {dec_ctx->width / 2, dec_ctx->height / 2},
                    {dec_ctx->width / 2, dec_ctx->height / 2}};

  for (int i = 0; i < 3; i++) {
    glBindTexture(GL_TEXTURE_2D, texs[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, dims[i][0], dims[i][1], 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  //

  char *vs_code = read_file("shader.vs");
  char *fs_code = read_file("shader.fs");

  GLuint program;
  if (vs_code && fs_code) {
    program = create_program(vs_code, fs_code);
    free(vs_code);
    free(fs_code);
  } else {
    program = create_program(vs_src, fs_src);
  }

  GLint aPos = glGetAttribLocation(program, "aPos");
  GLint aTex = glGetAttribLocation(program, "aTex");

  //
  // Get uniform locations
  GLint locY = glGetUniformLocation(program, "uTexY");
  GLint locU = glGetUniformLocation(program, "uTexU");
  GLint locV = glGetUniformLocation(program, "uTexV");
  //

  glEnableVertexAttribArray(aPos);
  glEnableVertexAttribArray(aTex);

  glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
  glVertexAttribPointer(aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        quad + 2);

  double last_time = glfwGetTime();
  double target_fps = 30.0; // Change to your video's FPS
  double time_per_frame = 1.0 / target_fps;
  while (!glfwWindowShouldClose(window)) {

    double current_time = glfwGetTime();

    if (current_time - last_time >= time_per_frame) {
      // update_video_frame(tex);
      update_yuv_video_frame(texY, texU, texV);
      last_time = current_time;
    }

    // glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);

    //
    // 1. Bind Textures to Units 0, 1, 2
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texY);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texU);
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, texV);

    // 2. Tell Shader which Unit corresponds to which Uniform
    glUniform1i(locY, 0); 
    glUniform1i(locU, 1); 
    glUniform1i(locV, 2);
    //

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // glBindTexture(GL_TEXTURE_2D, tex);
    /* bind attributes, draw quad */

    glfwSwapBuffers(window);
    glfwPollEvents();
  }

  glfwTerminate();
  return 0;
}

void framebuffer_size_callback(GLFWwindow *window, int width, int height) {
  glViewport(0, 0, width, height);
}

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
  frame_yuv_clean = av_frame_alloc();
  int size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, dec_ctx->width,
                                      dec_ctx->height, 1);
  uint8_t *internal_buffer = av_malloc(size);
  av_image_fill_arrays(frame_yuv_clean->data, frame_yuv_clean->linesize,
                       internal_buffer, AV_PIX_FMT_YUV420P, dec_ctx->width,
                       dec_ctx->height, 1);

  sws_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                           dec_ctx->width, dec_ctx->height, AV_PIX_FMT_YUV420P,
                           SWS_POINT, NULL, NULL, NULL);
  pkt = av_packet_alloc();
  return 0;
}

void update_video_frame(GLuint tex_id) {
  // if (av_read_frame(fmt_ctx, pkt) >= 0) {
  //   if (pkt->stream_index == video_stream_idx) {
  //     avcodec_send_packet(dec_ctx, pkt);
  //     if (avcodec_receive_frame(dec_ctx, frame) == 0) {
  //       // Convert YUV (or other) to RGB
  //       sws_scale(sws_ctx, (uint8_t const *const *)frame->data,
  //       frame->linesize,
  //                 0, dec_ctx->height, frame_rgb->data, frame_rgb->linesize);

  //       // Push new pixels to OpenGL
  //       glBindTexture(GL_TEXTURE_2D, tex_id);
  //       glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dec_ctx->width,
  //       dec_ctx->height,
  //                       GL_RGB, GL_UNSIGNED_BYTE, frame_rgb->data[0]);
  //     }
  //   }
  //   av_packet_unref(pkt);
  // }

  int ret = av_read_frame(fmt_ctx, pkt);

  if (ret == AVERROR_EOF) {
    // 1. Seek back to the beginning of the stream
    // AVSEEK_FLAG_BACKWARD helps find the nearest keyframe before the start
    av_seek_frame(fmt_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);

    // 2. Flush the decoder buffers so it doesn't try to mix old and new frames
    avcodec_flush_buffers(dec_ctx);

    // 3. Try reading again
    ret = av_read_frame(fmt_ctx, pkt);
  }

  if (ret >= 0) {
    if (pkt->stream_index == video_stream_idx) {
      if (avcodec_send_packet(dec_ctx, pkt) == 0) {
        while (avcodec_receive_frame(dec_ctx, frame) == 0) {
          // Convert and upload
          sws_scale(sws_ctx, (uint8_t const *const *)frame->data,
                    frame->linesize, 0, dec_ctx->height, frame_rgb->data,
                    frame_rgb->linesize);

          glBindTexture(GL_TEXTURE_2D, tex_id);
          glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dec_ctx->width,
                          dec_ctx->height, GL_RGB, GL_UNSIGNED_BYTE,
                          frame_rgb->data[0]);
        }
      }
    }
    av_packet_unref(pkt);
  }
}

void update_yuv_video_frame(GLuint texY, GLuint texU, GLuint texV) {
    while (1) {
        int ret = av_read_frame(fmt_ctx, pkt);

        // Handle End of File (Loop video)
        if (ret == AVERROR_EOF) {
            av_seek_frame(fmt_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(dec_ctx);
            continue; // Try reading again from start
        }
        
        if (ret < 0) break; // Error or other issue

        if (pkt->stream_index == video_stream_idx) {
            // Send packet to decoder
            if (avcodec_send_packet(dec_ctx, pkt) == 0) {
                // Try to retrieve a frame
                if (avcodec_receive_frame(dec_ctx, frame) == 0) {
                    // SUCCESS: We got a video frame! Upload and stop looking.
                    upload_yuv_textures(frame, texY, texU, texV);
                    av_packet_unref(pkt);
                    return; // Exit function, we are done for this frame
                }
            }
        }
        
        // If it was audio or no frame ready, clean up and LOOP AGAIN
        av_packet_unref(pkt);
    }
}

void upload_plane(GLuint texID, int width, int height, int linesize,
                  uint8_t *data) {
  glBindTexture(GL_TEXTURE_2D, texID);

  // If the data is already tightly packed (linesize == width), upload directly
  if (linesize == width) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_LUMINANCE,
                    GL_UNSIGNED_BYTE, data);
  } else {
    // If there is padding, we must copy row-by-row to a temporary packed buffer
    // Note: For high performance, you should allocate this buffer once and
    // reuse it instead of malloc/free every frame.
    unsigned char *packed_data = (unsigned char *)malloc(width * height);

    if (packed_data) {
      for (int i = 0; i < height; i++) {
        // Copy only the visible width, skip the padding (linesize - width)
        memcpy(packed_data + (i * width), data + (i * linesize), width);
      }

      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_LUMINANCE,
                      GL_UNSIGNED_BYTE, packed_data);

      free(packed_data);
    }
  }
}

void upload_yuv_textures(AVFrame *frame, GLuint texY, GLuint texU,
                         GLuint texV) {
  // Helper function to upload a single plane handling stride

  // 1. Upload Y plane (Luma)
  // For Y plane, width and height are the full video dimensions
  upload_plane(texY, frame->width, frame->height, frame->linesize[0],
               frame->data[0]);

  // 2. Upload U plane (Chroma Blue)
  // For YUV420P, U/V dimensions are usually half the width/height
  upload_plane(texU, frame->width / 2, frame->height / 2, frame->linesize[1],
               frame->data[1]);

  // 3. Upload V plane (Chroma Red)
  upload_plane(texV, frame->width / 2, frame->height / 2, frame->linesize[2],
               frame->data[2]);
}

char *read_file(const char *filename) {
  FILE *f = fopen(filename, "rb");
  if (!f) {
    fprintf(stderr, "Could not open shader file: %s\n", filename);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long length = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buffer = malloc(length + 1);
  fread(buffer, 1, length, f);
  fclose(f);
  buffer[length] = '\0';
  return buffer;
}
