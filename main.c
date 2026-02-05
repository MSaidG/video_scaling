#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// --- RESOURCES ---
typedef struct {
  uint32_t handle;
  uint32_t stride;
  uint32_t size;
  uint32_t fb_id;
  uint8_t *map;
} DumbBuffer;

struct {
  int fd;
  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;

  // ZynqMP Specific Plane IDs
  uint32_t plane_primary_id;
  uint32_t plane_overlay_id;

  // Double Buffering
  DumbBuffer bufs[2];
  int current_buf_idx;

  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;
} kms;

volatile sig_atomic_t running = 1;

double get_time_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void handle_sigint(int sig) { running = 0; }

// --- CLEANUP FUNCTION ---
void cleanup() {
  printf("\nCleaning up resources...\n");

  // 1. Disable Planes (Clear screen)
  if (kms.fd >= 0 && kms.crtc) {
    // Disable Primary Plane (Our App)
    drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0);

    // Optional: We could try to re-enable the console plane here if we knew its
    // original FB ID, but typically closing the DRM Master FD allows the kernel
    // to restore the console eventually.
  }

  // 2. EGL Cleanup
  if (kms.egl_disp != EGL_NO_DISPLAY) {
    eglMakeCurrent(kms.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (kms.egl_surf != EGL_NO_SURFACE)
      eglDestroySurface(kms.egl_disp, kms.egl_surf);
    if (kms.egl_ctx != EGL_NO_CONTEXT)
      eglDestroyContext(kms.egl_disp, kms.egl_ctx);
    eglTerminate(kms.egl_disp);
  }

  // 3. Dumb Buffer Cleanup
  for (int i = 0; i < 2; i++) {
    // Unmap Memory
    if (kms.bufs[i].map) {
      munmap(kms.bufs[i].map, kms.bufs[i].size);
    }
    // Remove Framebuffer Object
    if (kms.bufs[i].fb_id) {
      drmModeRmFB(kms.fd, kms.bufs[i].fb_id);
    }
    // Destroy Dumb Buffer Handle (Frees GPU memory)
    if (kms.bufs[i].handle) {
      struct drm_mode_destroy_dumb destroy_req = {.handle = kms.bufs[i].handle};
      ioctl(kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    }
  }

  // 4. DRM Resources
  if (kms.crtc)
    drmModeFreeCrtc(kms.crtc);
  if (kms.connector)
    drmModeFreeConnector(kms.connector);
  if (kms.fd >= 0)
    close(kms.fd);

  printf("Cleanup Done.\n");
}

int init_drm() {
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    return -1;

  drmModeRes *res = drmModeGetResources(kms.fd);
  if (!res)
    return -1;

  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *conn = drmModeGetConnector(kms.fd, res->connectors[i]);
    if (conn->connection == DRM_MODE_CONNECTED) {
      kms.connector = conn;
      break;
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);

  if (!kms.connector) {
    fprintf(stderr, "No monitor connected\n");
    return -1;
  }

  kms.mode = kms.connector->modes[0];
  printf("Mode: %dx%d @ %dHz\n", kms.mode.hdisplay, kms.mode.vdisplay,
         kms.mode.vrefresh);

  drmModeEncoder *enc = drmModeGetEncoder(kms.fd, kms.connector->encoder_id);
  if (enc) {
    kms.crtc = drmModeGetCrtc(kms.fd, enc->crtc_id);
    drmModeFreeEncoder(enc);
  } else {
    res = drmModeGetResources(kms.fd);
    kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[0]);
    drmModeFreeResources(res);
  }

  kms.plane_primary_id = 39;
  kms.plane_overlay_id = 41;

  return 0;
}

