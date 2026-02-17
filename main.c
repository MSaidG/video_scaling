#include <fcntl.h>
#include <ncurses.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
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
#define VIDEO_COUNT 4
#define RAW_FILE_1 "videos/smpte_nv12_1080p60.yuv"
#define RAW_FILE_2 "videos/smpte_nv12_480p30.yuv"
#define RAW_FILE_3 "videos/smpte_nv12_720p30.yuv"
#define RAW_FILE_4 "videos/smpte_nv12_600x600p30.yuv"

#define VID_W 1920
#define VID_H 1080
// #define FPS 60

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

typedef struct {
  const char *filename;
  int fd;
  unsigned char *data; // Memory mapped file
  size_t size;
  size_t frame_size;
  int total_frames;
  int curr_frame_idx;
  GLuint tex_y;
  GLuint tex_uv;
  int width;
  int height;
} VideoSource;

VideoSource videos[VIDEO_COUNT];

volatile sig_atomic_t running = 1;
int waiting_for_flip = 0;

// --- LAYOUT STATE MANAGEMENT ---
// Maps Screen Position [0..3] -> Video Source Index [0..3]
// 0: TL, 1: TR, 2: BL, 3: BR
volatile int layout[4] = {0, 1, 2, 3};

// State for the Input Machine
volatile int selected_slot = -1; // -1 means nothing selected
volatile int in_change_mode = 0; // 0 = false, 1 = true
volatile int in_resize_mode = 0;

// Geometry State
typedef struct {
  float x, y, w, h;
} Rect;

// Helper Constants for Shapes
const Rect RECT_FULL = {-1.0f, -1.0f, 2.0f, 2.0f};
const Rect RECT_TOP = {-1.0f, 0.0f, 2.0f, 1.0f};
const Rect RECT_BOTTOM = {-1.0f, -1.0f, 2.0f, 1.0f};
const Rect RECT_LEFT = {-1.0f, -1.0f, 1.0f, 2.0f};
const Rect RECT_RIGHT = {0.0f, -1.0f, 1.0f, 2.0f};

volatile Rect slot_rects[4];
// Default positions
const Rect default_rects[4] = {
    {-1.0f, 0.0f, 1.0f, 1.0f},  // TL
    {0.0f, 0.0f, 1.0f, 1.0f},   // TR
    {-1.0f, -1.0f, 1.0f, 1.0f}, // BL
    {0.0f, -1.0f, 1.0f, 1.0f}   // BR
};

volatile int layout_dirty = 1; // Flag to tell Main Thread to re-upload vertices

// --- UTILS ---
void handle_signal(int s) { running = 0; }

// Helper: Check if two rects overlap
int rects_overlap(Rect r1, Rect r2) {
  if (r1.w == 0 || r1.h == 0 || r2.w == 0 || r2.h == 0)
    return 0; // Ignore hidden
  return r1.x < r2.x + r2.w && r1.x + r1.w > r2.x && r1.y < r2.y + r2.h &&
         r1.y + r1.h > r2.y;
}

// Helper: Reset grid to default
void reset_layout() {
  for (int i = 0; i < 4; i++)
    slot_rects[i] = default_rects[i];
  layout_dirty = 1;
}

const char *vs_src = "attribute vec4 pos;\n"
                     "attribute vec2 tex;\n"
                     "attribute float a_vid;\n"
                     "varying vec2 v_tex;\n"
                     "varying float v_vid;\n"
                     "void main() {\n"
                     "  gl_Position = pos;\n"
                     "  v_tex = tex;\n"
                     "  v_vid = a_vid;\n"
                     "}\n";

