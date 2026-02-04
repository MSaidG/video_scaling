#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

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

// Standard Linux clock for timing animations
double get_time_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void handle_sigint(int sig) { running = 0; }

void print_egl_diagnostics(EGLDisplay dpy) {
    printf("--- EGL DIAGNOSTICS ---\n");

    const char *vendor = eglQueryString(dpy, EGL_VENDOR);
    printf("Vendor:   %s\n", vendor ? vendor : "UNKNOWN");

    const char *version = eglQueryString(dpy, EGL_VERSION);
    printf("Version:  %s\n", version ? version : "UNKNOWN");

    const char *apis = eglQueryString(dpy, EGL_CLIENT_APIS);
    printf("APIs:     %s\n", apis ? apis : "UNKNOWN");

    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    if (exts) {
        int has_dma = (strstr(exts, "EGL_EXT_image_dma_buf_import") != NULL);
        int has_modifiers = (strstr(exts, "EGL_EXT_image_dma_buf_import_modifiers") != NULL);
        int has_khr_plaform_gbm = (strstr(exts, "EGL_KHR_platform_gbm") != NULL);
        int has_khr_surfaceless_context = (strstr(exts, "EGL_KHR_surfaceless_context") != NULL);
        
        printf("Support for DMA-BUF Import:              %s\n", has_dma ? "YES" : "NO (Zero-Copy will fail)");
        printf("Support for DMA-BUF Modifiers:           %s\n", has_modifiers ? "YES" : "NO");
        printf("Support for KHR-GBM Platform :           %s\n", has_khr_plaform_gbm ? "YES" : "NO");        
        printf("Support for KHR Surfaceless Context :    %s\n", has_khr_surfaceless_context ? "YES" : "NO");        
    } else {
        printf("Extensions: NONE (Critical Error)\n");
    }

    printf("-----------------------\n");
}

int init_kms() {

  // 1. Open DRM Device
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0) {
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if (kms.fd < 0) {
      fprintf(stderr, "Error: Could not open /dev/dri/card0 or card1\n");
      return -1;
    }
  }

  if (drmIsKMS(kms.fd)) {
    printf("DRM node support kernel mode setting.\n");
  } else {
    printf("DRM node does not support kernel mode setting.\n");
  }

  // 2. Setup KMS (Connector, CRTC, Mode)
  drmModeRes *resources = drmModeGetResources(kms.fd);
  if (!resources)
    return -1;

  // Find a connected connector
  for (int i = 0; i < resources->count_connectors; i++) {
    drmModeConnector *conn =
        drmModeGetConnector(kms.fd, resources->connectors[i]);
    if (conn->connection == DRM_MODE_CONNECTED) {
      kms.connector = conn;
      break;
    }
    drmModeFreeConnector(conn);
  }
  // Clean up resources immediately as we only need the connector now
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
    // Re-fetch resources just to find a CRTC if the encoder method failed
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

  // 3. Setup GBM
  kms.gbm_dev = gbm_create_device(kms.fd);
  printf("GBM backend: %s\n", gbm_device_get_backend_name(kms.gbm_dev));

  uint32_t gbm_format = GBM_FORMAT_XRGB8888;
  if (!gbm_device_is_format_supported(
          kms.gbm_dev, gbm_format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING)) {
    fprintf(stderr, "Format (Ox%x) is not supported!\n", gbm_format);
  } else {
    printf("Format (Ox%x) is supported!\n", gbm_format);
  }

  kms.gbm_surf =
      gbm_surface_create(kms.gbm_dev, kms.mode.hdisplay, kms.mode.vdisplay,
                         gbm_format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!kms.gbm_surf)
    return -1;

  // 4. Setup EGL
  kms.egl_disp = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, kms.gbm_dev,
                                       NULL); // EGL_PLATFORM_GBM_MESA
  if (!eglInitialize(kms.egl_disp, NULL, NULL)) {
    fprintf(stderr, "Error: EGL Initialize failed.\n");
    return -1;
  }

  // 5.  Check EGL Properties
  print_egl_diagnostics(kms.egl_disp);

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    fprintf(stderr, "Couldn't bind the egl to GLES api!\n");
    return -1;
  }
  printf("Current egl api: 0x%x\n", eglQueryAPI());

  // --- FIND MATCHING CONFIG ---
  EGLConfig config;
  EGLint num_configs;
  EGLConfig *configs;
  int found_config = 0;

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

  printf("KMS/GBM/EGL Initialized successfully.\n");
  return 0;
}

// --- FLIP HANDLER ---
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

    // 1. Setup Handles/Strides
    for (int i = 0; i < planes; i++) {
        handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
        pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
        offsets[i] = gbm_bo_get_offset(bo, i);
        
        // FIX: Only set modifiers for valid planes. 
        // Setting junk in modifiers[1..3] for a 1-plane format causes EINVAL on strict drivers.
        if (modifier != DRM_FORMAT_MOD_INVALID) {
             modifiers[i] = modifier;
        }
    }

    int ret = -1;

    // ATTEMPT 1: Try AddFB2 with Modifiers (The "Correct" Modern Way)
    if (modifier != DRM_FORMAT_MOD_INVALID) {
        ret = drmModeAddFB2WithModifiers(kms.fd, width, height, format, handles,
                                         pitches, offsets, modifiers, &fb_id,
                                         DRM_MODE_FB_MODIFIERS);
    }

    // ATTEMPT 2: Fallback to AddFB2 without Modifiers (If driver dislikes the specific modifier)
    if (ret) {
        ret = drmModeAddFB2(kms.fd, width, height, format, handles, pitches,
                            offsets, &fb_id, 0);
    }

    // ATTEMPT 3: Ultimate Fallback to Legacy AddFB (The "Safe" Way)
    // This is what fixed it on your Laptop. Laptops often prefer this for simple RGB buffers.
    if (ret) {
        // Assuming XRGB8888 (Depth 24, Bpp 32)
        ret = drmModeAddFB(kms.fd, width, height, 24, 32, pitches[0], handles[0], &fb_id);
    }

    if (ret) {
        // If all 3 failed, we have a real problem
        perror("Error: Failed to create DRM Framebuffer (All attempts failed)");
        return 0; // Return 0 to indicate failure
    }

    return fb_id;
}

