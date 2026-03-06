#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include <linux/videodev2.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// Ensure X410 is defined even on older headers
#ifndef V4L2_PIX_FMT_X410
#define V4L2_PIX_FMT_X410 v4l2_fourcc('X', '4', '1', '0')
#endif

// --- CONFIG ---
const char *CAMERA_DEVICE = "/dev/video0";
#define CAM_WIDTH 1920
#define CAM_HEIGHT 1080
#define CAM_BUF_COUNT 4

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

// --- EXTENSIONS ---
typedef EGLImageKHR(EGLAPIENTRYP PFNEGLCREATEIMAGEKHRPROC)(
    EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer,
    const EGLint *attrib_list);
typedef EGLBoolean(EGLAPIENTRYP PFNEGLDESTROYIMAGEKHRPROC)(EGLDisplay dpy,
                                                           EGLImageKHR image);
typedef void(GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(
    GLenum target, GLeglImageOES image);

PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR = NULL;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR = NULL;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES = NULL;

// --- STRUCTURES ---
typedef struct {
  uint32_t handle;
  uint32_t stride;
  uint32_t size;
  uint32_t fb_id;
  int prime_fd;
  EGLImageKHR egl_img;
  GLuint tex_id;
  GLuint fbo_id;
} DumbBuffer;

struct {
  int fd;
  drmModeConnector *connector;
  drmModeModeInfo mode;
  drmModeCrtc *crtc;
  uint32_t plane_primary_id;
  uint32_t plane_overlay_id;
  DumbBuffer bufs[2];
  EGLDisplay egl_disp;
  EGLContext egl_ctx;
  EGLSurface egl_surf;
  GLuint prog;
  GLuint vbo;
} kms;

typedef struct {
  int index;
  int dbuf_fd;
  EGLImageKHR egl_img;
  GLuint tex_id;
} CamBuffer;

struct {
  int fd;
  CamBuffer bufs[CAM_BUF_COUNT];
} camera;

volatile sig_atomic_t running = 1;

// --- SHADERS ---
const char *vs_src = "attribute vec4 a_pos;\n"
                     "attribute vec2 a_tex;\n"
                     "varying vec2 v_tex;\n"
                     "void main() {\n"
                     "   gl_Position = a_pos;\n"
                     "   v_tex = a_tex;\n"
                     "}\n";

// The Magic Shader: Unpacks 10-bit XVUY memory mapped as 8-bit ARGB bytes
const char *fs_src = "#extension GL_OES_EGL_image_external : require\n"
                     "precision highp float;\n"
                     "varying vec2 v_tex;\n"
                     "uniform samplerExternalOES tex_cam;\n"
                     "void main() {\n"
                     "  vec4 raw = texture2D(tex_cam, v_tex) * 255.0;\n"
                     "  float r = raw.r; float g = raw.g; float b = raw.b; float a = raw.a;\n"
                     
                     // Decode 10-bit Y (bits 0-9)
                     "  float y_upper = mod(g, 4.0);\n"
                     "  float Y = r + (y_upper * 256.0);\n"
                     "  float y_norm = Y / 1023.0;\n"
                     
                     // Decode 10-bit U (bits 10-19)
                     "  float u_lower = floor(g / 4.0);\n"
                     "  float u_upper = mod(b, 16.0);\n"
                     "  float U = u_lower + (u_upper * 64.0);\n"
                     "  float u_norm = U / 1023.0;\n"
                     
                     // Decode 10-bit V (bits 20-29)
                     "  float v_lower = floor(b / 16.0);\n"
                     "  float v_upper = mod(a, 64.0);\n"
                     "  float V = v_lower + (v_upper * 16.0);\n"
                     "  float v_norm = V / 1023.0;\n"

                     // Convert to RGB
                     "  float d = u_norm - 0.5;\n"
                     "  float e = v_norm - 0.5;\n"
                     "  float red = y_norm + 1.402 * e;\n"
                     "  float green = y_norm - 0.344 * d - 0.714 * e;\n"
                     "  float blue = y_norm + 1.772 * d;\n"
                     
                     "  gl_FragColor = vec4(red, green, blue, 1.0);\n"
                     "}\n";

// --- HELPERS ---
void handle_sigint(int sig) { running = 0; }

long get_diff_us(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000000 +
         (end.tv_nsec - start.tv_nsec) / 1000;
}

int load_egl_extensions() {
  eglCreateImageKHR =
      (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  eglDestroyImageKHR =
      (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  glEGLImageTargetTexture2DOES =
      (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
          "glEGLImageTargetTexture2DOES");
  return (eglCreateImageKHR && glEGLImageTargetTexture2DOES) ? 0 : -1;
}

// --- SETUP DRM ---
int create_dumb_buffer_fbo(DumbBuffer *buf) {
  struct drm_mode_create_dumb create_req = {0};
  create_req.width = kms.mode.hdisplay;
  create_req.height = kms.mode.vdisplay;
  create_req.bpp = 32;
  ioctl(kms.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req);

  buf->handle = create_req.handle;
  buf->stride = create_req.pitch;
  buf->size = create_req.size;

  drmModeAddFB(kms.fd, kms.mode.hdisplay, kms.mode.vdisplay, 24, 32,
               buf->stride, buf->handle, &buf->fb_id);

  struct drm_prime_handle prime = {0};
  prime.handle = buf->handle;
  prime.flags = DRM_CLOEXEC | DRM_RDWR;
  ioctl(kms.fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
  buf->prime_fd = prime.fd;

  EGLint attribs[] = {EGL_WIDTH, kms.mode.hdisplay,
                      EGL_HEIGHT, kms.mode.vdisplay,
                      EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
                      EGL_DMA_BUF_PLANE0_FD_EXT, buf->prime_fd,
                      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
                      EGL_DMA_BUF_PLANE0_PITCH_EXT, buf->stride,
                      EGL_NONE};

  buf->egl_img = eglCreateImageKHR(kms.egl_disp, EGL_NO_CONTEXT,
                                   EGL_LINUX_DMA_BUF_EXT, NULL, attribs);

  glGenTextures(1, &buf->tex_id);
  glBindTexture(GL_TEXTURE_2D, buf->tex_id);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, buf->egl_img);

  glGenFramebuffers(1, &buf->fbo_id);
  glBindFramebuffer(GL_FRAMEBUFFER, buf->fbo_id);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         buf->tex_id, 0);
  return 0;
}

// --- SETUP V4L2 CAMERA (MULTIPLANAR API) ---
int init_camera() {
  printf("Initializing V4L2 Camera on %s...\n", CAMERA_DEVICE);
  camera.fd = open(CAMERA_DEVICE, O_RDWR | O_NONBLOCK);
  if (camera.fd < 0) {
    perror("Failed to open camera");
    return -1;
  }

  // 1. Set Multiplanar Format (Request 10-bit X410 natively)
  struct v4l2_format fmt = {0};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_X410;
  fmt.fmt.pix_mp.width = CAM_WIDTH;
  fmt.fmt.pix_mp.height = CAM_HEIGHT;
  fmt.fmt.pix_mp.num_planes = 1;

  if (ioctl(camera.fd, VIDIOC_S_FMT, &fmt) < 0) {
    perror("Failed to set camera format");
    return -1;
  }
  
  int pitch = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
  printf("Camera format set to X410 %dx%d (Multiplanar, Pitch: %d)\n", 
         fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height, pitch);

  // 2. Request Multiplanar Buffers
  struct v4l2_requestbuffers req = {0};
  req.count = CAM_BUF_COUNT;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;

  if (ioctl(camera.fd, VIDIOC_REQBUFS, &req) < 0) {
    perror("Failed to request buffers");
    return -1;
  }

  // Pre-map all camera buffers directly to OpenGL Textures
  for (int i = 0; i < CAM_BUF_COUNT; i++) {
      
    struct v4l2_plane planes[1] = {0};
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    buf.length = 1;
    buf.m.planes = planes;

    // **THE MISSING LINK** - Query the buffer to let the kernel populate the exact lengths
    if (ioctl(camera.fd, VIDIOC_QUERYBUF, &buf) < 0) {
      perror("Failed to query buffer lengths");
      return -1;
    }

    // 3. Export DMABUF FD using Multiplanar type
    struct v4l2_exportbuffer expbuf = {0};
    expbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    expbuf.index = i;
    expbuf.plane = 0; 
    
    if (ioctl(camera.fd, VIDIOC_EXPBUF, &expbuf) < 0) {
      perror("Failed to export buffer");
      return -1;
    }

    camera.bufs[i].index = i;
    camera.bufs[i].dbuf_fd = expbuf.fd;

    // Map DMABUF directly to an EGL Image as standard ARGB8888 
    EGLint egl_img_attr[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_DMA_BUF_PLANE0_FD_EXT, camera.bufs[i].dbuf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, pitch, 
        EGL_WIDTH, CAM_WIDTH,
        EGL_HEIGHT, CAM_HEIGHT,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888, // Lying to the GPU here!
        EGL_NONE
    };

    camera.bufs[i].egl_img = eglCreateImageKHR(
        kms.egl_disp, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, egl_img_attr);

    if (camera.bufs[i].egl_img == EGL_NO_IMAGE_KHR) {
      fprintf(stderr, "Failed to create EGL image for buffer %d\n", i);
      return -1;
    }

    // Create a persistent OpenGL texture
    glGenTextures(1, &camera.bufs[i].tex_id);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, camera.bufs[i].tex_id);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, camera.bufs[i].egl_img);

    // 4. Queue Multiplanar Buffer using the properly sized arrays from QUERYBUF
    if (ioctl(camera.fd, VIDIOC_QBUF, &buf) < 0) {
      perror("Failed to queue buffer");
      return -1;
    }
  }

  // Start the camera stream
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (ioctl(camera.fd, VIDIOC_STREAMON, &type) < 0) {
    perror("Failed to start stream");
    return -1;
  }
  printf("Camera streaming started successfully.\n");
  return 0;
}

