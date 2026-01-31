#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <drm/drm_fourcc.h>

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

void handle_sigint(int sig) { 
    running = 0; 
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

    // 2. Setup KMS (Connector, CRTC, Mode)
    drmModeRes *resources = drmModeGetResources(kms.fd);
    if (!resources) return -1;

    // Find a connected connector
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(kms.fd, resources->connectors[i]);
        if (conn->connection == DRM_MODE_CONNECTED) {
            kms.connector = conn;
            break;
        }
        drmModeFreeConnector(conn);
    }
    if (!kms.connector) {
        fprintf(stderr, "Error: No connected monitor found.\n");
        return -1;
    }

    // Pick first mode
    kms.mode = kms.connector->modes[0];
    printf("Selected Mode: %dx%d @ %dHz\n", kms.mode.hdisplay, kms.mode.vdisplay, kms.mode.vrefresh);

    // Find encoder/CRTC
    drmModeEncoder *enc = drmModeGetEncoder(kms.fd, kms.connector->encoders[0]);
    if (enc && enc->crtc_id) {
        kms.crtc = drmModeGetCrtc(kms.fd, enc->crtc_id);
    } else {
        kms.crtc = drmModeGetCrtc(kms.fd, resources->crtcs[0]); 
    }
    if (enc) drmModeFreeEncoder(enc);

    // 3. Setup GBM
    kms.gbm_dev = gbm_create_device(kms.fd);
    // GBM_FORMAT_XRGB8888 >-< XR24 (/sys/kernel/debug/dri/1/state) 
    // GBM_FORMAT_RGB888 >-< BG24 
    // GBM_FORMAT_ARGB8888
    uint32_t gbm_format = GBM_FORMAT_XRGB8888; 

    kms.gbm_surf = gbm_surface_create(kms.gbm_dev, kms.mode.hdisplay, kms.mode.vdisplay,
                                      gbm_format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!kms.gbm_surf) return -1;

    // 4. Setup EGL
    kms.egl_disp = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, kms.gbm_dev, NULL);
    eglInitialize(kms.egl_disp, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    // --- FIND MATCHING CONFIG ---
    EGLConfig config;
    EGLint num_configs;
    EGLConfig *configs;
    int found_config = 0;

    if (!eglGetConfigs(kms.egl_disp, NULL, 0, &num_configs)) return -1;
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
        fprintf(stderr, "Error: No matching EGL config for GBM format 0x%x\n", gbm_format);
        return -1;
    }

    EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    kms.egl_ctx = eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT, ctx_attribs);
    kms.egl_surf = eglCreateWindowSurface(kms.egl_disp, config, (EGLNativeWindowType)kms.gbm_surf, NULL);

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
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t width = gbm_bo_get_width(bo);
    uint32_t height = gbm_bo_get_height(bo);

    drmModeAddFB(kms.fd, width, height, 24, 32, stride, handle, &fb_id);
    return fb_id;
}

void swap_buffers_kms() {
    // 1. Finish GL Rendering
    eglSwapBuffers(kms.egl_disp, kms.egl_surf);

    // 2. Lock the newly rendered buffer
    struct gbm_bo *next_bo = gbm_surface_lock_front_buffer(kms.gbm_surf);
    uint32_t next_fb_id = get_fb_for_bo(next_bo);

    // 3. Flip to screen
    int ret = drmModePageFlip(kms.fd, kms.crtc->crtc_id, next_fb_id,
                              DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);

    if (ret) {
        fprintf(stderr, "Page Flip failed: %d\n", ret);
        gbm_surface_release_buffer(kms.gbm_surf, next_bo);
        return;
    }

    waiting_for_flip = 1;

    // 4. Cleanup previous buffer
    if (kms.current_bo) {
        gbm_surface_release_buffer(kms.gbm_surf, kms.current_bo);
    }

    kms.current_bo = next_bo;
    kms.current_fb_id = next_fb_id;
}

int main(int argc, char **argv) {
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
    return 0;
}
