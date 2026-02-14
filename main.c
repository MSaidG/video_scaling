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
#include <drm/drm_fourcc.h>

// --- CONFIG ---
#define VIDEO_COUNT 4
#define RAW_FILE_1 "videos/smpte_nv12_1080p60.yuv"
#define RAW_FILE_2 "videos/smpte_nv12_480p30.yuv"
#define RAW_FILE_3 "videos/smpte_nv12_720p30.yuv"
#define RAW_FILE_4 "videos/smpte_nv12_600x600p30.yuv"

#define VID_W 1920
#define VID_H 1080
// #define FPS 60

// Extension Function Pointers
PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

// --- GLOBALS ---
struct
{
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

typedef struct
{
  const char *filename;
  int fd;
  unsigned char *file_data; // Memory mapped file
  size_t size;
  size_t frame_size;
  int total_frames;
  int curr_frame_idx;
  int width;
  int height;

  // EGL/GBM Resources for Zero-Copy
  struct gbm_bo *bo; // The GPU Buffer
  void *bo_map_data; // CPU pointer to write to BO
  uint32_t bo_stride;
  EGLImageKHR egl_img; // The EGL Image handle
  GLuint tex;          // The GL Texture wrapping the EGL Image
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
typedef struct
{
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
long long current_timestamp()
{
  struct timeval te;
  gettimeofday(&te, NULL);
  return te.tv_sec * 1000LL + te.tv_usec / 1000;
}

// Check Extensions
int has_extension(const char *extensions, const char *ext)
{
  size_t ext_len = strlen(ext);
  const char *end = extensions + strlen(extensions);
  const char *p = extensions;
  while (p < end)
  {
    size_t n = strcspn(p, " ");
    if (n == ext_len && strncmp(ext, p, n) == 0)
      return 1;
    p += n + 1;
  }
  return 0;
}

// Helper: Check if two rects overlap
int rects_overlap(Rect r1, Rect r2)
{
  if (r1.w == 0 || r1.h == 0 || r2.w == 0 || r2.h == 0)
    return 0; // Ignore hidden
  return r1.x < r2.x + r2.w && r1.x + r1.w > r2.x && r1.y < r2.y + r2.h &&
         r1.y + r1.h > r2.y;
}

// Helper: Reset grid to default
void reset_layout()
{
  for (int i = 0; i < 4; i++)
    slot_rects[i] = default_rects[i];
  layout_dirty = 1;
}

// Shader Sources (Simplified for Single Texture per Video if using R8/GR88)
// NOTE: For true NV12 import, we usually import as external_oes or handle planes.
// To keep it simple but fast, we will treat the buffer as a single R8 texture
// with the height * 1.5. This allows "fake" NV12 reading in shader.
const char *vs_src = "attribute vec4 pos;\n"
                     "attribute vec2 tex;\n"
                     "attribute float a_vid;\n"
                     "varying vec2 v_tex;\n"
                     "varying float v_vid;\n"
                     "void main() { gl_Position = pos; v_tex = tex; v_vid = a_vid; }";

const char *fs_src =
    "precision mediump float;\n"
    "varying vec2 v_tex;\n"
    "varying float v_vid;\n"
    "uniform sampler2D tex0; uniform sampler2D tex1;\n"
    "uniform sampler2D tex2; uniform sampler2D tex3;\n"

    // Helper to read NV12 from a single R8 texture
    // The texture height is 1.5x larger than visual height.
    "vec3 read_nv12(sampler2D t, vec2 uv) {\n"
    "   float y = texture2D(t, vec2(uv.x, uv.y * 0.66666)).r;\n"   // Top 2/3 is Y
    "   vec2 uv_coord = vec2(uv.x, 0.66666 + (uv.y * 0.33333));\n" // Bottom 1/3 is UV
    "   // We need to read UV. This depends on implementation details of R8 vs RG88.\n"
    "   // A simpler hack for raw speed test: Just return grayscale Y\n"
    "   return vec3(y, y, y);\n"
    "}\n"

    "void main() {\n"
    "  vec3 color;\n"
    "  if (v_vid < 0.5) color = read_nv12(tex0, v_tex);\n"
    "  else if (v_vid < 1.5) color = read_nv12(tex1, v_tex);\n"
    "  else if (v_vid < 2.5) color = read_nv12(tex2, v_tex);\n"
    "  else color = read_nv12(tex3, v_tex);\n"
    "  gl_FragColor = vec4(color, 1.0);\n"
    "}";

// --- GEOMETRY UPDATE FUNCTION ---
// Called by Main Thread when layout_dirty is true
void update_geometry_buffer(GLuint vbo)
{
  // 4 quads * 6 verts/quad * 5 floats/vert (x,y, u,v, id)
  GLfloat verts[4 * 6 * 5];
  int idx = 0;

  for (int i = 0; i < 4; i++)
  {
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

int init_kms()
{
  kms.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    kms.fd = open("/dev/dri/card1", O_RDWR | O_CLOEXEC);
  if (kms.fd < 0)
    return -1;

  drmModeRes *res = drmModeGetResources(kms.fd);
  if (!res)
    return -1;

  // Find a connected connector
  for (int i = 0; i < res->count_connectors; i++)
  {
    drmModeConnector *c = drmModeGetConnector(kms.fd, res->connectors[i]);
    if (c->connection == DRM_MODE_CONNECTED)
    {
      kms.conn = c;
      break;
    }
    drmModeFreeConnector(c);
  }
  drmModeFreeResources(res);

  if (!kms.conn)
  {
    fprintf(stderr, "No monitor found\n");
    return -1;
  }
  kms.mode = kms.conn->modes[0];

  // Find Encoder & CRTC
  drmModeEncoder *enc = NULL;
  if (kms.conn->encoder_id)
  {
    enc = drmModeGetEncoder(kms.fd, kms.conn->encoder_id);
  }

  if (enc && enc->crtc_id)
  {
    kms.crtc = drmModeGetCrtc(kms.fd, enc->crtc_id);
  }
  else
  {
    // Re-fetch resources just for CRTC fallback (rare case)
    res = drmModeGetResources(kms.fd);
    if (res && res->count_crtcs > 0)
    {
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
  for (int i = 0; i < num_configs; i++)
  {
    EGLint id;
    eglGetConfigAttrib(kms.egl_disp, configs[i], EGL_NATIVE_VISUAL_ID, &id);
    if (id == gbm_format)
    {
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

// Init Video Source using GBM BO (Zero Copy)
int init_video_source(VideoSource *v, const char *filename, int width, int height)
{
  v->filename = filename;
  v->fd = open(filename, O_RDONLY);
  if (v->fd < 0)
    return -1;
  struct stat sb;
  fstat(v->fd, &sb);
  v->size = sb.st_size;
  v->frame_size = width * height * 3 / 2;
  v->total_frames = v->size / v->frame_size;
  v->curr_frame_idx = 0;
  v->width = width;
  v->height = height;

  // 1. Map File for reading (CPU side)
  v->file_data = mmap(NULL, v->size, PROT_READ, MAP_PRIVATE, v->fd, 0);
  if (v->file_data == MAP_FAILED)
    return -1;

  // 2. Allocate GBM BO for GPU usage
  // We allocate a buffer big enough for NV12 (Height * 1.5) using R8 format
  v->bo = gbm_bo_create(kms.gbm_dev, width, height * 3 / 2, GBM_FORMAT_R8, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
  if (!v->bo)
  {
    fprintf(stderr, "Failed to create GBM BO\n");
    return -1;
  }

  // 3. Create EGLImage from GBM BO
  // Get DMA BUF FD (not strictly needed for internal create, but good practice)
  int dma_fd = gbm_bo_get_fd(v->bo);
  int stride = gbm_bo_get_stride(v->bo);
  v->bo_stride = stride;

  EGLint attribs[] = {
      EGL_WIDTH, width,
      EGL_HEIGHT, height * 3 / 2,
      EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
      EGL_DMA_BUF_PLANE0_FD_EXT, dma_fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
      EGL_NONE};

  v->egl_img = eglCreateImageKHR(kms.egl_disp, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL, attribs);
  if (v->egl_img == EGL_NO_IMAGE_KHR)
  {
    // Fallback: Try creating from GBM handle directly if DMA_BUF fails
    v->egl_img = eglCreateImageKHR(kms.egl_disp, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, (EGLClientBuffer)v->bo, NULL);
  }

  if (v->egl_img == EGL_NO_IMAGE_KHR)
  {
    fprintf(stderr, "Failed to create EGLImage\n");
    return -1;
  }

  // 4. Create GL Texture from EGLImage
  glGenTextures(1, &v->tex);
  glBindTexture(GL_TEXTURE_2D, v->tex);
  glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, v->egl_img);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

  return 0;
}

// The Critical "Zero-Copy" Update
void update_video_content(VideoSource *v)
{
  // We map the GBM BO to CPU space to write new data
  // NOTE: gbm_bo_map is not always available/performant on all drivers.
  // Ideally, you would have the video decoder write directly to dma_fd.
  // Here we simulate it by memcpying to the mapped BO.

  uint32_t stride;
  void *map_data;
  void *map_handle;

  // Map the GPU buffer
  map_data = gbm_bo_map(v->bo, 0, 0, v->width, v->height * 3 / 2, GBM_BO_TRANSFER_WRITE, &stride, &map_handle);
  if (!map_data)
    return;

  // Get pointer to current frame in file
  unsigned char *src = v->file_data + (v->curr_frame_idx * v->frame_size);

  // Copy! (This is still a copy, BUT it is into Uncached Write-Combined memory usually, which is faster than glTexImage2D driver overhead)
  memcpy(map_data, src, v->frame_size);

  // Unmap (Flushes caches if needed)
  gbm_bo_unmap(v->bo, map_handle);

  v->curr_frame_idx = (v->curr_frame_idx + 1) % v->total_frames;
}

// Helper function to load pointers
void load_egl_extensions()
{
  eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
  eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
  glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

  if (!eglCreateImageKHR || !glEGLImageTargetTexture2DOES)
  {
    fprintf(stderr, "FATAL: EGLImage extensions missing.\n");
    exit(1);
  }
}

static void page_flip_handler(int fd, unsigned int frame, unsigned int sec,
                              unsigned int usec, void *data)
{
  *(int *)data = 0;
}

void swap_buffers()
{
  eglSwapBuffers(kms.egl_disp, kms.egl_surf);
  struct gbm_bo *bo = gbm_surface_lock_front_buffer(kms.gbm_surf);
  uint32_t handle = gbm_bo_get_handle(bo).u32;
  uint32_t fb;
  drmModeAddFB(kms.fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), 24, 32,
               gbm_bo_get_stride(bo), handle, &fb);

  drmModePageFlip(kms.fd, kms.crtc->crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT,
                  &waiting_for_flip);
  waiting_for_flip = 1;

  if (kms.curr_bo)
  {
    gbm_surface_release_buffer(kms.gbm_surf, kms.curr_bo);
    drmModeRmFB(kms.fd, kms.curr_fb);
  }
  kms.curr_bo = bo;
  kms.curr_fb = fb;
}

void cleanup()
{
  printf("Cleaning up resources...\n");

  for (int i = 0; i < VIDEO_COUNT; ++i)
  {
    // 1. Clean up Camera/Input Resources
    if (videos[i].file_data && videos[i].file_data != MAP_FAILED)
    {
      munmap(videos[i].file_data, videos[i].size);
    }
    if (videos[i].fd >= 0)
    {
      close(videos[i].fd);
    }

    // Clean up textures (if GL context is still alive)
    if (videos[i].tex)
      glDeleteTextures(1, &videos[i].tex);
  }

  // 2. Clean up KMS/GBM Resources (Current Frame)
  if (kms.curr_bo)
  {
    gbm_surface_release_buffer(kms.gbm_surf, kms.curr_bo);
    drmModeRmFB(kms.fd, kms.curr_fb);
    kms.curr_bo = NULL;
  }

  // 3. Clean up EGL
  if (kms.egl_disp != EGL_NO_DISPLAY)
  {
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

  if (kms.fd >= 0)
  {
    close(kms.fd);
  }

  printf("Cleanup Done.\n");
}

void exit_program() { running = false; }

void apply_resize(int slot, int key)
{
  // 1. First, set default layout positions so we know where everyone starts.
  reset_layout();

  int filler = -1; // Who will fill the empty space?

  if (slot == 0)
  { // TL
    if (key == KEY_UP)
    {
      slot_rects[0] = RECT_TOP;
    } // Safe (Covered)
    if (key == KEY_LEFT)
    {
      slot_rects[0] = RECT_LEFT;
    } // Safe (Covered)
    if (key == KEY_DOWN)
    {
      slot_rects[0] = RECT_BOTTOM;
      filler = 1;
      slot_rects[filler] = RECT_TOP;
    }
    if (key == KEY_RIGHT)
    {
      slot_rects[0] = RECT_RIGHT;
      filler = 2;
      slot_rects[filler] = RECT_LEFT;
    }
  }
  else if (slot == 1)
  { // TR
    if (key == KEY_UP)
    {
      slot_rects[1] = RECT_TOP;
    } // Safe
    if (key == KEY_RIGHT)
    {
      slot_rects[1] = RECT_RIGHT;
    } // Safe
    if (key == KEY_DOWN)
    {
      slot_rects[1] = RECT_BOTTOM;
      filler = 0;
      slot_rects[filler] = RECT_TOP;
    }
    if (key == KEY_LEFT)
    {
      slot_rects[1] = RECT_LEFT;
      filler = 3;
      slot_rects[filler] = RECT_RIGHT;
    }
  }
  else if (slot == 2)
  { // BL
    if (key == KEY_DOWN)
    {
      slot_rects[2] = RECT_BOTTOM;
    } // Safe
    if (key == KEY_LEFT)
    {
      slot_rects[2] = RECT_LEFT;
    } // Safe
    if (key == KEY_UP)
    {
      slot_rects[2] = RECT_TOP;
      filler = 3;
      slot_rects[filler] = RECT_BOTTOM;
    }
    if (key == KEY_RIGHT)
    {
      slot_rects[2] = RECT_RIGHT;
      filler = 0;
      slot_rects[filler] = RECT_LEFT;
    }
  }
  else if (slot == 3)
  { // BR
    if (key == KEY_DOWN)
    {
      slot_rects[3] = RECT_BOTTOM;
    } // Safe
    if (key == KEY_RIGHT)
    {
      slot_rects[3] = RECT_RIGHT;
    } // Safe
    if (key == KEY_UP)
    {
      slot_rects[3] = RECT_TOP;
      filler = 2;
      slot_rects[filler] = RECT_BOTTOM;
    }
    if (key == KEY_LEFT)
    {
      slot_rects[3] = RECT_LEFT;
      filler = 1;
      slot_rects[filler] = RECT_RIGHT;
    }
  }

  // 2. Hide Loop: Clean up overlaps
  // Any video that overlaps the 'Selected' or the 'Filler' must be hidden.
  for (int i = 0; i < 4; i++)
  {
    if (i == slot)
      continue; // Don't check self
    if (filler != -1 && i == filler)
      continue; // Don't check filler

    // Check against selected
    if (rects_overlap(slot_rects[slot], slot_rects[i]))
    {
      slot_rects[i].w = 0;
      slot_rects[i].h = 0;
    }
    // Check against filler (if it exists)
    if (filler != -1 && rects_overlap(slot_rects[filler], slot_rects[i]))
    {
      slot_rects[i].w = 0;
      slot_rects[i].h = 0;
    }
  }

  layout_dirty = 1;
}

void *input_thread(void *arg)
{
  initscr();
  cbreak();
  noecho();
  nodelay(stdscr, TRUE);
  keypad(stdscr, TRUE);

  while (running)
  {
    int ch = getch();
    if (ch != ERR)
    {
      if (ch == 'q')
        exit_program();

      int pressed_slot = -1;
      if (ch >= '1' && ch <= '4')
        pressed_slot = ch - '1';

      if (ch == '0')
      {
        reset_layout();
        in_resize_mode = 0;
        in_change_mode = 0;
        selected_slot = -1;
      }
      else if (in_resize_mode)
      {
        if (pressed_slot != -1)
        {
          selected_slot = pressed_slot;
        }
        else if (ch == 'f')
        {
          if (selected_slot != -1)
          {
            // Fullscreen is safe (hide all others)
            for (int i = 0; i < 4; i++)
            {
              slot_rects[i].w = 0;
              slot_rects[i].h = 0;
            }
            slot_rects[selected_slot] = RECT_FULL;
            layout_dirty = 1;
          }
        }
        else if (ch == KEY_LEFT || ch == KEY_RIGHT || ch == KEY_UP ||
                 ch == KEY_DOWN)
        {
          if (selected_slot != -1)
          {
            apply_resize(selected_slot, ch);
          }
        }
        else if (ch == 'r')
        {
          in_resize_mode = 0;
        }
      }
      else if (in_change_mode)
      {
        if (pressed_slot != -1)
        {
          int src = selected_slot;
          int dst = pressed_slot;
          if (src != dst)
          {
            int tmp = layout[src];
            layout[src] = layout[dst];
            layout[dst] = tmp;
          }
          in_change_mode = 0;
          selected_slot = -1;
        }
        else
        {
          in_change_mode = 0;
          selected_slot = -1;
        }
      }
      else
      {
        if (pressed_slot != -1)
        {
          selected_slot = pressed_slot;
        }
        else if (ch == 'c')
        {
          if (selected_slot != -1)
            in_change_mode = 1;
        }
        else if (ch == 'r')
        {
          if (selected_slot != -1)
            in_resize_mode = 1;
        }
        else
        {
          selected_slot = -1;
        }
      }
    }
    usleep(10000);
  }
  return NULL;
}

int main()
{
  signal(SIGINT, handle_signal);
  if (init_kms() != 0)
    return 1;

  // Check extension
  const char *exts = eglQueryString(kms.egl_disp, EGL_EXTENSIONS);
  if (!has_extension(exts, "EGL_EXT_image_dma_buf_import"))
  {
    printf("WARNING: EGL_EXT_image_dma_buf_import not found. Trying fallback...\n");
  }
  load_egl_extensions();

  // Init Videos
  if (init_video_source(&videos[0], RAW_FILE_1, 1920, 1080) != 0)
    return 1;
  if (init_video_source(&videos[1], RAW_FILE_2, 640, 480) != 0)
    return 1;
  if (init_video_source(&videos[2], RAW_FILE_3, 1280, 720) != 0)
    return 1;
  if (init_video_source(&videos[3], RAW_FILE_4, 600, 600) != 0)
    return 1;

  // Compile Shaders (New Simple Shader)
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

  glUniform1i(glGetUniformLocation(p, "tex0"), 0);
  glUniform1i(glGetUniformLocation(p, "tex1"), 1);
  glUniform1i(glGetUniformLocation(p, "tex2"), 2);
  glUniform1i(glGetUniformLocation(p, "tex3"), 3);

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

  printf("Ready. Keys: 1-4 Select | c=Swap | r=Resize | 0=Reset\n");
  printf("Resize: f=Full, Arrows=Halves\n");

  while (running)
  {
    while (waiting_for_flip)
    {
      if (!running)
        break;
      FD_ZERO(&fds);
      FD_SET(kms.fd, &fds);
      if (select(kms.fd + 1, &fds, 0, 0, 0) > 0)
        drmHandleEvent(kms.fd, &ev);
    }
    if (!running)
      break;

    update_video_content(&videos[0]);
    update_video_content(&videos[1]);
    update_video_content(&videos[2]);
    update_video_content(&videos[3]);

    // Draw
    if (layout_dirty)
      update_geometry_buffer(vbo);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, videos[layout[0]].tex); // Was videos[0].tex

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, videos[layout[1]].tex); // Was videos[1].tex

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, videos[layout[2]].tex); // Was videos[2].tex

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, videos[layout[3]].tex); // Was videos[3].tex

    glDrawArrays(GL_TRIANGLES, 0, 24);
    swap_buffers(); // Using the updated swap_buffers from previous turn

    // --- FPS CALCULATION ---
    frame_count++;
    long long current_time = current_timestamp();
    if (current_time - last_time >= 1000)
    { // If 1 second has passed
      printf("FPS: %d\r\n", frame_count);
      frame_count = 0;
      last_time = current_time;
    }
  }

  glDeleteProgram(p);
  cleanup();

  pthread_join(tid, NULL);
  endwin();
  delwin(stdscr);
  return 0;
}
