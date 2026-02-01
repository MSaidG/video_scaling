#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// --- EXTENSION FUNCTION POINTERS ---
// We need these to manually talk to the extension APIs
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
glEGLImageTargetRenderbufferStorageOES;

#define NUM_BUFFERS 3

struct DumbBuffer {
  uint32_t handle;
  uint32_t stride;
  uint32_t size;
  int fd;         // DMA-BUF FD
  uint32_t fb_id; // DRM Framebuffer ID

  // OpenGL/EGL Objects linked to this buffer
  EGLImageKHR image;
  GLuint rbo; // Renderbuffer
  GLuint fbo; // Framebuffer Object
};

struct {
  int fd;
  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;

  struct DumbBuffer buffers[NUM_BUFFERS];
  int current_buf_idx;

  EGLDisplay egl_disp;
  EGLContext egl_ctx;
} kms;

volatile sig_atomic_t running = 1;
int waiting_for_flip = 0;

double get_time_sec() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

void handle_sigint(int sig) { running = 0; }

// --- DRM DUMB BUFFER MANAGEMENT ---

int create_dumb_buffer(struct DumbBuffer *buf) {
  // 1. Ask DRM to allocate a "Dumb" buffer (Scanout capable RAM)
  struct drm_mode_create_dumb creq = {0};
  creq.width = kms.mode.hdisplay;
  creq.height = kms.mode.vdisplay;
  creq.bpp = 32;

  if (ioctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
    perror("Create Dumb");
    return -1;
  }

  buf->handle = creq.handle;
  buf->stride = creq.pitch;
  buf->size = creq.size;

  // 2. Create a DRM Framebuffer ID (So KMS can display it)
  if (drmModeAddFB(kms.fd, creq.width, creq.height, 24, 32, creq.pitch,
                   buf->handle, &buf->fb_id)) {
    perror("Add FB");
    return -1;
  }

  // 3. Export as DMA-BUF (So EGL can see it)
  // We convert the "Handle" (Driver specific) to "FD" (Universal)
  if (drmPrimeHandleToFD(kms.fd, buf->handle, DRM_CLOEXEC, &buf->fd)) {
    perror("Prime Export");
    return -1;
  }

  return 0;
}

int bind_buffer_to_gl(struct DumbBuffer *buf) {
  EGLint attribs[] = {EGL_WIDTH,
                      kms.mode.hdisplay,
                      EGL_HEIGHT,
                      kms.mode.vdisplay,
                      EGL_LINUX_DRM_FOURCC_EXT,
                      DRM_FORMAT_XRGB8888, // Must match .bpp=32
                      EGL_DMA_BUF_PLANE0_FD_EXT,
                      buf->fd,
                      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                      0,
                      EGL_DMA_BUF_PLANE0_PITCH_EXT,
                      (int)buf->stride,
                      EGL_NONE};

  buf->image = eglCreateImageKHR(kms.egl_disp, EGL_NO_CONTEXT,
                                 EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
  if (buf->image == EGL_NO_IMAGE_KHR) {
    fprintf(stderr, "Failed to create EGLImage\n");
    return -1;
  }

  // 2. Bind EGL Image to OpenGL Renderbuffer
  // We use a Renderbuffer because we want to Draw INTO it (FBO attachment)
  glGenRenderbuffers(1, &buf->rbo);
  glBindRenderbuffer(GL_RENDERBUFFER, buf->rbo);
  glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, buf->image);

  // 3. Wrap Renderbuffer in a Framebuffer Object (FBO)
  glGenFramebuffers(1, &buf->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, buf->rbo);

  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "FBO Incomplete\n");
    return -1;
  }

  return 0;
}


