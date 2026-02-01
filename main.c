#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// --- CONFIG ---
#define RAW_FILE "videos/test_nv12.yuv"
#define VID_W 1920
#define VID_H 1080
#define FPS 60

// --- GLOBALS ---
struct {
  int fd;
  drmModeConnector *conn;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;
  struct gbm_device *gbm_dev;
  struct gbm_surface *gbm_surf;
  struct gbm_bo *curr_bo;
  uint32_t curr_fb;
  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;
} kms;

struct {
  int fd;
  unsigned char *data; // Memory mapped file
  size_t size;
  size_t frame_size;
  int total_frames;
  int curr_frame_idx;
  GLuint tex_y;
  GLuint tex_uv;
} cam;

volatile sig_atomic_t running = 1;
int waiting_for_flip = 0;

// --- UTILS ---
void handle_signal(int s) { running = 0; }

// --- SHADERS (NV12 -> RGB Conversion) ---
// NV12 has Y plane (full res) and UV plane (half res interleaved)
const char *vs_src =
    "attribute vec4 pos; attribute vec2 tex; varying vec2 v_tex; "
    "void main() { gl_Position = pos; v_tex = tex; }";

const char *fs_src =
    "precision mediump float;"
    "varying vec2 v_tex;"
    "uniform sampler2D tex_y;"
    "uniform sampler2D tex_uv;"
    "void main() {"
    "  float y = texture2D(tex_y, v_tex).r;"
    "  // UV texture is half size, but GL handles coordinates automatically \n"
    "  // We read RG from the texture because UV are interleaved bytes \n"
    "  // NV12: U is in 'r' (or 'a' depending on GL format), V is in 'g' (or "
    "'l') \n"
    "  // Standard luminance conversion: \n"
    "  vec4 uv_raw = texture2D(tex_uv, v_tex);"
    "  float u = uv_raw.r - 0.5;"
    "  float v = uv_raw.a - 0.5;" // Usually stored as Luminance Alpha (L=U,
                                  // A=V) or RG

    "  float r = y + 1.402 * v;"
    "  float g = y - 0.344 * u - 0.714 * v;"
    "  float b = y + 1.772 * u;"
    "  gl_FragColor = vec4(r, g, b, 1.0);"
    "}";

int init_kms() {
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    return -1;

  drmModeRes *res = drmModeGetResources(kms.fd);
  if (!res)
    return -1;

  // Find a connected connector
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *c = drmModeGetConnector(kms.fd, res->connectors[i]);
    if (c->connection == DRM_MODE_CONNECTED) {
      kms.conn = c;
      break;
    }
    drmModeFreeConnector(c);
  }

  // We are done with resources, free it NOW to prevent leaks
  drmModeFreeResources(res);

  if (!kms.conn) {
    fprintf(stderr, "No monitor found\n");
    return -1;
  }
  kms.mode = kms.conn->modes[0];

  // Find Encoder & CRTC
  drmModeEncoder *enc = NULL;
  if (kms.conn->encoder_id) {
    enc = drmModeGetEncoder(kms.fd, kms.conn->encoder_id);
  }

  if (enc && enc->crtc_id) {
    kms.crtc = drmModeGetCrtc(kms.fd, enc->crtc_id);
  } else {
    // Re-fetch resources just for CRTC fallback (rare case)
    res = drmModeGetResources(kms.fd);
    if (res && res->count_crtcs > 0) {
      kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[0]);
    }
    if (res)
      drmModeFreeResources(res);
  }

  // CLEANUP: Free the encoder if we retrieved it
  if (enc)
    drmModeFreeEncoder(enc);

  if (!kms.crtc)
    return -1;

  // --- GBM / EGL Setup (Same as before) ---
  kms.gbm_dev = gbm_create_device(kms.fd);
  uint32_t gbm_format = GBM_FORMAT_XRGB8888;
  kms.gbm_surf =
      gbm_surface_create(kms.gbm_dev, kms.mode.hdisplay, kms.mode.vdisplay,
                         gbm_format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

  kms.egl_disp =
      eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, kms.gbm_dev, NULL);
  eglInitialize(kms.egl_disp, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);

  // Manual Config Selection (Keep this from previous step!)
  EGLConfig config;
  EGLint num_configs;
  eglGetConfigs(kms.egl_disp, NULL, 0, &num_configs);
  EGLConfig *configs = malloc(num_configs * sizeof(EGLConfig));
  eglGetConfigs(kms.egl_disp, configs, num_configs, &num_configs);

  int found_config = 0;
  for (int i = 0; i < num_configs; i++) {
    EGLint id;
    eglGetConfigAttrib(kms.egl_disp, configs[i], EGL_NATIVE_VISUAL_ID, &id);
    if (id == gbm_format) {
      config = configs[i];
      found_config = 1;
      break;
    }
  }
  free(configs);

  if (!found_config)
    return -1;

  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT,
                       (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
  kms.egl_surf = eglCreateWindowSurface(
      kms.egl_disp, config, (EGLNativeWindowType)kms.gbm_surf, NULL);
  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);

  return 0;
}

int init_raw_input() {
  // Open the generated YUV file
  cam.fd = open(RAW_FILE, O_RDONLY);
  if (cam.fd < 0) {
    perror("Open RAW file failed");
    return -1;
  }

  struct stat sb;
  fstat(cam.fd, &sb);
  cam.size = sb.st_size;

  // NV12 Size = W * H * 1.5
  cam.frame_size = VID_W * VID_H * 3 / 2;
  cam.total_frames = cam.size / cam.frame_size;
  cam.curr_frame_idx = 0;

  // Memory map the file to simulate RAM access (like a camera buffer)
  cam.data = mmap(NULL, cam.size, PROT_READ, MAP_PRIVATE, cam.fd, 0);
  if (cam.data == MAP_FAILED)
    return -1;

  printf("Mapped RAW File: %ld bytes (%d frames)\n", cam.size,
         cam.total_frames);

  // Create Textures for NV12 (Y Plane and UV Plane)
  glGenTextures(1, &cam.tex_y);
  glBindTexture(GL_TEXTURE_2D, cam.tex_y);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  glGenTextures(1, &cam.tex_uv);
  glBindTexture(GL_TEXTURE_2D, cam.tex_uv);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  return 0;
}