void update_geometry() {
  GLfloat verts[] = {
    // x,      y,     u,    v
    -1.0f,  1.0f,  0.0f, 1.0f, // TL
    -1.0f, -1.0f,  0.0f, 0.0f, // BL
     1.0f,  1.0f,  1.0f, 1.0f, // TR
     1.0f,  1.0f,  1.0f, 1.0f, // TR
    -1.0f, -1.0f,  0.0f, 0.0f, // BL
     1.0f, -1.0f,  1.0f, 0.0f  // BR
  };
  glBindBuffer(GL_ARRAY_BUFFER, kms.vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
}

void cleanup() {
  printf("\n--- Cleaning Up ---\n");

  if (camera.fd >= 0) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(camera.fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < CAM_BUF_COUNT; i++) {
      if (camera.bufs[i].tex_id) glDeleteTextures(1, &camera.bufs[i].tex_id);
      if (camera.bufs[i].egl_img) eglDestroyImageKHR(kms.egl_disp, camera.bufs[i].egl_img);
      if (camera.bufs[i].dbuf_fd >= 0) close(camera.bufs[i].dbuf_fd);
    }
    close(camera.fd);
  }

  if (kms.prog) glDeleteProgram(kms.prog);
  if (kms.vbo) glDeleteBuffers(1, &kms.vbo);

  for (int i = 0; i < 2; i++) {
    if (kms.bufs[i].fbo_id) glDeleteFramebuffers(1, &kms.bufs[i].fbo_id);
    if (kms.bufs[i].tex_id) glDeleteTextures(1, &kms.bufs[i].tex_id);
    if (kms.bufs[i].egl_img && eglDestroyImageKHR) eglDestroyImageKHR(kms.egl_disp, kms.bufs[i].egl_img);
    if (kms.bufs[i].prime_fd >= 0) close(kms.bufs[i].prime_fd);
    if (kms.bufs[i].fb_id) drmModeRmFB(kms.fd, kms.bufs[i].fb_id);
    if (kms.bufs[i].handle) {
      struct drm_mode_destroy_dumb destroy_req = {.handle = kms.bufs[i].handle};
      ioctl(kms.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
    }
  }

  if (kms.egl_disp != EGL_NO_DISPLAY) {
    eglMakeCurrent(kms.egl_disp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglTerminate(kms.egl_disp);
  }
  if (kms.crtc) drmModeFreeCrtc(kms.crtc);
  if (kms.connector) drmModeFreeConnector(kms.connector);
  if (kms.fd >= 0) close(kms.fd);
  printf("Done.\n");
}

int main(int argc, char **argv) {
  signal(SIGINT, handle_sigint);

  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0) kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0) return -1;

  drmModeRes *res = drmModeGetResources(kms.fd);
  if (!res) return -1;

  kms.connector = NULL;
  for (int i = 0; i < res->count_connectors; i++) {
    drmModeConnector *conn = drmModeGetConnector(kms.fd, res->connectors[i]);
    if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      kms.connector = conn;
      break;
    }
    if (conn) drmModeFreeConnector(conn);
  }
  if (!kms.connector) return -1;

  kms.mode = kms.connector->modes[0];
  printf("Display mode: %dx%d @ %dHz\n", kms.mode.hdisplay, kms.mode.vdisplay, kms.mode.vrefresh);

  kms.crtc = drmModeGetCrtc(kms.fd, res->crtcs[0]);
  if (!kms.crtc) return -1;
  drmModeFreeResources(res);

  kms.plane_primary_id = 39;
  kms.plane_overlay_id = 41;

  kms.egl_disp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!eglInitialize(kms.egl_disp, NULL, NULL)) {
    kms.egl_disp = eglGetDisplay((EGLNativeDisplayType)kms.fd);
    eglInitialize(kms.egl_disp, NULL, NULL);
  }
  eglBindAPI(EGL_OPENGL_ES_API);

  EGLConfig config;
  EGLint num;
  EGLint attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
  eglChooseConfig(kms.egl_disp, attribs, &config, 1, &num);
  kms.egl_surf = eglCreatePbufferSurface(kms.egl_disp, config, (EGLint[]){EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE});
  kms.egl_ctx = eglCreateContext(kms.egl_disp, config, EGL_NO_CONTEXT, (EGLint[]){EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE});
  eglMakeCurrent(kms.egl_disp, kms.egl_surf, kms.egl_surf, kms.egl_ctx);
  load_egl_extensions();

  create_dumb_buffer_fbo(&kms.bufs[0]);
  create_dumb_buffer_fbo(&kms.bufs[1]);

  if (init_camera() < 0) {
    cleanup();
    return -1;
  }

  kms.prog = glCreateProgram();
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_src, NULL);
  glCompileShader(vs);
  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_src, NULL);
  glCompileShader(fs);
  glAttachShader(kms.prog, vs);
  glAttachShader(kms.prog, fs);
  glLinkProgram(kms.prog);
  glUseProgram(kms.prog);

  glGenBuffers(1, &kms.vbo);
  glBindBuffer(GL_ARRAY_BUFFER, kms.vbo);
  glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), NULL, GL_DYNAMIC_DRAW);
  update_geometry();

  GLint loc_pos = glGetAttribLocation(kms.prog, "a_pos");
  GLint loc_tex = glGetAttribLocation(kms.prog, "a_tex");
  int stride = 4 * sizeof(float);
  glEnableVertexAttribArray(loc_pos);
  glVertexAttribPointer(loc_pos, 2, GL_FLOAT, GL_FALSE, stride, (void *)0);
  glEnableVertexAttribArray(loc_tex);
  glVertexAttribPointer(loc_tex, 2, GL_FLOAT, GL_FALSE, stride, (void *)(2 * sizeof(float)));

  glUniform1i(glGetUniformLocation(kms.prog, "tex_cam"), 0);

  drmModeSetPlane(kms.fd, kms.plane_overlay_id, kms.crtc->crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

  int back_buf = 0;
  struct pollfd fds = { .fd = camera.fd, .events = POLLIN };
  
  struct timespec sec_start, sec_end;
  clock_gettime(CLOCK_MONOTONIC, &sec_start);
  int frames_this_sec = 0;

  printf("Running Raw V4L2 Zero-Copy Loop... Press Ctrl+C to exit.\n");

  while (running) {
    if (poll(&fds, 1, 5000) > 0 && (fds.revents & POLLIN)) {
      
      struct v4l2_plane planes[1] = {0};
      struct v4l2_buffer vb = {0};
      vb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
      vb.memory = V4L2_MEMORY_MMAP;
      vb.length = 1;
      vb.m.planes = planes;

      if (ioctl(camera.fd, VIDIOC_DQBUF, &vb) < 0) {
        perror("VIDIOC_DQBUF failed");
        continue;
      }

      glBindFramebuffer(GL_FRAMEBUFFER, kms.bufs[back_buf].fbo_id);
      glViewport(0, 0, kms.mode.hdisplay, kms.mode.vdisplay);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_EXTERNAL_OES, camera.bufs[vb.index].tex_id);
      
      glDrawArrays(GL_TRIANGLES, 0, 6);

      // Requeue the buffer exactly as it was dequeued
      if (ioctl(camera.fd, VIDIOC_QBUF, &vb) < 0) {
        perror("VIDIOC_QBUF failed");
      }

      drmModeSetPlane(kms.fd, kms.plane_primary_id, kms.crtc->crtc_id,
                      kms.bufs[back_buf].fb_id, 0, 0, 0, kms.mode.hdisplay,
                      kms.mode.vdisplay, 0, 0, kms.mode.hdisplay << 16,
                      kms.mode.vdisplay << 16);
      back_buf = !back_buf;

      frames_this_sec++;
      clock_gettime(CLOCK_MONOTONIC, &sec_end);
      if (get_diff_us(sec_start, sec_end) >= 1000000) {
        printf("FPS: %d\n", frames_this_sec);
        frames_this_sec = 0;
        sec_start = sec_end;
      }
    }
  }

  cleanup();
  return 0;
}