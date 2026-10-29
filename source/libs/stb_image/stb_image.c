// Theme art is PNG only (screen.c); the other decoders stay out of the binary.
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION

// v2.30 leaves two overflow helpers unused when only PNG is built.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop
