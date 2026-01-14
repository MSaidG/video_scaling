#include "main.h"
#include "libavcodec/packet.h"
#include <time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> // Required for GL_TEXTURE_EXTERNAL_OES
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>

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

static AVDRMFrameDescriptor *get_drm_desc(AVFrame *frame) {
  return (AVDRMFrameDescriptor *)frame->data[0];
}

PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

// Maps a DRM frame to an EGLImage for zero-copy access
EGLImageKHR create_eglimage_from_frame(EGLDisplay egl_display, AVFrame *frame) {
  // In a DRM_PRIME frame, data[0] contains the descriptor
  AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
  if (!desc)
    return EGL_NO_IMAGE_KHR;

  if (desc->nb_layers < 1)
    return EGL_NO_IMAGE_KHR;

  // We assume the first layer contains the image data (typical for NV12/VAAPI)
  AVDRMLayerDescriptor *layer = &desc->layers[0];

  EGLint attribs[32];
  int k = 0;

  attribs[k++] = EGL_WIDTH;
  attribs[k++] = frame->width;
  attribs[k++] = EGL_HEIGHT;
  attribs[k++] = frame->height;
  attribs[k++] = EGL_LINUX_DRM_FOURCC_EXT;
  attribs[k++] = layer->format;

  // Map each plane (Plane 0 is Y, Plane 1 is UV for NV12)
  for (int i = 0; i < layer->nb_planes; i++) {
    int obj_idx = layer->planes[i].object_index;
    int fd = desc->objects[obj_idx].fd;

    if (i == 0) {
      attribs[k++] = EGL_DMA_BUF_PLANE0_FD_EXT;
      attribs[k++] = fd;
      attribs[k++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
      attribs[k++] = layer->planes[i].offset;
      attribs[k++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
      attribs[k++] = layer->planes[i].pitch;
    } else if (i == 1) {
      attribs[k++] = EGL_DMA_BUF_PLANE1_FD_EXT;
      attribs[k++] = fd;
      attribs[k++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
      attribs[k++] = layer->planes[i].offset;
      attribs[k++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
      attribs[k++] = layer->planes[i].pitch;
    }
  }
  attribs[k++] = EGL_NONE;

  // Create the image from the DMA-BUF file descriptors
  return eglCreateImageKHR(egl_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                           NULL, attribs);
}

void update_video_frame_egl(GLuint texID);

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

  GLuint texVideo;
  glGenTextures(1, &texVideo);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, texVideo);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (open_video("videos/zoo.mp4") < 0) {
    fprintf(stderr, "Error: Exiting because video couldnt be opened.\n");
    return -1;
  }

  // glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  // GLuint texY, texU, texV;
  // glGenTextures(1, &texY);
  // glGenTextures(1, &texU);
  // glGenTextures(1, &texV);

  // // Helper to init YUV textures
  // GLuint texs[3] = {texY, texU, texV};
  // int dims[3][2] = {{dec_ctx->width, dec_ctx->height},
  //                   {dec_ctx->width / 2, dec_ctx->height / 2},
  //                   {dec_ctx->width / 2, dec_ctx->height / 2}};

  // for (int i = 0; i < 3; i++) {
  //   glBindTexture(GL_TEXTURE_2D, texs[i]);
  //   glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, dims[i][0], dims[i][1], 0,
  //                GL_LUMINANCE, GL_UNSIGNED_BYTE, NULL);
  //   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  //   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  //   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  //   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // }

  // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  // glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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

  // // Get uniform locations
  // GLint locY = glGetUniformLocation(program, "uTexY");
  // GLint locU = glGetUniformLocation(program, "uTexU");
  // GLint locV = glGetUniformLocation(program, "uTexV");

  // glEnableVertexAttribArray(aPos);
  // glEnableVertexAttribArray(aTex);

  // glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
  // quad); glVertexAttribPointer(aTex, 2, GL_FLOAT, GL_FALSE, 4 *
  // sizeof(float),
  //                       quad + 2);

  while (!glfwWindowShouldClose(window)) {

    // update_yuv_video_frame(texY, texU, texV);
    update_video_frame_egl(texVideo);

    glClearColor(0.0f, 0.0f, 1.0f, 1.0f); // Bright Blue
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);

    // // 1. Bind Textures to Units 0, 1, 2
    // glActiveTexture(GL_TEXTURE0);
    // glBindTexture(GL_TEXTURE_2D, texY);
    // glActiveTexture(GL_TEXTURE1);
    // glBindTexture(GL_TEXTURE_2D, texU);
    // glActiveTexture(GL_TEXTURE2);
    // glBindTexture(GL_TEXTURE_2D, texV);

    // // 2. Tell Shader which Unit corresponds to which Uniform
    // glUniform1i(locY, 0);
    // glUniform1i(locU, 1);
    // glUniform1i(locV, 2);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texVideo);
    glUniform1i(locTex, 0);

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

void debug_drm_frame(AVFrame *f) {
  if (f->format != AV_PIX_FMT_DRM_PRIME) {
    fprintf(stderr, "[DEBUG] Not a DRM frame! Format: %d\n", f->format);
    return;
  }
  AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)f->data[0];
  fprintf(stderr, "[DEBUG] Frame: %dx%d | Objects: %d | Layers: %d\n", f->width,
          f->height, desc->nb_objects, desc->nb_layers);

  for (int i = 0; i < desc->nb_layers; i++) {
    AVDRMLayerDescriptor *layer = &desc->layers[i];
    // Convert FourCC to string
    uint32_t fmt = layer->format;
    fprintf(stderr, "  Layer %d: Format %.4s (0x%x) | Planes: %d\n", i,
            (char *)&fmt, fmt, layer->nb_planes);

    for (int j = 0; j < layer->nb_planes; j++) {
      fprintf(stderr, "    Plane %d: ObjIdx %d | Offset %ld | Pitch %ld\n", j,
              layer->planes[j].object_index, (long)layer->planes[j].offset,
              (long)layer->planes[j].pitch);
    }
  }
}

void update_video_frame_egl(GLuint texID) {
  while (1) {
    if (av_read_frame(fmt_ctx, pkt) < 0) {
      av_seek_frame(fmt_ctx, video_stream_idx, 0, AVSEEK_FLAG_BACKWARD);
      continue;
    }

    if (pkt->stream_index != video_stream_idx) {
      av_packet_unref(pkt);
      continue;
    }

    if (avcodec_send_packet(dec_ctx, pkt) < 0) {
      av_packet_unref(pkt);
      continue;
    }

    if (avcodec_receive_frame(dec_ctx, frame) == 0) {

      /* 🔴 EXPECT VAAPI HERE */
      if (frame->format != AV_PIX_FMT_VAAPI) {
        fprintf(stderr, "Unexpected frame format: %d\n", frame->format);
        av_frame_unref(frame);
        av_packet_unref(pkt);
        return;
      }

      /* ✅ MAP VAAPI → DRM_PRIME */
      AVFrame *drm_frame = av_frame_alloc();

      if (av_hwframe_map(drm_frame, frame, AV_HWFRAME_MAP_READ) < 0) {
        fprintf(stderr, "av_hwframe_map failed\n");
        av_frame_free(&drm_frame);
        av_frame_unref(frame);
        av_packet_unref(pkt);
        return;
      }

      printf("Mapped format: %s\n", av_get_pix_fmt_name(drm_frame->format));

      if (drm_frame->format != AV_PIX_FMT_DRM_PRIME) {
        fprintf(stderr, "Mapped frame is not DRM_PRIME\n");
        av_frame_free(&drm_frame);
        av_frame_unref(frame);
        av_packet_unref(pkt);
        return;
      }

      EGLDisplay disp = eglGetCurrentDisplay();
      EGLImageKHR img = create_eglimage_from_frame(disp, drm_frame);

      if (img != EGL_NO_IMAGE_KHR) {
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, texID);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);

        /* Destroy previous image AFTER next frame */
        static EGLImageKHR last_img = EGL_NO_IMAGE_KHR;
        if (last_img != EGL_NO_IMAGE_KHR)
          eglDestroyImageKHR(disp, last_img);
        last_img = img;
      } else {
        fprintf(stderr, "Failed to create EGLImage\n");
      }

      av_frame_free(&drm_frame);
      av_frame_unref(frame);
      av_packet_unref(pkt);
      return;
    }

    av_packet_unref(pkt);
  }
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
  avformat_open_input(&fmt_ctx, filename, NULL, NULL);
  avformat_find_stream_info(fmt_ctx, NULL);

  for (int i = 0; i < fmt_ctx->nb_streams; i++) {
    if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_stream_idx = i;
      break;
    }
  }

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

