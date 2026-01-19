#include "main.h"
#include "gl.h"
#include "utils.h"
#include <GLES2/gl2.h>
#include <time.h>

// Position (x,y)   Texcoord (u,v)
GLfloat quad[] = {
    -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
    -1.0f, 1.0f,  0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 0.0f,
};

// EGL ZERO-COPY
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

int is_paused = 0;               // 0 = Playing, 1 = Paused
double total_pause_offset = 0.0; // Total time spent paused
double last_pause_start = 0.0;   // Timestamp when the current pause started

void key_callback(GLFWwindow *window, int key, int scancode, int action,
                  int mods) {
  if (key == GLFW_KEY_SPACE && action == GLFW_PRESS) {
    is_paused = !is_paused; // Toggle state

    if (is_paused) {
      // Just paused: record the time
      last_pause_start = glfwGetTime();
      printf("Paused\n");
    } else {
      // Just resumed: calculate how long we were paused and add to offset
      double paused_duration = glfwGetTime() - last_pause_start;
      total_pause_offset += paused_duration;
      printf("Resumed (Offset: %.2f sec)\n", total_pause_offset);
    }
  }
}

int main(int argc, char **argv) {

  GLFWwindow *window = initGLFW(VIDEO_W, VIDEO_H);
  if (window == NULL)
    return -1;

  glfwSetKeyCallback(window, key_callback);

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

  char *vs_code = read_file("shader.vs"); // MUST UPDATE THIS FILE
  char *fs_code = read_file("shader.fs");
  GLuint program = create_program(vs_code, fs_code);
  free(vs_code);
  free(fs_code);

  GLint aPos = glGetAttribLocation(program, "aPos");
  GLint aTex = glGetAttribLocation(program, "aTex");

  // 4. Initialize 4 Video Players
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

  while (!glfwWindowShouldClose(window)) {

    // Update Decode Logic (CPU/Decoder)
    for (int i = 0; i < 4; i++) {
      update_player(&players[i]);
    }

    for (int i = 0; i < 4; i++) {
      render_player(&players[i], program);
    }

    glfwSwapBuffers(window);
    glfwPollEvents();
  }

  glfwTerminate();
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

// Helper to create a basic 4x4 matrix for grid positioning
void calculate_transform(int id, float *m) {
  // Identity
  for (int i = 0; i < 16; i++)
    m[i] = 0.0f;
  m[0] = 1.0f;
  m[5] = 1.0f;
  m[10] = 1.0f;
  m[15] = 1.0f;

  // Scale by 0.5 (since we are squeezing 4 into 1 screen)
  m[0] = 0.5f;
  m[5] = 0.5f;

  // Translate based on ID
  // 0: Top-Left (-0.5, 0.5)
  // 1: Top-Right (0.5, 0.5)
  // 2: Bot-Left (-0.5, -0.5)
  // 3: Bot-Right (0.5, -0.5)
  float tx = (id % 2 == 0) ? -0.5f : 0.5f;
  float ty = (id < 2) ? 0.5f : -0.5f;

  // In Column-Major 4x4, translation is indices 12, 13
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

  // HW Device Init (VAAPI)
  // Note: Creating a separate HW context per player is safest for simple code
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
  // ---------------------------------------------------------
  // 1. DECODE / FILL BUFFER
  // Only try to decode a new frame if the current buffer (vp->frame)
  // is empty. We use width==0 to check if the frame is unreferenced.
  // ---------------------------------------------------------
  if (vp->frame->width == 0) {
    int got_frame = 0;

    // Loop until we get one frame or error/EOF
    while (!got_frame) {
      int ret = av_read_frame(vp->fmt_ctx, vp->pkt);

      if (ret == AVERROR_EOF) {
        // Handle Loop: Seek to start and reset timing
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

      // If we processed a packet but didn't get a frame (e.g. B-frames needed
      // more data), we loop again. But if we got a frame, we stop.
    }
  }

  // If after trying to decode, we still have no frame, return.
  if (vp->frame->width == 0)
    return;

  // ---------------------------------------------------------
  // 2. PACING / SYNC CHECK (UPDATED)
  // ---------------------------------------------------------

  if (vp->first_pts == AV_NOPTS_VALUE) {
    vp->first_pts = vp->frame->pts;
    // IMPORTANT: We must subtract existing pause offset here too
    // so new videos start in sync with currently playing ones if needed.
    vp->start_time = glfwGetTime() - total_pause_offset;
  }

  double current_time = glfwGetTime();

  // ADJUST CLOCK: Subtract the total time we spent paused
  // If we slept for 10s, we subtract 10s so the video thinks no time passed.
  double master_clock = (current_time - vp->start_time) - total_pause_offset;

  double pts_sec =
      pts_to_seconds(vp->frame->pts - vp->first_pts, vp->video_time_base);

  if (pts_sec > master_clock) {
    return;
  }

  // ---------------------------------------------------------
  // 3. DISPLAY / HW MAPPING
  // If we reach here, it is time (or past time) to display.
  // ---------------------------------------------------------

  AVFrame *drm_frame = av_frame_alloc();
  drm_frame->format = AV_PIX_FMT_DRM_PRIME;

  if (av_hwframe_map(drm_frame, vp->frame, AV_HWFRAME_MAP_READ) == 0) {

    // --- [Standard EGL Setup from your code] ---
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

    // Update OpenGL Textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, vp->texY);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, vp->image_y);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, vp->texUV);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, vp->image_uv);
  } else {
    // Map failed, cleanup the temp frame
    av_frame_free(&drm_frame);
  }

  // ---------------------------------------------------------
  // 4. CLEANUP BUFFER
  // We have consumed this frame (sent to GPU).
  // Clear it so Step 1 can decode the *next* frame on the next call.
  // ---------------------------------------------------------
  av_frame_unref(vp->frame);
}

void render_player(VideoPlayer *vp, GLuint program) {
  if (vp->image_y == EGL_NO_IMAGE_KHR)
    return; // Nothing to render yet

  // Send the Transform Matrix
  GLint locTransform = glGetUniformLocation(program, "uTransform");
  glUniformMatrix4fv(locTransform, 1, GL_FALSE, vp->transform);

  // Bind Textures
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, vp->texY);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, vp->texUV);

  // Draw
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
