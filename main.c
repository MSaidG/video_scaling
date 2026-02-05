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
struct {
  int fd;
  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;

  // ZynqMP Specific Plane IDs
  uint32_t plane_primary_id; // Usually 39
  uint32_t plane_overlay_id; // Usually 41 (TTY)

  uint32_t dumb_handle;
  uint32_t dumb_stride;
  uint32_t dumb_size;
  uint32_t dumb_fb_id;
  uint8_t *dumb_map;

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

// --- FIND PLANES ---
void find_planes() {
  drmModePlaneRes *plane_res = drmModeGetPlaneResources(kms.fd);
  if (!plane_res) {
    fprintf(stderr, "Failed to get plane resources\n");
    return;
  }

  // Default fallbacks for ZynqMP
  kms.plane_primary_id = 39;
  kms.plane_overlay_id = 41;

  // Optional: Iterate to find them dynamically if needed
  // For now, hardcoding based on your log is safer for ZynqMP.
  printf("Targeting Primary Plane: %u, Disabling Overlay Plane: %u\n",
         kms.plane_primary_id, kms.plane_overlay_id);

  drmModeFreePlaneResources(plane_res);
}

// --- DRM SETUP ---
int init_drm() {
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0) {
    perror("Failed to open card0 or card1");
    return -1;
  }

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

  find_planes(); // Find the plane IDs
  return 0;
}

// --- DUMB BUFFER SETUP ---
int init_dumb_buffer() {
  struct drm_mode_create_dumb create_req = {0};
  create_req.width = kms.mode.hdisplay;
  create_req.height = kms.mode.vdisplay;
  create_req.bpp = 32;

  ioctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);
  kms.dumb_handle = create_req.handle;
  kms.dumb_stride = create_req.pitch;
  kms.dumb_size = create_req.size;

  struct drm_mode_map_dumb map_req = {0};
  map_req.handle = kms.dumb_handle;
  ioctl(kms.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req);

  kms.dumb_map = mmap(0, kms.dumb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                      kms.fd, map_req.offset);
  memset(kms.dumb_map, 0x00, kms.dumb_size); // Clear to Black

  // Use XRGB8888 (24 depth, 32 bpp) for the Display
  drmModeAddFB(kms.fd, kms.mode.hdisplay, kms.mode.vdisplay, 24, 32,
               kms.dumb_stride, kms.dumb_handle, &kms.dumb_fb_id);

  return 0;
}

// --- EGL SETUP ---
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

  if (init_drm() < 0)
    return -1;
  if (init_dumb_buffer() < 0)
    return -1;

  // 1. Fill with WHITE to prove screen works (You confirmed this works)
  printf("Filling buffer with WHITE...\n");
  memset(kms.dumb_map, 0xFF, kms.dumb_size);

  if (init_egl() < 0)
    return -1;

  // Force Planes (Keep this, it's working)
  drmModeSetPlane(kms.fd, kms.plane_overlay_id, kms.crtc->crtc_id, 0, 0, 0, 0,
                  0, 0, 0, 0, 0, 0);
  drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                  kms.dumb_fb_id, 0, 0, 0, kms.mode.hdisplay, kms.mode.vdisplay,
                  0, 0, kms.mode.hdisplay << 16, kms.mode.vdisplay << 16);

  printf("Starting render loop...\n");

  while (running) {
    double t = get_time_sec();

    // --- RENDER ---
    // Cycle background color
    glClearColor((sin(t) + 1) / 2, (sin(t + 2) + 1) / 2, (sin(t + 4) + 1) / 2,
                 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    // Draw Red Box
    glEnable(GL_SCISSOR_TEST);
    glScissor(100, 100, 400, 400);
    glClearColor(1.0, 0.0, 0.0, 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    glFinish();

    // --- READ BACK (The Fix) ---
    // Try Standard GL_RGBA first.
    // Note: ZynqMP is Little Endian, so GL_RGBA in memory = ABGR to the
    // hardware. If colors look swapped, we can swap bytes later.
    glReadPixels(0, 0, kms.mode.hdisplay, kms.mode.vdisplay, GL_RGBA,
                 GL_UNSIGNED_BYTE, kms.dumb_map);

    // --- DEBUGGING ---
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
      printf("glReadPixels Error: 0x%x\n", err);
    }

    // Print the first pixel. If it stays FF FF FF FF, the copy is failing.
    // If it changes, the copy works but the display isn't updating (cache
    // issue).
    static int frame = 0;
    if (frame++ % 60 == 0) {
      printf("Pixel[0]: %02X %02X %02X %02X\n", kms.dumb_map[0],
             kms.dumb_map[1], kms.dumb_map[2], kms.dumb_map[3]);
    }

    // Tell DRM to show the new frame
    drmModeDirtyFB(kms.fd, kms.dumb_fb_id, NULL, 0);

    usleep(16000);
  }

  return 0;
}