int create_dumb_buffer(DumbBuffer *buf) {
  struct drm_mode_create_dumb create_req = {0};
  create_req.width = kms.mode.hdisplay;
  create_req.height = kms.mode.vdisplay;
  create_req.bpp = 32;

  if (ioctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) < 0)
    return -1;
  buf->handle = create_req.handle;
  buf->stride = create_req.pitch;
  buf->size = create_req.size;

  struct drm_mode_map_dumb map_req = {0};
  map_req.handle = buf->handle;
  if (ioctl(kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0)
    return -1;

  buf->map = mmap(0, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, kms.fd,
                  map_req.offset);
  if (buf->map == MAP_FAILED)
    return -1;

  memset(buf->map, 0, buf->size);

  return drmModeAddFB(kms.fd, kms.mode.hdisplay, kms.mode.vdisplay, 24, 32,
                      buf->stride, buf->handle, &buf->fb_id);
}

int init_egl() {
  kms.egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(kms.egl_disp, NULL, NULL)) {
    kms.egl_disp = eglGetDisplay((EGLNativeDisplayType)kms.fd);
    eglInitialize(kms.egl_disp, NULL, NULL);
  }
  eglBindAPI(EGL_OPENGL_ES_API);

  EGLConfig config;
  EGLint num_configs;
  EGLint attribs[] = {EGL_SURFACE_TYPE,
                      EGL_PBUFFER_BIT,
                      EGL_RED_SIZE,
                      8,
                      EGL_GREEN_SIZE,
                      8,
                      EGL_BLUE_SIZE,
                      8,
                      EGL_ALPHA_SIZE,
                      8,
                      EGL_RENDERABLE_TYPE,
                      EGL_OPENGL_ES2_BIT,
                      EGL_NONE};
  eglChooseConfig(kms.egl_disp, attribs, &config, 1, &num_configs);

  EGLint pbuffer_attribs[] = {EGL_WIDTH, kms.mode.hdisplay, EGL_HEIGHT,
                              kms.mode.vdisplay, EGL_NONE};
  kms.egl_surf = eglCreatePbufferSurface(kms.egl_disp, config, pbuffer_attribs);

  EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT, ctx_attribs);

  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);
  return 0;
}

int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);

  if (init_drm() < 0) {
    fprintf(stderr, "Failed to init DRM\n");
    return -1;
  }

  // Create TWO buffers for Double Buffering
  if (create_dumb_buffer(&kms.bufs[0]) < 0) {
    cleanup();
    return -1;
  }
  if (create_dumb_buffer(&kms.bufs[1]) < 0) {
    cleanup();
    return -1;
  }

  if (init_egl() < 0) {
    cleanup();
    return -1;
  }

  printf("Setup Complete. Double Buffering Active.\n");

  // 1. Disable Console Plane
  drmModeSetPlane(kms.fd, kms.plane_overlay_id, kms.crtc->crtc_id, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0);

  // 2. Set Initial Buffer (Buffer 0) to Primary Plane
  drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                  kms.bufs[0].fb_id, 0, 0, 0, kms.mode.hdisplay,
                  kms.mode.vdisplay, 0, 0, kms.mode.hdisplay << 16,
                  kms.mode.vdisplay << 16);

  int back_buf_idx = 1; // Start writing to Buffer 1

  printf("Rendering... Press Ctrl+C to exit.\n");

  while (running) {
    double t = get_time_sec();

    // --- 1. RENDER (To PBuffer) ---
    glClearColor((sin(t) + 1) / 2, (sin(t + 2) + 1) / 2, (sin(t + 4) + 1) / 2,
                 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_SCISSOR_TEST);
    glScissor((int)(t * 100) % 1720, 100, 200, 200);
    glClearColor(1.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    glFinish();

    // --- 2. COPY (To Back Buffer) ---
    glReadPixels(0, 0, kms.mode.hdisplay, kms.mode.vdisplay, GL_RGBA,
                 GL_UNSIGNED_BYTE, kms.bufs[back_buf_idx].map);

    // --- 3. FLIP (Swap Buffers) ---
    int ret = drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                              kms.bufs[back_buf_idx].fb_id, 0, 0, 0,
                              kms.mode.hdisplay, kms.mode.vdisplay, 0, 0,
                              kms.mode.hdisplay << 16, kms.mode.vdisplay << 16);

    if (ret != 0) {
      perror("Flip failed");
    }

    // Swap indices
    back_buf_idx = (back_buf_idx + 1) % 2;
  }

  cleanup(); // Clean up on exit
  return 0;
}