// Isolated translation unit so tinygltf / nlohmann / stb stay out of -Wpedantic
// on the rest of the frontend. Vendored from Photon .external/tinygltf v2.4.2.
// TINYGLTF_NOEXCEPTION / TINYGLTF_NO_STB_IMAGE_WRITE / JSON_NOEXCEPTION come
// from photon-tinygltf compile definitions.
#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "tiny_gltf.h"
