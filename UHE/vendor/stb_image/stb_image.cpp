#include "uhepch.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include "stb_image.h"

// Writer for RHIDevice::RequestFrameCapture output (golden-image tests).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