const char *fs_src = "precision mediump float;\n"
                     "varying vec2 v_tex;\n"
                     "varying float v_vid;\n"
                     // 8 Samplers (4 Videos x 2 Planes)
                     "uniform sampler2D ty0; uniform sampler2D tu0;\n"
                     "uniform sampler2D ty1; uniform sampler2D tu1;\n"
                     "uniform sampler2D ty2; uniform sampler2D tu2;\n"
                     "uniform sampler2D ty3; uniform sampler2D tu3;\n"

                     "vec3 yuv2rgb(float y, float u, float v) {\n"
                     "  float r = y + 1.402 * v;\n"
                     "  float g = y - 0.344 * u - 0.714 * v;\n"
                     "  float b = y + 1.772 * u;\n"
                     "  return vec3(r, g, b);\n"
                     "}\n"

                     "void main() {\n"
                     "  float y, u, v;\n"
                     "  if (v_vid < 0.5) {\n" // Video 0
                     "    y = texture2D(ty0, v_tex).r;\n"
                     "    vec4 uv = texture2D(tu0, v_tex);\n"
                     "    u = uv.r - 0.5; v = uv.a - 0.5;\n"
                     "  } else if (v_vid < 1.5) {\n" // Video 1
                     "    y = texture2D(ty1, v_tex).r;\n"
                     "    vec4 uv = texture2D(tu1, v_tex);\n"
                     "    u = uv.r - 0.5; v = uv.a - 0.5;\n"
                     "  } else if (v_vid < 2.5) {\n" // Video 2
                     "    y = texture2D(ty2, v_tex).r;\n"
                     "    vec4 uv = texture2D(tu2, v_tex);\n"
                     "    u = uv.r - 0.5; v = uv.a - 0.5;\n"
                     "  } else {\n" // Video 3
                     "    y = texture2D(ty3, v_tex).r;\n"
                     "    vec4 uv = texture2D(tu3, v_tex);\n"
                     "    u = uv.r - 0.5; v = uv.a - 0.5;\n"
                     "  }\n"
                     "  gl_FragColor = vec4(yuv2rgb(y, u, v), 1.0);\n"
                     "}\n";

// --- GEOMETRY UPDATE FUNCTION ---
// Called by Main Thread when layout_dirty is true
void update_geometry_buffer(GLuint vbo) {
  // 4 quads * 6 verts/quad * 5 floats/vert (x,y, u,v, id)
  GLfloat verts[4 * 6 * 5];
  int idx = 0;

  for (int i = 0; i < 4; i++) {
    Rect r = slot_rects[i];
    float id = (float)i;

    // Define Quad vertices based on Rect r (x,y is bottom-left of rect in
    // standard GL, but our logic uses Top-Left origin for convenience or BL?
    // Let's stick to standard GL coords: -1,-1 is BL.

    // Vertices: X, Y, U, V, ID

    // Triangle 1
    // TL
    verts[idx++] = r.x;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 0.0f;
    verts[idx++] = 0.0f;
    verts[idx++] = id;
    // BL
    verts[idx++] = r.x;
    verts[idx++] = r.y;
    verts[idx++] = 0.0f;
    verts[idx++] = 1.0f;
    verts[idx++] = id;
    // TR
    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 1.0f;
    verts[idx++] = 0.0f;
    verts[idx++] = id;

    // Triangle 2
    // TR
    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y + r.h;
    verts[idx++] = 1.0f;
    verts[idx++] = 0.0f;
    verts[idx++] = id;
    // BL
    verts[idx++] = r.x;
    verts[idx++] = r.y;
    verts[idx++] = 0.0f;
    verts[idx++] = 1.0f;
    verts[idx++] = id;
    // BR
    verts[idx++] = r.x + r.w;
    verts[idx++] = r.y;
    verts[idx++] = 1.0f;
    verts[idx++] = 1.0f;
    verts[idx++] = id;
  }

  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
  layout_dirty = 0;
}

