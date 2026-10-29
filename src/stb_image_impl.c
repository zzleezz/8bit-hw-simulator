/* stb_image_impl.c - the picture loader of --match (third_party/stb,
 * stb_image v2.30, public domain): PNG, BMP, JPG, TGA and GIF, in a file
 * of its own so its warnings stay out of the rest of the build. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#define STBI_WINDOWS_UTF8
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "stb/stb_image.h"