// uint32_t get_fb_for_bo(struct gbm_bo *bo) {
//   // uint32_t fb_id;
//   // uint32_t handle = gbm_bo_get_handle(bo).u32;
//   // uint32_t stride = gbm_bo_get_stride(bo);
//   // uint32_t width = gbm_bo_get_width(bo);
//   // uint32_t height = gbm_bo_get_height(bo);
//   // uint32_t bpp = gbm_bo_get_bpp(bo);
//   // uint32_t depth = 24; // DEPENDS ON THE GBM/DRM FORMAT

//   // int ret = drmModeAddFB(kms.fd, width, height, (uint8_t)depth, (uint8_t)bpp,
//   //                        stride, handle, &fb_id);
//   // if (ret) {
//   //   perror("drmModeAddFB");
//   //   return -1;
//   // }
//   // return fb_id;

//   uint32_t fb_id;

//   uint32_t width = gbm_bo_get_width(bo);
//   uint32_t height = gbm_bo_get_height(bo);
//   uint32_t format = gbm_bo_get_format(bo);

//   uint32_t handles[4] = {0};
//   uint32_t pitches[4] = {0};
//   uint32_t offsets[4] = {0};
//   uint64_t modifiers[4] = {0};

//   int planes = gbm_bo_get_plane_count(bo);
//   uint64_t modifier = gbm_bo_get_modifier(bo);

//   /* Fill only valid planes */
//   for (int i = 0; i < planes; i++) {
//     handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
//     pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
//     offsets[i] = gbm_bo_get_offset(bo, i);
//   }

//   uint64_t mod =
//       (modifier != DRM_FORMAT_MOD_INVALID) ? modifier : DRM_FORMAT_MOD_INVALID;

//   /* Zero unused planes explicitly (important!) */
//   for (int i = planes; i < 4; i++) {
//     handles[i] = 0;
//     pitches[i] = 0;
//     offsets[i] = 0;
//     modifiers[i] = mod;
//   }

//   int ret;
//   if (modifier == DRM_FORMAT_MOD_INVALID) {
//     ret = drmModeAddFB2(kms.fd, width, height, format, handles, pitches,
//                         offsets, &fb_id, 0);
//   } else {
//     ret = drmModeAddFB2WithModifiers(kms.fd, width, height, format, handles,
//                                      pitches, offsets, modifiers, &fb_id,
//                                      DRM_MODE_FB_MODIFIERS);
//   }

//   if (ret) {
//     perror("drmModeAddFB2");
//     return -1;
//   }

//   return fb_id;
// }

void swap_buffers_kms() {
  // 1. Finish GL Rendering
  eglSwapBuffers(kms.egl_disp, kms.egl_surf);

  // 2. Lock the newly rendered buffer
  struct gbm_bo *next_bo = gbm_surface_lock_front_buffer(kms.gbm_surf);
  if (!next_bo) {
    fprintf(stderr, "Error: Failed to lock front buffer.\n");
    return;
  }
  uint32_t next_fb_id = get_fb_for_bo(next_bo);

  // 3. Flip to screen
  int ret = drmModePageFlip(kms.fd, kms.crtc->crtc_id, next_fb_id,
                            DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);

  if (ret) {
    fprintf(stderr, "Page Flip failed: %d\n", ret);
    gbm_surface_release_buffer(kms.gbm_surf, next_bo);
    // If flip failed, we also need to clean up the FB we just created
    drmModeRmFB(kms.fd, next_fb_id);
    return;
  }

  waiting_for_flip = 1;

  // 4. Cleanup previous buffer
  if (kms.current_bo) {
    gbm_surface_release_buffer(kms.gbm_surf, kms.current_bo);
    drmModeCloseFB(kms.fd, kms.current_fb_id); // drmModeRmFB
  }

  kms.current_bo = next_bo;
  kms.current_fb_id = next_fb_id;
}

void cleanup() {
  printf("Cleaning up resources...\n");

  // 1. Release the current GBM buffer if it exists
  if (kms.current_bo) {
    gbm_surface_release_buffer(kms.gbm_surf, kms.current_bo);
    drmModeRmFB(kms.fd, kms.current_fb_id);
    kms.current_bo = NULL;
  }

  // 2. Destroy EGL (Reverse Order)
  if (kms.egl_disp != EGL_NO_DISPLAY) {
    eglMakeCurrent(kms.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (kms.egl_surf != EGL_NO_SURFACE)
      eglDestroySurface(kms.egl_disp, kms.egl_surf);
    if (kms.egl_ctx != EGL_NO_CONTEXT)
      eglDestroyContext(kms.egl_disp, kms.egl_ctx);
    eglTerminate(kms.egl_disp);
  }

  // 3. Destroy GBM
  if (kms.gbm_surf)
    gbm_surface_destroy(kms.gbm_surf);
  if (kms.gbm_dev)
    gbm_device_destroy(kms.gbm_dev);

  // 4. Destroy DRM/KMS
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

  // Setup event context for VSync
  drmEventContext evctx = {0};
  evctx.version = 2;
  evctx.page_flip_handler = page_flip_handler;
  fd_set fds;

  printf("Starting render loop... Press Ctrl+C to exit.\n");

  while (running) {
    // 1. Wait for previous Flip to finish (VSync)
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
