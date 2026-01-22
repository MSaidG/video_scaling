#include "main.h"
#include "libavcodec/packet.h"
#include <libavutil/frame.h>
#include <libswscale/swscale.h> // Required for scaling
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

static inline double pts_to_seconds(int64_t pts) {
  return pts * av_q2d(video_time_base);
}

// Default startup size
int target_w = 800;
int target_h = 600;

// Flag to tell the video loop to resize buffers
int resize_pending = 1;

// Scaling State
struct SwsContext *scaler_ctx = NULL;
AVFrame *frame_scaled = NULL;

int init_converted_frame() {
  frame_scaled = av_frame_alloc();
  if (!frame_scaled)
    return -1;

  frame_scaled->format = AV_PIX_FMT_YUV420P;
  frame_scaled->width = target_w;
  frame_scaled->height = target_h;

  // Allocate the buffer for the scaled frame
  if (av_frame_get_buffer(frame_scaled, 32) < 0) {
    fprintf(stderr, "Could not allocate buffer for scaled frame\n");
    return -1;
  }
  return 0;
}

// Helper to reset textures and buffers when window size changes
// Returns 0 on success, -1 on error
int reconfigure_scaler_and_textures(GLuint texY, GLuint texU, GLuint texV) {

  // 1. Free existing CPU scaler frame
  if (frame_scaled) {
    av_frame_free(&frame_scaled);
  }

  // 2. Free existing SwsContext (it locks in the old resolution)
  if (scaler_ctx) {
    sws_freeContext(scaler_ctx);
    scaler_ctx = NULL; // Force re-creation next frame
  }

  // 3. Allocate new CPU frame
  frame_scaled = av_frame_alloc();
  frame_scaled->format = AV_PIX_FMT_YUV420P;
  frame_scaled->width = target_w;
  frame_scaled->height = target_h;

  if (av_frame_get_buffer(frame_scaled, 32) < 0) {
    return -1;
  }

  // 4. Resize OpenGL Textures (Re-allocate storage with glTexImage2D)
  // We pass NULL pixels because we just want to resize the GPU memory

  // Y Plane (Full res)
  glBindTexture(GL_TEXTURE_2D, texY);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, target_w, target_h, 0,
               GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

  // U Plane (Half res)
  glBindTexture(GL_TEXTURE_2D, texU);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, target_w / 2, target_h / 2, 0,
               GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

  // V Plane (Half res)
  glBindTexture(GL_TEXTURE_2D, texV);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, target_w / 2, target_h / 2, 0,
               GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);

  return 0;
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

  if (open_video("videos/zoo.mp4") < 0) {
    fprintf(stderr, "Error: Exiting because video couldnt be opened.\n");
    return -1;
  }

  if (init_converted_frame() < 0)
    return -1;

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  GLuint texY, texU, texV;
  glGenTextures(1, &texY);
  glGenTextures(1, &texU);
  glGenTextures(1, &texV);

  // Helper to init YUV textures
  GLuint texs[3] = {texY, texU, texV};
  int dims[3][2] = {{target_w, target_h},
                    {target_w / 2, target_h / 2},
                    {target_w / 2, target_h / 2}};

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

  // Get uniform locations
  GLint locY = glGetUniformLocation(program, "uTexY");
  GLint locU = glGetUniformLocation(program, "uTexU");
  GLint locV = glGetUniformLocation(program, "uTexV");

  glEnableVertexAttribArray(aPos);
  glEnableVertexAttribArray(aTex);

  glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), quad);
  glVertexAttribPointer(aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        quad + 2);

  while (!glfwWindowShouldClose(window)) {

    update_yuv_video_frame(texY, texU, texV);

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);

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

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glfwSwapBuffers(window);
    glfwPollEvents();
  }

  if (scaler_ctx) sws_freeContext(scaler_ctx);
  if (frame_scaled) av_frame_free(&frame_scaled);

  glfwTerminate();
  return 0;
}

