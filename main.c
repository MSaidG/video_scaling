#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct {
  int fd;
  uint32_t current_fb_id;

  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;

  struct gbm_device *gbm_dev;
  struct gbm_surface *gbm_surf;
  struct gbm_bo *current_bo;

  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;

} kms;

volatile sig_atomic_t running = 1;
int waiting_for_flip = 0;

double get_time_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void handle_sigint(int sig) { running = 0; }

void print_egl_diagnostics(EGLDisplay dpy) {
  printf("--- EGL DIAGNOSTICS ---\n");
  printf("Vendor:   %s\n", eglQueryString(dpy, EGL_VENDOR) ?: "UNKNOWN");
  printf("Version:  %s\n", eglQueryString(dpy, EGL_VERSION) ?: "UNKNOWN");
  printf("APIs:     %s\n", eglQueryString(dpy, EGL_CLIENT_APIS) ?: "UNKNOWN");

  const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
  if (exts) {
    printf("DMA-BUF Import:              %s\n",
           strstr(exts, "EGL_EXT_image_dma_buf_import") ? "YES" : "NO");
    printf("DMA-BUF Modifiers:           %s\n",
           strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers") ? "YES"
                                                                  : "NO");
    printf("KHR-GBM Platform :           %s\n",
           strstr(exts, "EGL_KHR_platform_gbm") ? "YES" : "NO");
    printf("KHR-GBM Platform :           %s\n",
           strstr(exts, "EGL_KHR_surfaceless_context") ? "YES" : "NO");
  } else {
    printf("Extensions: NONE (Critical Error)\n");
  }
  printf("-----------------------\n");
}

// --- INIT STEP 1: OPEN DRM DEVICE ---
int init_drm_device() {
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0) {
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if (kms.fd < 0) {
      fprintf(stderr, "Error: Could not open /dev/dri/card0 or card1\n");
      return -1;
    }
  }

  if (drmIsKMS(kms.fd)) {
    printf("DRM node supports kernel mode setting.\n");
  } else {
    printf("DRM node does not support kernel mode setting.\n");
    return -1;
  }
  return 0;
}

