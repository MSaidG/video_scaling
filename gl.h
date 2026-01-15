#define GLFW_INCLUDE_ES2
#include <GLFW/glfw3.h>
GLFWwindow *initGLFW(int width, int height);

GLuint compile_shader(GLenum type, const char *src);
GLuint create_program(const char *vs, const char *fs);
void framebuffer_size_callback(GLFWwindow *window, int width, int height);