// int open_video(const char *filename) {

//   fmt_ctx = NULL;

//   if (avformat_open_input(&fmt_ctx, filename, NULL, NULL) < 0) {
//     fprintf(stderr, "Error: Could not open file %s\n", filename);
//     return -1;
//   }
//   if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
//     fprintf(stderr, "Error: Could not find stream information\n");
//     return -1;
//   }

//   // Find video stream
//   video_stream_idx = -1;
//   for (int i = 0; i < fmt_ctx->nb_streams; i++) {
//     if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
//       video_stream_idx = i;
//       break;
//     }
//   }

//   if (video_stream_idx == -1) {
//     fprintf(stderr, "Error: Could not find a video stream\n");
//     return -1;
//   }

//   video_time_base = fmt_ctx->streams[video_stream_idx]->time_base;

//   const AVCodec *codec = avcodec_find_decoder(
//       fmt_ctx->streams[video_stream_idx]->codecpar->codec_id);
//   dec_ctx = avcodec_alloc_context3(codec);
//   avcodec_parameters_to_context(dec_ctx,
//                                 fmt_ctx->streams[video_stream_idx]->codecpar);

//   // --- FOR HWACCEL ---
//   // Try VAAPI (standard for Linux/Fedora)
//   enum AVHWDeviceType type =
//       av_hwdevice_find_type_by_name("vaapi"); // "drm" or "v4l2m2m"
//   if (type != AV_HWDEVICE_TYPE_NONE) {
//     // Find the format the hardware uses
//     printf("HW_TYPE: %d\n", type);
//     fflush(stdout);