// --- INIT STEP 2: SETUP CONNECTOR & CRTC ---
int setup_kms_resources() {
  drmModeRes *resources = drmModeGetResources(kms.fd);
  if (!resources)
    return -1;

  // Find a connected connector
  kms.connector = NULL;
  for (int i = 0; i < resources->count_connectors; i++) {
    drmModeConnector *conn =
        drmModeGetConnector(kms.fd, resources->connectors[i]);
    if (conn->connection == DRM_MODE_CONNECTED) {
      kms.connector = conn;
      break;
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(resources);

  if (!kms.connector) {
    fprintf(stderr, "Error: No connected monitor found.\n");
    return -1;
  }

  printf("Connector type is %s\n",
         drmModeGetConnectorTypeName(kms.connector->connector_type));

  // Pick first mode
  kms.mode = kms.connector->modes[0];
  printf("Selected Mode: %dx%d @ %dHz\n", kms.mode.hdisplay, kms.mode.vdisplay,
         kms.mode.vrefresh);

  // Find encoder/CRTC
  drmModeEncoder *enc = NULL;
  if (kms.connector->encoder_id) {
    enc = drmModeGetEncoder(kms.fd, kms.connector->encoder_id);
  }

  if (enc && enc->crtc_id) {
    kms.crtc = drmModeGetCrtc(kms.fd, enc->crtc_id);
  } else {
    // Re-fetch resources to find a CRTC if the encoder method failed
    resources = drmModeGetResources(kms.fd);
    if (resources && resources->count_crtcs > 0) {
      kms.crtc = drmModeGetCrtc(kms.fd, resources->crtcs[0]);
    }
    if (resources)
      drmModeFreeResources(resources);
  }
  if (enc)
    drmModeFreeEncoder(enc);

  if (!kms.crtc) {
    fprintf(stderr, "Error: Could not find a valid CRTC.\n");
    return -1;
  }
  return 0;
}

// --- INIT STEP 3: SETUP GBM ---
int setup_gbm() {
  kms.gbm_dev = gbm_create_device(kms.fd);
  if (!kms.gbm_dev)
    return -1;

  printf("GBM backend: %s\n", gbm_device_get_backend_name(kms.gbm_dev));

  uint32_t gbm_format = GBM_FORMAT_XRGB8888;
  if (!gbm_device_is_format_supported(
          kms.gbm_dev, gbm_format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)) {
    fprintf(stderr, "Format (0x%x) is not supported!\n", gbm_format);
  } else {
    printf("Format (Ox%x) is supported!\n", gbm_format);
  }

  kms.gbm_surf =
      gbm_surface_create(kms.gbm_dev, kms.mode.hdisplay, kms.mode.vdisplay,
                         gbm_format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!kms.gbm_surf)
    return -1;

  return 0;
}

// --- INIT STEP 4: SETUP EGL ---
int setup_egl() {
  kms.egl_disp = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, kms.gbm_dev, NULL);
  if (!eglInitialize(kms.egl_disp, NULL, NULL)) {
    fprintf(stderr, "Error: EGL Initialize failed.\n");
    return -1;
  }

  print_egl_diagnostics(kms.egl_disp);

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    fprintf(stderr, "Couldn't bind EGL to GLES API!\n");
    return -1;
  }
  printf("Current egl api: 0x%x\n", eglQueryAPI());

  // Find Matching Config
  EGLConfig config;
  EGLint num_configs;
  EGLConfig *configs;
  int found_config = 0;
  uint32_t gbm_format = GBM_FORMAT_XRGB8888; // Must match GBM setup

  if (!eglGetConfigs(kms.egl_disp, NULL, 0, &num_configs))
    return -1;
  configs = malloc(num_configs * sizeof(EGLConfig));
  eglGetConfigs(kms.egl_disp, configs, num_configs, &num_configs);

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

  if (!found_config) {
    fprintf(stderr, "Error: No matching EGL config for GBM format 0x%x\n",
            gbm_format);
    return -1;
  }

  EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT, ctx_attribs);
  kms.egl_surf = eglCreateWindowSurface(
      kms.egl_disp, config, (EGLNativeWindowType)kms.gbm_surf, NULL);

  if (kms.egl_surf == EGL_NO_SURFACE) {
    fprintf(stderr, "Error: Failed to create EGL Surface.\n");
    return -1;
  }

  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);
  return 0;
}

int init_kms() {
  if (init_drm_device() < 0)
    return -1;
  if (setup_kms_resources() < 0)
    return -1;
  if (setup_gbm() < 0)
    return -1;
  if (setup_egl() < 0)
    return -1;

  printf("KMS/GBM/EGL Initialized successfully.\n");
  return 0;
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void *data) {
  int *waiting = (int *)data;
  *waiting = 0;
}

uint32_t get_fb_for_bo(struct gbm_bo *bo) {
  uint32_t fb_id;
  uint32_t width = gbm_bo_get_width(bo);
  uint32_t height = gbm_bo_get_height(bo);
  uint32_t format = gbm_bo_get_format(bo);

  uint32_t handles[4] = {0};
  uint32_t pitches[4] = {0};
  uint32_t offsets[4] = {0};
  uint64_t modifiers[4] = {0};

  int planes = gbm_bo_get_plane_count(bo);
  uint64_t modifier = gbm_bo_get_modifier(bo);

  for (int i = 0; i < planes; i++) {
    handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
    pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
    offsets[i] = gbm_bo_get_offset(bo, i);
    if (modifier != DRM_FORMAT_MOD_INVALID) {
      modifiers[i] = modifier;
    }
  }

  int ret = -1;
  if (modifier != DRM_FORMAT_MOD_INVALID) {
    ret = drmModeAddFB2WithModifiers(kms.fd, width, height, format, handles,
                                     pitches, offsets, modifiers, &fb_id,
                                     DRM_MODE_FB_MODIFIERS);
  }
  if (ret) {
    ret = drmModeAddFB2(kms.fd, width, height, format, handles, pitches,
                        offsets, &fb_id, 0);
  }
  if (ret) {
    ret = drmModeAddFB(kms.fd, width, height, 24, 32, pitches[0], handles[0],
                       &fb_id);
  }

  if (ret) {
    perror("Error: Failed to create DRM Framebuffer");
    return 0;
  }
  return fb_id;
}