int init_kms() {
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card2", O_RDWR | O_CLOEXEC);
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

int init_video_source(VideoSource *v, const char *filename, int width,
                      int height) {
  v->filename = filename;
  v->fd = open(filename, O_RDONLY);
  if (v->fd < 0) {
    fprintf(stderr, "Failed to open %s\n", filename);
    return -1;
  }

  struct stat sb;
  fstat(v->fd, &sb);
  v->size = sb.st_size;
  v->frame_size = width * height * 3 / 2; // NV12
  v->total_frames = v->size / v->frame_size;
  v->curr_frame_idx = 0;
  v->width = width;
  v->height = height;

  v->data = mmap(NULL, v->size, PROT_READ, MAP_PRIVATE, v->fd, 0);
  if (v->data == MAP_FAILED)
    return -1;

  // Create Textures
  glGenTextures(1, &v->tex_y);
  glBindTexture(GL_TEXTURE_2D, v->tex_y);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glGenTextures(1, &v->tex_uv);
  glBindTexture(GL_TEXTURE_2D, v->tex_uv);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  printf("Init Video: %s (%d frames)\n", filename, v->total_frames);
  return 0;
}

void update_texture(VideoSource *v) {
  unsigned char *frame_start = v->data + (v->curr_frame_idx * v->frame_size);
  unsigned char *uv_start = frame_start + (v->width * v->height);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, v->tex_y);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, v->width, v->height, 0,
               GL_LUMINANCE, GL_UNSIGNED_BYTE, frame_start);

  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, v->tex_uv);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, v->width / 2,
               v->height / 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
               uv_start);

  v->curr_frame_idx = (v->curr_frame_idx + 1) % v->total_frames;
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

  // drmModePageFlip(kms.fd, kms.crtc->crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT,
  //                 &waiting_for_flip);

  // TRY ASYNC FLIP
  // DRM_MODE_PAGE_FLIP_ASYNC (0x02) tells the driver: "Flip immediately, don't
  // wait for VSYNC"
  int ret = drmModePageFlip(kms.fd, kms.crtc->crtc_id, fb,
                            DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_PAGE_FLIP_ASYNC,
                            &waiting_for_flip);

  // Fallback: Some drivers don't support ASYNC. If it fails, fall back to
  // standard VSync.
  if (ret < 0) {
    drmModePageFlip(kms.fd, kms.crtc->crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT,
                    &waiting_for_flip);
  }

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

  for (int i = 0; i < VIDEO_COUNT; ++i) {
    // 1. Clean up Camera/Input Resources
    if (videos[i].data && videos[i].data != MAP_FAILED) {
      munmap(videos[i].data, videos[i].size);
    }
    if (videos[i].fd >= 0) {
      close(videos[i].fd);
    }

    // Clean up textures (if GL context is still alive)
    if (videos[i].tex_y)
      glDeleteTextures(1, &videos[i].tex_y);
    if (videos[i].tex_uv)
      glDeleteTextures(1, &videos[i].tex_uv);
  }

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

// Uploads data to GPU, but does NOT draw.
void upload_video_frame(VideoSource *v, int base_unit) {
  unsigned char *f = v->data + (v->curr_frame_idx * v->frame_size);

  glActiveTexture(GL_TEXTURE0 + base_unit);
  glBindTexture(GL_TEXTURE_2D, v->tex_y);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, v->width, v->height, 0,
               GL_LUMINANCE, GL_UNSIGNED_BYTE, f);

  glActiveTexture(GL_TEXTURE0 + base_unit + 1);
  glBindTexture(GL_TEXTURE_2D, v->tex_uv);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE_ALPHA, v->width / 2,
               v->height / 2, 0, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE,
               f + (v->width * v->height));

  v->curr_frame_idx = (v->curr_frame_idx + 1) % v->total_frames;
}

// --- DEBUG HELPER ---
void check_shader(GLuint shader, const char *name) {
  GLint success;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  if (!success) {
    char infoLog[512];
    glGetShaderInfoLog(shader, 512, NULL, infoLog);
    fprintf(stderr, "ERROR::%s::COMPILATION_FAILED\n%s\n", name, infoLog);
    exit(1);
  }
}

void check_program(GLuint program) {
  GLint success;
  glGetProgramiv(program, GL_LINK_STATUS, &success);
  if (!success) {
    char infoLog[512];
    glGetProgramInfoLog(program, 512, NULL, infoLog);
    fprintf(stderr, "ERROR::PROGRAM::LINKING_FAILED\n%s\n", infoLog);
    exit(1);
  }
}

// Returns current time in milliseconds
long long current_timestamp() {
  struct timeval te;
  gettimeofday(&te, NULL); // get current time
  long long milliseconds = te.tv_sec * 1000LL + te.tv_usec / 1000;
  return milliseconds;
}

