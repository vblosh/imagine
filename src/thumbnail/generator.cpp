#include "imagine/thumbnail/generator.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#if __has_include(<stb_image_resize2.h>)
#include <stb_image_resize2.h>
#define USE_STB_RESIZE2 1
#else
#include <stb_image_resize.h>
#define USE_STB_RESIZE2 0
#endif

namespace imagine::thumbnail {

Result<std::pair<int, int>> Generator::getImageDimensions(const std::string& filePath) {
    int w = 0, h = 0, comp = 0;
    if (stbi_info(filePath.c_str(), &w, &h, &comp)) {
        return std::make_pair(w, h);
    }
    return Status::parseError("Failed to read image info: " + filePath);
}

Result<std::pair<int, int>> Generator::getImageDimensionsFromMemory(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return Status::invalidArgument("Empty memory buffer for image info");
    }
    int w = 0, h = 0, comp = 0;
    if (stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &comp)) {
        return std::make_pair(w, h);
    }
    return Status::parseError("Failed to read image info from memory");
}

Result<ImageBuffer> Generator::loadImage(const std::string& filePath) {
    int w = 0, h = 0, channels = 0;
    // Force 3 channels (RGB) for consistent thumbnailing
    unsigned char* pixels = stbi_load(filePath.c_str(), &w, &h, &channels, 3);
    if (!pixels) {
        return Status::ioError("Failed to load image: " + filePath + " (" + stbi_failure_reason() + ")");
    }

    ImageBuffer buf;
    buf.width = w;
    buf.height = h;
    buf.channels = 3;
    buf.data.assign(pixels, pixels + (w * h * 3));
    stbi_image_free(pixels);

    return buf;
}

Result<ImageBuffer> Generator::loadImageFromMemory(const uint8_t* data, size_t size) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 3);
    if (!pixels) {
        return Status::parseError("Failed to decode image from memory: " + std::string(stbi_failure_reason()));
    }

    ImageBuffer buf;
    buf.width = w;
    buf.height = h;
    buf.channels = 3;
    buf.data.assign(pixels, pixels + (w * h * 3));
    stbi_image_free(pixels);

    return buf;
}

ImageBuffer Generator::rotate(const ImageBuffer& src, int orientation) {
    if (orientation <= 1 || orientation > 8 || src.data.empty()) {
        return src;
    }

    int inW = src.width;
    int inH = src.height;
    int c = src.channels;

    ImageBuffer dst;
    dst.channels = c;

    if (orientation == 3) {
        // 180 degrees
        dst.width = inW;
        dst.height = inH;
        dst.data.resize(inW * inH * c);
        for (int y = 0; y < inH; ++y) {
            for (int x = 0; x < inW; ++x) {
                int srcIdx = (y * inW + x) * c;
                int dstIdx = ((inH - 1 - y) * inW + (inW - 1 - x)) * c;
                for (int ch = 0; ch < c; ++ch) {
                    dst.data[dstIdx + ch] = src.data[srcIdx + ch];
                }
            }
        }
        return dst;
    } else if (orientation == 6) {
        // 90 degrees CW
        dst.width = inH;
        dst.height = inW;
        dst.data.resize(inW * inH * c);
        for (int y = 0; y < inH; ++y) {
            for (int x = 0; x < inW; ++x) {
                int srcIdx = (y * inW + x) * c;
                int dstIdx = (x * inH + (inH - 1 - y)) * c;
                for (int ch = 0; ch < c; ++ch) {
                    dst.data[dstIdx + ch] = src.data[srcIdx + ch];
                }
            }
        }
        return dst;
    } else if (orientation == 8) {
        // 270 degrees CW (90 CCW)
        dst.width = inH;
        dst.height = inW;
        dst.data.resize(inW * inH * c);
        for (int y = 0; y < inH; ++y) {
            for (int x = 0; x < inW; ++x) {
                int srcIdx = (y * inW + x) * c;
                int dstIdx = ((inW - 1 - x) * inH + y) * c;
                for (int ch = 0; ch < c; ++ch) {
                    dst.data[dstIdx + ch] = src.data[srcIdx + ch];
                }
            }
        }
        return dst;
    }

    return src;
}

Result<ImageBuffer> Generator::resize(const ImageBuffer& src, int maxDimension) {
    if (src.data.empty() || src.width <= 0 || src.height <= 0) {
        return Status::invalidArgument("Empty source image for resizing");
    }

    int inW = src.width;
    int inH = src.height;

    int outW = inW;
    int outH = inH;

    if (inW > maxDimension || inH > maxDimension) {
        if (inW >= inH) {
            outW = maxDimension;
            outH = std::max(1, static_cast<int>(std::round(static_cast<double>(inH * maxDimension) / inW)));
        } else {
            outH = maxDimension;
            outW = std::max(1, static_cast<int>(std::round(static_cast<double>(inW * maxDimension) / inH)));
        }
    } else {
        // No need to upscale
        return src;
    }

    ImageBuffer dst;
    dst.width = outW;
    dst.height = outH;
    dst.channels = src.channels;
    dst.data.resize(outW * outH * src.channels);

#if USE_STB_RESIZE2
    stbir_resize_uint8_linear(
        src.data.data(), inW, inH, 0,
        dst.data.data(), outW, outH, 0,
        static_cast<stbir_pixel_layout>(src.channels)
    );
#else
    stbir_resize_uint8(
        src.data.data(), inW, inH, 0,
        dst.data.data(), outW, outH, 0,
        src.channels
    );
#endif

    return dst;
}

Status Generator::saveJpeg(const ImageBuffer& img, const std::string& destPath, int quality) {
    std::filesystem::path p(destPath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    int rc = stbi_write_jpg(destPath.c_str(), img.width, img.height, img.channels, img.data.data(), quality);
    if (!rc) {
        return Status::ioError("Failed to write JPEG thumbnail to " + destPath);
    }
    return Status::ok();
}

Status Generator::savePng(const ImageBuffer& img, const std::string& destPath) {
    std::filesystem::path p(destPath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    int stride = img.width * img.channels;
    int rc = stbi_write_png(destPath.c_str(), img.width, img.height, img.channels, img.data.data(), stride);
    if (!rc) {
        return Status::ioError("Failed to write PNG thumbnail to " + destPath);
    }
    return Status::ok();
}

} // namespace imagine::thumbnail