void swap_buffers_kms() {
  eglSwapBuffers(kms.egl_disp, kms.egl_surf);

  struct gbm_bo *next_bo = gbm_surface_lock_front_buffer(kms.gbm_surf);
  if (!next_bo) {
    fprintf(stderr, "Error: Failed to lock front buffer.\n");
    return;
  }
  uint32_t next_fb_id = get_fb_for_bo(next_bo);

  int ret = drmModePageFlip(kms.fd, kms.crtc->crtc_id, next_fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
  if (ret) {
    fprintf(stderr, "Page Flip failed: %d\n", ret);
    gbm_surface_release_buffer(kms.gbm_surf, next_bo);
    drmModeRmFB(kms.fd, next_fb_id);
    return;
  }

  waiting_for_flip = 1;

  if (kms.current_bo) {
    gbm_surface_release_buffer(kms.gbm_surf, kms.current_bo);
    drmModeCloseFB(kms.fd, kms.current_fb_id);
  }

  kms.current_bo = next_bo;
  kms.current_fb_id = next_fb_id;
}

void cleanup() {
  printf("Cleaning up resources...\n");
  if (kms.current_bo) {
    gbm_surface_release_buffer(kms.gbm_surf, kms.current_bo);
    drmModeRmFB(kms.fd, kms.current_fb_id);
  }

  if (kms.egl_disp != EGL_NO_DISPLAY) {
    eglMakeCurrent(kms.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (kms.egl_surf != EGL_NO_SURFACE)
      eglDestroySurface(kms.egl_disp, kms.egl_surf);
    if (kms.egl_ctx != EGL_NO_CONTEXT)
      eglDestroyContext(kms.egl_disp, kms.egl_ctx);
    eglTerminate(kms.egl_disp);
  }

  if (kms.gbm_surf)
    gbm_surface_destroy(kms.gbm_surf);
  if (kms.gbm_dev)
    gbm_device_destroy(kms.gbm_dev);

  if (kms.crtc)
    drmModeFreeCrtc(kms.crtc);
  if (kms.connector)
    drmModeFreeConnector(kms.connector);
  if (kms.fd >= 0)
    close(kms.fd);

  printf("Cleanup Done.\n");
}

int main(int argc, char **argv) {
  printf("> <\n");
  signal(SIGINT, handle_sigint);

  if (init_kms() != 0) {
    return -1;
  }

  drmEventContext evctx = {0};
  evctx.version = 2;
  evctx.page_flip_handler = page_flip_handler;
  fd_set fds;

  printf("Starting render loop... Press Ctrl+C to exit.\n");

  while (running) {
    while (waiting_for_flip) {
      FD_ZERO(&fds);
      FD_SET(kms.fd, &fds);
      int ret = select(kms.fd + 1, &fds, NULL, NULL, NULL);
      if (ret > 0) {
        drmHandleEvent(kms.fd, &evctx);
      } else if (ret < 0 && errno != EINTR) {
        break;
      }
    }

    double t = get_time_sec();
    float r = (sin(t) + 1.0f) / 2.0f;
    float g = (sin(t + 2.0f) + 1.0f) / 2.0f;
    float b = (sin(t + 4.0f) + 1.0f) / 2.0f;

    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    swap_buffers_kms();
  }

  printf("\nExiting...\n");
  cleanup();
  return 0;
}