void exit_program() { running = false; }

void apply_resize(int slot, int key) {
  // 1. First, set default layout positions so we know where everyone starts.
  reset_layout();

  int filler = -1; // Who will fill the empty space?

  if (slot == 0) { // TL
    if (key == KEY_UP) {
      slot_rects[0] = RECT_TOP;
    } // Safe (Covered)
    if (key == KEY_LEFT) {
      slot_rects[0] = RECT_LEFT;
    } // Safe (Covered)
    if (key == KEY_DOWN) {
      slot_rects[0] = RECT_BOTTOM;
      filler = 1;
      slot_rects[filler] = RECT_TOP;
    }
    if (key == KEY_RIGHT) {
      slot_rects[0] = RECT_RIGHT;
      filler = 2;
      slot_rects[filler] = RECT_LEFT;
    }
  } else if (slot == 1) { // TR
    if (key == KEY_UP) {
      slot_rects[1] = RECT_TOP;
    } // Safe
    if (key == KEY_RIGHT) {
      slot_rects[1] = RECT_RIGHT;
    } // Safe
    if (key == KEY_DOWN) {
      slot_rects[1] = RECT_BOTTOM;
      filler = 0;
      slot_rects[filler] = RECT_TOP;
    }
    if (key == KEY_LEFT) {
      slot_rects[1] = RECT_LEFT;
      filler = 3;
      slot_rects[filler] = RECT_RIGHT;
    }
  } else if (slot == 2) { // BL
    if (key == KEY_DOWN) {
      slot_rects[2] = RECT_BOTTOM;
    } // Safe
    if (key == KEY_LEFT) {
      slot_rects[2] = RECT_LEFT;
    } // Safe
    if (key == KEY_UP) {
      slot_rects[2] = RECT_TOP;
      filler = 3;
      slot_rects[filler] = RECT_BOTTOM;
    }
    if (key == KEY_RIGHT) {
      slot_rects[2] = RECT_RIGHT;
      filler = 0;
      slot_rects[filler] = RECT_LEFT;
    }
  } else if (slot == 3) { // BR
    if (key == KEY_DOWN) {
      slot_rects[3] = RECT_BOTTOM;
    } // Safe
    if (key == KEY_RIGHT) {
      slot_rects[3] = RECT_RIGHT;
    } // Safe
    if (key == KEY_UP) {
      slot_rects[3] = RECT_TOP;
      filler = 2;
      slot_rects[filler] = RECT_BOTTOM;
    }
    if (key == KEY_LEFT) {
      slot_rects[3] = RECT_LEFT;
      filler = 1;
      slot_rects[filler] = RECT_RIGHT;
    }
  }

  // 2. Hide Loop: Clean up overlaps
  // Any video that overlaps the 'Selected' or the 'Filler' must be hidden.
  for (int i = 0; i < 4; i++) {
    if (i == slot)
      continue; // Don't check self
    if (filler != -1 && i == filler)
      continue; // Don't check filler

    // Check against selected
    if (rects_overlap(slot_rects[slot], slot_rects[i])) {
      slot_rects[i].w = 0;
      slot_rects[i].h = 0;
    }
    // Check against filler (if it exists)
    if (filler != -1 && rects_overlap(slot_rects[filler], slot_rects[i])) {
      slot_rects[i].w = 0;
      slot_rects[i].h = 0;
    }
  }

  layout_dirty = 1;
}

