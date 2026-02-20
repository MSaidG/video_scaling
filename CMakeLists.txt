cmake_minimum_required(VERSION 3.10)
project(video_scaling)

set(OpenGL_GL_PREFERENCE GLVND)

find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)

# Check for required packages
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(GBM REQUIRED gbm)

# GStreamer with more components
pkg_check_modules(GSTREAMER REQUIRED 
    gstreamer-1.0 
    gstreamer-app-1.0
    gstreamer-video-1.0
    gstreamer-allocators-1.0
    gstreamer-gl-1.0  # For OpenGL integration
)

# Also check for specific DRM headers
find_path(DRM_FOURCC_INCLUDE_DIR
    NAMES drm_fourcc.h
    PATHS /usr/include/libdrm /usr/local/include/libdrm
)

if(NOT DRM_FOURCC_INCLUDE_DIR)
    message(FATAL_ERROR "drm_fourcc.h not found. Please install libdrm development package.")
endif()

add_executable(video_scaling main.c)

# Add include directories
target_include_directories(video_scaling PRIVATE
    ${EGL_INCLUDE_DIRS}
    ${DRM_INCLUDE_DIRS}
    ${GBM_INCLUDE_DIRS}
    ${GSTREAMER_INCLUDE_DIRS}
    ${DRM_FOURCC_INCLUDE_DIR}  # Explicitly add drm_fourcc.h location
)

# Add compile definitions and flags
target_compile_options(video_scaling PRIVATE
    ${EGL_CFLAGS_OTHER}
    ${DRM_CFLAGS_OTHER}
    ${GBM_CFLAGS_OTHER}
    ${GSTREAMER_CFLAGS_OTHER}
)

# Link libraries
target_link_libraries(video_scaling PRIVATE
    glfw
    OpenGL::GL
    ${EGL_LIBRARIES}
    ${DRM_LIBRARIES}
    ${GBM_LIBRARIES}
    ${GSTREAMER_LIBRARIES}
    m
    dl  # Often needed for dynamic linking
    rt  # Real-time library, sometimes needed
)