#include <GLES2/gl2.h>

GLuint compile_shader(GLenum type, const char *src);
GLuint create_program(const char *vs, const char *fs);