int init_kms() {
  kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    return -1;

  drmModeRes *res = drmModeGetResources(kms.fd);
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *c = drmModeGetConnector(kms.fd, res->connectors[i]);
    if (c->connection == DRM_MODE_CONNECTED) {
      kms.connector = c;
      break;
    }
    drmModeFreeConnector(c);
  }
  drmModeFreeResources(res);
  if (!kms.connector)
    return -1;

  kms.mode = kms.connector->modes[0];
  drmModeEncoder *enc = drmModeGetEncoder(kms.fd, kms.connector->encoder_id);
  kms.crtc =
      drmModeGetCrtc(kms.fd, enc ? enc->crtc_id : 0); // Simplified fallback
  if (enc)
    drmModeFreeEncoder(enc);

  return 0;
}

int init_egl() {
  kms.egl_disp =
      eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);

  // 2. Fallback: If Surfaceless is missing (e.g., older ZynqMP drivers), try
  // Default
  if (kms.egl_disp == EGL_NO_DISPLAY) {
    printf("Surfaceless platform not found, falling back to Default...\n");
    kms.egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  }

  if (kms.egl_disp == EGL_NO_DISPLAY) {
    fprintf(stderr, "Failed to get EGL display connection\n");
    return -1;
  }

  EGLint major, minor;
  if (!eglInitialize(kms.egl_disp, &major, &minor)) {
    // Detailed error printing
    EGLint err = eglGetError();
    fprintf(stderr, "EGL Init Failed! Error: 0x%x\n", err);
    if (err == EGL_NOT_INITIALIZED) {
      fprintf(stderr, "Hint: This often happens if no X11/Wayland is running "
                      "and 'Surfaceless' is not supported.\n");
    }
    return -1;
  }
  printf("EGL Initialized (Version %d.%d)\n", major, minor);

  // Load Extensions
  eglCreateImageKHR =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  glEGLImageTargetRenderbufferStorageOES =
      (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)eglGetProcAddress(
          "glEGLImageTargetRenderbufferStorageOES");

  if (!eglCreateImageKHR || !glEGLImageTargetRenderbufferStorageOES) {
    fprintf(stderr, "Missing Required EGL/GL Extensions for DMA-BUF\n");
    return -1;
  }

  EGLConfig config;
  EGLint n;
  EGLint attribs[] = {EGL_SURFACE_TYPE,
                      EGL_PBUFFER_BIT, // <--- CRITICAL FIX
                      EGL_RENDERABLE_TYPE,
                      EGL_OPENGL_ES2_BIT,
                      EGL_RED_SIZE,
                      8,
                      EGL_GREEN_SIZE,
                      8,
                      EGL_BLUE_SIZE,
                      8,
                      EGL_ALPHA_SIZE,
                      0, // DUMB buffers usually XRGB (Alpha ignored)
                      EGL_NONE};

  // Choose Config
  if (!eglChooseConfig(kms.egl_disp, attribs, &config, 1, &n) || n != 1) {
    // Fallback: Try with absolutely NO surface requirements (if PBUFFER failed)
    EGLint fallback_attribs[] = {EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_ES2_BIT, EGL_NONE};
    if (!eglChooseConfig(kms.egl_disp, fallback_attribs, &config, 1, &n) ||
        n != 1) {
      fprintf(stderr, "Failed to choose EGL config (Surfaceless)\n");
      return -1;
    }
  }

  // 4. Create Context
  EGLint ctx_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  kms.egl_ctx =
      eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT, ctx_attribs);

  if (kms.egl_ctx == EGL_NO_CONTEXT) {
    fprintf(stderr, "Context creation failed\n");
    return -1;
  }

  // 5. Make Current (NO SURFACE)
  // This is the key: We pass EGL_NO_SURFACE because we have no window.
  if (!eglMakeCurrent(kms.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                      kms.egl_ctx)) {
    fprintf(stderr, "eglMakeCurrent failed (Error 0x%x)\n", eglGetError());
    return -1;
  }

  return 0;
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void *data) {
  *(int *)data = 0;
}

