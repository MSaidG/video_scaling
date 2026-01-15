#include "utils.h"
#include <stdio.h>
#include <stdlib.h>


char *read_file(const char *filename) {
  FILE *f = fopen(filename, "rb");
  if (!f) {
    fprintf(stderr, "Could not open shader file: %s\n", filename);
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  long length = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buffer = malloc(length + 1);
  fread(buffer, 1, length, f);
  fclose(f);
  buffer[length] = '\0';
  return buffer;
}