void *input_thread(void *arg) {
  initscr();
  cbreak();
  noecho();
  nodelay(stdscr, TRUE);
  keypad(stdscr, TRUE);

  while (running) {
    int ch = getch();
    if (ch != ERR) {
      if (ch == 'q')
        exit_program();

      int pressed_slot = -1;
      if (ch >= '1' && ch <= '4')
        pressed_slot = ch - '1';

      if (ch == '0') {
        reset_layout();
        in_resize_mode = 0;
        in_change_mode = 0;
        selected_slot = -1;
      } else if (in_resize_mode) {
        if (pressed_slot != -1) {
          selected_slot = pressed_slot;
        } else if (ch == 'f') {
          if (selected_slot != -1) {
            // Fullscreen is safe (hide all others)
            for (int i = 0; i < 4; i++) {
              slot_rects[i].w = 0;
              slot_rects[i].h = 0;
            }
            slot_rects[selected_slot] = RECT_FULL;
            layout_dirty = 1;
          }
        } else if (ch == KEY_LEFT || ch == KEY_RIGHT || ch == KEY_UP ||
                   ch == KEY_DOWN) {
          if (selected_slot != -1) {
            apply_resize(selected_slot, ch);
          }
        } else if (ch == 'r') {
          in_resize_mode = 0;
        }
      } else if (in_change_mode) {
        if (pressed_slot != -1) {
          int src = selected_slot;
          int dst = pressed_slot;
          if (src != dst) {
            int tmp = layout[src];
            layout[src] = layout[dst];
            layout[dst] = tmp;
          }
          in_change_mode = 0;
          selected_slot = -1;
        } else {
          in_change_mode = 0;
          selected_slot = -1;
        }
      } else {
        if (pressed_slot != -1) {
          selected_slot = pressed_slot;
        } else if (ch == 'c') {
          if (selected_slot != -1)
            in_change_mode = 1;
        } else if (ch == 'r') {
          if (selected_slot != -1)
            in_resize_mode = 1;
        } else {
          selected_slot = -1;
        }
      }
    }
    usleep(10000);
  }
  return NULL;
}

// Helper to calculate time difference in microseconds (us)
long get_diff_us(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000000 +
         (end.tv_nsec - start.tv_nsec) / 1000;
}