void update_texture() {
  // Point to current frame in the big memory buffer
  unsigned char *frame_start = cam.data + (cam.curr_frame_idx * cam.frame_size);
  unsigned char *uv_start = frame_start + (VID_W * VID_H);

  // Upload Y Plane (Luminance) - 1 byte per pixel
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, cam.tex_y);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, VID_W, VID_H, 0, GL_LUMINANCE,
               GL_UNSIGNED_BYTE, frame_start);

  // Upload UV Plane (Chrominance) - Interleaved (UVUV...)
  // This is effectively W/2 x H/2 resolution but 2 bytes per pixel (Luminance
  // Alpha)
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, cam.tex_uv);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, VID_W / 2, VID_H / 2, 0,
               GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, uv_start);

  // Advance frame
  cam.curr_frame_idx = (cam.curr_frame_idx + 1) % cam.total_frames;
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void *data) {
  *(int *)data = 0;
}

void swap_buffers() {
  eglSwapBuffers(kms.egl_disp, kms.egl_surf);
  struct gbm_bo *bo = gbm_surface_lock_front_buffer(kms.gbm_surf);
  uint32_t handle = gbm_bo_get_handle(bo).u32;
  uint32_t fb;
  drmModeAddFB(kms.fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), 24, 32,
               gbm_bo_get_stride(bo), handle, &fb);

  drmModePageFlip(kms.fd, kms.crtc->crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT,
                  &waiting_for_flip);
  waiting_for_flip = 1;

  if (kms.curr_bo) {
    gbm_surface_release_buffer(kms.gbm_surf, kms.curr_bo);
    drmModeRmFB(kms.fd, kms.curr_fb);
  }
  kms.curr_bo = bo;
  kms.curr_fb = fb;
}

void cleanup() {
  printf("Cleaning up resources...\n");

  // 1. Clean up Camera/Input Resources
  if (cam.data && cam.data != MAP_FAILED) {
    munmap(cam.data, cam.size);
  }
  if (cam.fd >= 0) {
    close(cam.fd);
  }

  // Clean up textures (if GL context is still alive)
  if (cam.tex_y)
    glDeleteTextures(1, &cam.tex_y);
  if (cam.tex_uv)
    glDeleteTextures(1, &cam.tex_uv);

  // 2. Clean up KMS/GBM Resources (Current Frame)
  if (kms.curr_bo) {
    gbm_surface_release_buffer(kms.gbm_surf, kms.curr_bo);
    drmModeRmFB(kms.fd, kms.curr_fb);
    kms.curr_bo = NULL;
  }

  // 3. Clean up EGL
  if (kms.egl_disp != EGL_NO_DISPLAY) {
    eglMakeCurrent(kms.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (kms.egl_surf != EGL_NO_SURFACE)
      eglDestroySurface(kms.egl_disp, kms.egl_surf);
    if (kms.egl_ctx != EGL_NO_CONTEXT)
      eglDestroyContext(kms.egl_disp, kms.egl_ctx);
    eglTerminate(kms.egl_disp);
  }

  // 4. Clean up GBM Device
  if (kms.gbm_surf)
    gbm_surface_destroy(kms.gbm_surf);
  if (kms.gbm_dev)
    gbm_device_destroy(kms.gbm_dev);

  // 5. Clean up DRM/KMS
  if (kms.crtc)
    drmModeFreeCrtc(kms.crtc);
  if (kms.conn)
    drmModeFreeConnector(kms.conn);

  if (kms.fd >= 0) {
    close(kms.fd);
  }

  printf("Cleanup Done.\n");
}

int main() {
  signal(SIGINT, handle_signal);

  if (init_kms() != 0)
    return 1;
  if (init_raw_input() != 0)
    return 1;

  // Compile Shaders
  GLuint p = glCreateProgram();
  GLuint v = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(v, 1, &vs_src, 0);
  glCompileShader(v);
  GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(f, 1, &fs_src, 0);
  glCompileShader(f);
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glUseProgram(p);

  // Set Uniforms (Texture Units 0 and 1)
  glUniform1i(glGetUniformLocation(p, "tex_y"), 0);
  glUniform1i(glGetUniformLocation(p, "tex_uv"), 1);

  GLfloat verts[] = {-1, 1, 0, 0, -1, -1, 0, 1, 1, 1, 1, 0, 1, -1, 1, 1};
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, 0, 16, verts);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, 0, 16, verts + 2);

  drmEventContext ev = {0};
  ev.version = 2;
  ev.page_flip_handler = page_flip_handler;
  fd_set fds;

  printf("Simulating Camera Feed (%dx%d NV12)...\n", VID_W, VID_H);

  while (running) {
    while (waiting_for_flip) {
      if (!running)
        break;
      FD_ZERO(&fds);
      FD_SET(kms.fd, &fds);
      if (select(kms.fd + 1, &fds, 0, 0, 0) > 0)
        drmHandleEvent(kms.fd, &ev);
    }
    if (!running)
      break;

    update_texture(); // Load next frame from RAM to GPU
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    swap_buffers();

    // Simple FPS cap (usleep is not precise but fine for testing)
    usleep(16000);
  }

  glDeleteProgram(p);
  cleanup();
  return 0;
}