//     for (int i = 0;; i++) {
//       const AVCodecHWConfig *config = avcodec_get_hw_config(codec, i);
//       if (!config)
//         break;
//       if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
//           config->device_type == type) {
//         hw_pix_fmt = config->pix_fmt;
//         break;
//       }
//     }
//     dec_ctx->get_format = get_hw_format;
//     hw_decoder_init(dec_ctx, type);
//   }
//   // ----------------------------

//   avcodec_open2(dec_ctx, codec, NULL);

//   // Prepare RGB frame for OpenGL
//   frame = av_frame_alloc();
//   frame_yuv_clean = av_frame_alloc();
//   int size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, dec_ctx->width,
//                                       dec_ctx->height, 1);
//   uint8_t *internal_buffer = av_malloc(size);
//   av_image_fill_arrays(frame_yuv_clean->data, frame_yuv_clean->linesize,
//                        internal_buffer, AV_PIX_FMT_YUV420P, dec_ctx->width,
//                        dec_ctx->height, 1);

//   sws_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
//                            dec_ctx->width, dec_ctx->height,
//                            AV_PIX_FMT_YUV420P, SWS_POINT, NULL, NULL, NULL);
//   pkt = av_packet_alloc();
//   return 0;
// }

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
      // Send packet to decoder
      if (avcodec_send_packet(dec_ctx, pkt) == 0) {

        // Try to retrieve a frame

        // if (avcodec_receive_frame(dec_ctx, frame) == 0) {
        //   // SUCCESS: We got a video frame! Upload and stop looking.
        //   upload_yuv_textures(frame, texY, texU, texV);
        //   av_packet_unref(pkt);
        //   return; // Exit function, we are done for this frame
        // }

        // --- FOR HWACCEL ---
        if (avcodec_receive_frame(dec_ctx, frame) == 0) {

          // --- Handle HW frames ---
          AVFrame *use_frame = frame;

          // Check if the frame is a hardware surface
          if (frame->format == hw_pix_fmt) {
            AVFrame *sw_frame = av_frame_alloc();
            sw_frame->format = AV_PIX_FMT_YUV420P;

            // Transfer GPU memory to System RAM
            if (av_hwframe_transfer_data(sw_frame, frame, 0) >= 0) {
              // Copy metadata needed for upload_yuv_textures
              sw_frame->width = frame->width;
              sw_frame->height = frame->height;
              use_frame = sw_frame;

              // CRITICAL: Upload the CPU-side data
              // upload_yuv_textures(sw_frame, texY, texU, texV);
            } else {
              av_frame_free(&sw_frame);
              av_packet_unref(pkt);
              return;
            }
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

          // -------- Upload --------
          upload_yuv_textures(use_frame, texY, texU, texV);

          if (use_frame != frame)
            av_frame_free(&use_frame);

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