void framebuffer_size_callback(GLFWwindow *window, int width, int height) {
  glViewport(0, 0, width, height);

  // Prevent 0 divide if minimized
  if (width > 0 && height > 0) {
    target_w = width;
    target_h = height;
    resize_pending = 1; // Signal the loop to rebuild buffers
  }
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

  video_time_base = fmt_ctx->streams[video_stream_idx]->time_base;

  const AVCodec *codec = avcodec_find_decoder(
      fmt_ctx->streams[video_stream_idx]->codecpar->codec_id);
  dec_ctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(dec_ctx,
                                fmt_ctx->streams[video_stream_idx]->codecpar);

  // --- FOR HWACCEL ---
  // Try VAAPI (standard for Linux/Fedora)
  enum AVHWDeviceType type =
      av_hwdevice_find_type_by_name("vaapi"); // "drm" or "v4l2m2m"
  if (type != AV_HWDEVICE_TYPE_NONE) {
    // Find the format the hardware uses
    printf("HW_TYPE: %d\n", type);
    fflush(stdout);

    for (int i = 0;; i++) {
      const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
      if (!config)
        break;
      if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
          config->device_type == type) {
        hw_pix_fmt = config->pix_fmt;
        break;
      }
    }
    dec_ctx->get_format = get_hw_format;
    hw_decoder_init(dec_ctx, type);
  }
  // ----------------------------

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

void update_yuv_video_frame(GLuint texY, GLuint texU, GLuint texV) {
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

    if (pkt->stream_index == video_stream_idx) {
      if (avcodec_send_packet(dec_ctx, pkt) == 0) {
        if (avcodec_receive_frame(dec_ctx, frame) == 0) {

          AVFrame *use_frame = frame;
          AVFrame *sw_frame = NULL;

          if (frame->format == hw_pix_fmt) {
            sw_frame = av_frame_alloc();
            if (av_hwframe_transfer_data(sw_frame, frame, 0) >= 0) {
              // Copy metadata
              sw_frame->pts = frame->pts;
              use_frame = sw_frame; // Now use_frame is the CPU data
            } else {
              av_frame_free(&sw_frame);
              av_packet_unref(pkt);
              return;
            }
          }

          // --- DYNAMIC RESIZING CHECK ---
          if (resize_pending) {
              reconfigure_scaler_and_textures(texY, texU, texV);
              resize_pending = 0;
          }
          // ------------------------------

          // 2. Initialize Scaler (Lazy initialization)
          // We do this here because we need the EXACT source format/dimensions
          if (!scaler_ctx) {
            scaler_ctx =
                sws_getContext(use_frame->width, use_frame->height,
                               use_frame->format,                      // Source
                               target_w, target_h, AV_PIX_FMT_YUV420P, // Target
                               SWS_BILINEAR, NULL, NULL, NULL // Algorithm
                );
          }

          // 3. Perform CPU Scaling
          // Converts 'use_frame' -> 'frame_scaled'
          sws_scale(scaler_ctx, (const uint8_t *const *)use_frame->data,
                    use_frame->linesize, 0, use_frame->height,
                    frame_scaled->data, frame_scaled->linesize);

          // 4. Time Sync (Logic unchanged)
          if (first_pts == AV_NOPTS_VALUE) {
            first_pts = use_frame->pts;
            video_start_time = glfwGetTime();
          }

          double frame_time = pts_to_seconds(use_frame->pts - first_pts);
          double now = glfwGetTime() - video_start_time;
          double delay = frame_time - now;

          if (delay > 0.0) {
            struct timespec ts;
            ts.tv_sec = (time_t)delay;
            ts.tv_nsec = (long)((delay - ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
          }

          // -------- Upload --------
          upload_yuv_textures(frame_scaled, texY, texU, texV);

          if (sw_frame)
            av_frame_free(&sw_frame);

          av_packet_unref(pkt);
          return;
        }
        // ----------------------------
        av_packet_unref(pkt);
      }
    }

    // If it was audio or no frame ready, clean up and LOOP AGAIN
    av_packet_unref(pkt);
  }
}

void upload_plane(GLuint texID, int width, int height, int linesize,
                  uint8_t *data) {
  // --- FOR HWACCEL ---
  if (!data) {
    fprintf(stderr, "Error: Plane data is NULL for texID %d\n", texID);
    return;
  }
  // ----------------------------

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

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts) {
  for (const enum AVPixelFormat *p = pix_fmts; *p != -1; p++) {
    if (*p == hw_pix_fmt)
      return *p;
  }
  fprintf(stderr, "Failed to get HW surface format.\n");
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
