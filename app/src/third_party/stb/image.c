// stb_image implementation: JPEG, PNG and GIF from memory only.
// Vendored code, built as published (tools/build.sh adds -w for third_party).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF // animated stickers (ps5-discord)
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"
