cmake_minimum_required(VERSION 3.10)
project(video_scaling)

set(OpenGL_GL_PREFERENCE GLVND)

find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)

pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)


add_executable(video_scaling main.c)

target_link_libraries(video_scaling PRIVATE 
    glfw
    OpenGL::GL
    PkgConfig::EGL
    PkgConfig::DRM
    PkgConfig::GBM
    m
)