// Helper to destroy the physical dumb buffer memory in Kernel
void destroy_dumb_buffer_memory(struct DumbBuffer *buf) {
  struct drm_mode_destroy_dumb dreq = {0};
  dreq.handle = buf->handle;

  if (drmIoctl(kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq) < 0) {
    perror("Failed to destroy dumb buffer");
  }
}

void cleanup() {
  printf("Cleaning up resources...\n");

  for (int i = 0; i < NUM_BUFFERS; i++) {
    struct DumbBuffer *buf = &kms.buffers[i];

    // A. OpenGL Cleanup
    if (buf->fbo)
      glDeleteFramebuffers(1, &buf->fbo);
    if (buf->rbo)
      glDeleteRenderbuffers(1, &buf->rbo);

    // B. EGL Image Cleanup
    if (buf->image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR) {
      eglDestroyImageKHR(kms.egl_disp, buf->image);
    }

    // C. DRM Framebuffer Cleanup
    // Tells Display Controller to stop using this ID
    if (buf->fb_id) {
      drmModeRmFB(kms.fd, buf->fb_id);
    }

    // D. Close the DMA-BUF File Descriptor
    if (buf->fd >= 0) {
      close(buf->fd);
    }

    // E. Free the physical RAM (Dumb Buffer)
    if (buf->handle) {
      destroy_dumb_buffer_memory(buf);
    }
  }

  // 2. EGL Teardown
  if (kms.egl_disp != EGL_NO_DISPLAY) {
    // Unbind context
    eglMakeCurrent(kms.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);

    if (kms.egl_ctx != EGL_NO_CONTEXT) {
      eglDestroyContext(kms.egl_disp, kms.egl_ctx);
    }
    eglTerminate(kms.egl_disp);
    eglReleaseThread();
  }

  // 3. DRM Cleanup
  if (kms.crtc)
    drmModeFreeCrtc(kms.crtc);
  if (kms.connector)
    drmModeFreeConnector(kms.connector);

  if (kms.fd >= 0) {
    close(kms.fd);
  }

  printf("Cleanup Done.\n");
}


int main() {
  signal(SIGINT, handle_sigint);

  if (init_kms() != 0)
    return -1;
  if (init_egl() != 0)
    return -1;

  // Create 2 buffers for Double Buffering
  for (int i = 0; i < NUM_BUFFERS; i++) {
    if (create_dumb_buffer(&kms.buffers[i]) != 0)
      return -1;
    if (bind_buffer_to_gl(&kms.buffers[i]) != 0)
      return -1;
  }
  printf("Triple Buffering Init (%dx%d)\n", kms.mode.hdisplay,
         kms.mode.vdisplay);
  printf("Initialized: %dx%d \n", kms.mode.hdisplay, kms.mode.vdisplay);

  // Setup event context for VSync
  drmEventContext evctx = {0};
  evctx.version = 2;
  evctx.page_flip_handler = page_flip_handler;
  fd_set fds;

  while (running) {

    while (waiting_for_flip) {
      if (!running)
        break;
      FD_ZERO(&fds);
      FD_SET(kms.fd, &fds);
      int ret = select(kms.fd + 1, &fds, 0, 0, 0);
      if (ret > 0)
        drmHandleEvent(kms.fd, &evctx);
      else if (ret < 0 && errno == EINTR)
        break;
    }
    if (!running)
      break;

    // --- 2. RENDER ---
    struct DumbBuffer *buf = &kms.buffers[kms.current_buf_idx];
    glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo);

    double t = get_time_sec();
    glClearColor((sin(t) + 1) / 2, (sin(t + 2) + 1) / 2, (sin(t + 4) + 1) / 2,
                 1.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    drmModePageFlip(kms.fd, kms.crtc->crtc_id, buf->fb_id,
                    DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
    waiting_for_flip = 1;

    kms.current_buf_idx = (kms.current_buf_idx + 1) % NUM_BUFFERS;
  }

  printf("\nExiting...\n");
  cleanup();
  return 0;
}