int main() {
  signal(SIGINT, handle_signal);

  if (init_kms() != 0)
    return 1;

  if (init_video_source(&videos[0], RAW_FILE_1, 1920, 1080) != 0)
    return 1;
  if (init_video_source(&videos[1], RAW_FILE_2, 640, 480) != 0)
    return 1;
  if (init_video_source(&videos[2], RAW_FILE_3, 1280, 720) != 0)
    return 1;
  if (init_video_source(&videos[3], RAW_FILE_4, 600, 600) != 0)
    return 1;

  // Compile Shaders
  GLuint p = glCreateProgram();

  GLuint v = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(v, 1, &vs_src, 0);
  glCompileShader(v);
  check_shader(v, "Vertex"); // CHECK ERRORS

  GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(f, 1, &fs_src, 0);
  glCompileShader(f);
  check_shader(f, "Fragment"); // CHECK ERRORS

  glAttachShader(p, v);
  glAttachShader(p, f);

  glLinkProgram(p);
  check_program(p);

  glUseProgram(p);

  glUniform1i(glGetUniformLocation(p, "ty0"), 0);
  glUniform1i(glGetUniformLocation(p, "tu0"), 1);
  glUniform1i(glGetUniformLocation(p, "ty1"), 2);
  glUniform1i(glGetUniformLocation(p, "tu1"), 3);
  glUniform1i(glGetUniformLocation(p, "ty2"), 4);
  glUniform1i(glGetUniformLocation(p, "tu2"), 5);
  glUniform1i(glGetUniformLocation(p, "ty3"), 6);
  glUniform1i(glGetUniformLocation(p, "tu3"), 7);

  reset_layout();

  // --- VBO SETUP ---
  GLuint vbo;
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  // Allocate space for 4 quads * 6 verts * 5 floats
  glBufferData(GL_ARRAY_BUFFER, 4 * 6 * 5 * sizeof(GLfloat), NULL,
               GL_DYNAMIC_DRAW);

  // Initial populate
  update_geometry_buffer(vbo);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 20, (void *)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void *)8);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 20, (void *)16);

  drmEventContext ev = {0};
  ev.version = 2;
  ev.page_flip_handler = page_flip_handler;
  fd_set fds;

  // --- FPS VARIABLES ---
  long long last_time = current_timestamp();
  int frame_count = 0;

  // --- INPUT THREAD ---
  pthread_t tid;
  pthread_create(&tid, NULL, input_thread, NULL);

  // --- PROFILERS ---
  struct timespec t0, t1, t2, t3, t4;

  // Accumulators for profiling (in microseconds)
  long acc_wait = 0;
  long acc_upload = 0;
  long acc_draw = 0;
  long acc_swap = 0;
  long acc_total = 0;

  int profile_frame_count = 0;

  FILE *log_file = fopen("output_log.csv", "w");
  if (log_file) {
    fprintf(log_file,
            "Timestamp_ms,Wait_us,Upload_us,Draw_us,Swap_us,Total_us,FPS\n");
  } else {
    printf("Warning: Could not open log file!\n");
  }

  printf("Ready. Keys: 1-4 Select | c=Swap | r=Resize | 0=Reset\n");
  printf("Resize: f=Full, Arrows=Halves\n");

  while (running) {
    // --- MEASURE WAIT TIME (VSync Idle) ---
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (waiting_for_flip) {
      if (!running)
        break;
      FD_ZERO(&fds);
      FD_SET(kms.fd, &fds);
      if (select(kms.fd + 1, &fds, 0, 0, 0) > 0)
        drmHandleEvent(kms.fd, &ev);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1); // End Wait
    if (!running) {
      break;
    }

    // --- CHECK FOR GEOMETRY UPDATES ---
    if (layout_dirty) {
      update_geometry_buffer(vbo);
    }

    upload_video_frame(&videos[layout[0]], 0);
    upload_video_frame(&videos[layout[1]], 2);
    upload_video_frame(&videos[layout[2]], 4);
    upload_video_frame(&videos[layout[3]], 6);
    clock_gettime(CLOCK_MONOTONIC, &t2); // End Upload

    glDrawArrays(GL_TRIANGLES, 0, 24);
    clock_gettime(CLOCK_MONOTONIC, &t3); // End Draw

    swap_buffers();
    clock_gettime(CLOCK_MONOTONIC, &t4); // End Swap

    // --- ACCUMULATE TIMES ---
    acc_wait += get_diff_us(t0, t1);
    acc_upload += get_diff_us(t1, t2);
    acc_draw += get_diff_us(t2, t3);
    acc_swap += get_diff_us(t3, t4);
    acc_total += get_diff_us(t0, t4); // Total loop time

    // --- REPORT EVERY 60 FRAMES ---
    profile_frame_count++;
    if (profile_frame_count >= 60) {
      // Calculate Averages
      long avg_wait = acc_wait / 60;
      long avg_upload = acc_upload / 60;
      long avg_draw = acc_draw / 60;
      long avg_swap = acc_swap / 60;
      long avg_total = acc_total / 60;
      long fps_est =
          1000000 / (avg_total > 0 ? avg_total : 1); // Avoid div by zero

      // 1. Print to Console (So you can still see it live)
      printf("FPS: %ld | Total: %ld us | Wait: %ld | Upload: %ld | Draw: %ld | "
             "Swap: %ld\n",
             fps_est, avg_total, avg_wait, avg_upload, avg_draw, avg_swap);

      // 2. Write to CSV File
      if (log_file) {
        fprintf(log_file, "%lld,%ld,%ld,%ld,%ld,%ld,%ld\n",
                current_timestamp(), // Uses your existing timestamp function
                avg_wait, avg_upload, avg_draw, avg_swap, avg_total, fps_est);

        fflush(log_file); // IMPORTANT: Force write to disk immediately
      }

      // Reset accumulators
      acc_wait = acc_upload = acc_draw = acc_swap = acc_total = 0;
      profile_frame_count = 0;
    }

    // // --- FPS CALCULATION ---
    // frame_count++;
    // long long current_time = current_timestamp();
    // if (current_time - last_time >= 1000)
    // { // If 1 second has passed
    //   printf("FPS: %d\r\n", frame_count);
    //   frame_count = 0;
    //   last_time = current_time;
    // }
  }

  glDeleteProgram(p);
  cleanup();

  pthread_join(tid, NULL);
  endwin();
  delwin(stdscr);
  return 0;
}
