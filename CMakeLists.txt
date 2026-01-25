cmake_minimum_required(VERSION 3.10)
project(video_scaling)

set(OpenGL_GL_PREFERENCE GLVND)

find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)


add_executable(video_scaling main.c utils.c gl.c)

target_link_libraries(video_scaling PRIVATE 
    glfw
    OpenGL::GL
    PkgConfig::AVCODEC
    PkgConfig::AVFORMAT
    PkgConfig::AVUTIL
    PkgConfig::SWSCALE
    PkgConfig::EGL
    PkgConfig::DRM
    PkgConfig::GBM
)
