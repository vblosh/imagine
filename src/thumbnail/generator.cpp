#include "imagine/thumbnail/generator.hpp"
#include "imagine/common/types.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#if IMAGINE_HAS_TURBOJPEG
#include <turbojpeg.h>
#endif

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
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file) {
        return Status::parseError("Failed to read image info: " + filePath);
    }
    stbi_io_callbacks callbacks;
    callbacks.read = [](void* user, char* data, int size) -> int {
        auto* stream = static_cast<std::istream*>(user);
        stream->read(data, size);
        return static_cast<int>(stream->gcount());
    };
    callbacks.skip = [](void* user, int n) {
        auto* stream = static_cast<std::istream*>(user);
        stream->clear();
        stream->seekg(n, std::ios::cur);
    };
    callbacks.eof = [](void* user) -> int {
        auto* stream = static_cast<std::istream*>(user);
        return stream->eof() ? 1 : 0;
    };
    int w = 0, h = 0, comp = 0;
    if (stbi_info_from_callbacks(&callbacks, &file, &w, &h, &comp)) {
        return std::make_pair(w, h);
    }
    return Status::parseError("Failed to read image info: " + filePath);
}

bool Generator::isJpeg(const uint8_t* data, size_t size) {
    return (data != nullptr && size >= 2 && data[0] == 0xFF && data[1] == 0xD8);
}

Result<std::pair<int, int>> Generator::getImageDimensionsFromMemory(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return Status::invalidArgument("Empty memory buffer for image info");
    }

#if IMAGINE_HAS_TURBOJPEG
    if (isJpeg(data, size)) {
        tjhandle h = tjInitDecompress();
        if (h) {
            int w = 0, h_dim = 0, subsamp = 0, cs = 0;
            int r = tjDecompressHeader3(h, data, size, &w, &h_dim, &subsamp, &cs);
            tjDestroy(h);
            if (r == 0) {
                return std::make_pair(w, h_dim);
            }
        }
    }
#endif

    int w = 0, h = 0, comp = 0;
    if (stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &comp)) {
        return std::make_pair(w, h);
    }
    return Status::parseError("Failed to read image info from memory");
}

Result<ImageBuffer> Generator::loadImage(const std::string& filePath) {
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file) {
        return Status::ioError("Failed to open image file: " + filePath);
    }
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return loadImageFromMemory(buffer.data(), buffer.size());
}

Result<ImageBuffer> Generator::loadImageFromMemory(const uint8_t* data, size_t size) {
    if (!data || size == 0) {
        return Status::invalidArgument("Empty memory buffer for image decode");
    }

#if IMAGINE_HAS_TURBOJPEG
    if (isJpeg(data, size)) {
        auto tjRes = loadImageFromMemoryTurboJpeg(data, size, 1);
        if (tjRes.isOk()) {
            return tjRes;
        }
    }
#endif

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

Result<ImageBuffer> Generator::loadImageFromMemoryTurboJpeg(
    const uint8_t* data,
    size_t size,
    int scaleDenom,
    int* outOrigW,
    int* outOrigH
) {
#if IMAGINE_HAS_TURBOJPEG
    if (!data || size < 4) {
        return Status::invalidArgument("Invalid memory buffer for TurboJPEG decode");
    }

    tjhandle handle = tjInitDecompress();
    if (!handle) {
        return Status::internal("Failed to initialize TurboJPEG decompressor");
    }

    struct HandleGuard {
        tjhandle h;
        ~HandleGuard() { if (h) tjDestroy(h); }
    } guard{handle};

    int origW = 0, origH = 0, subsamp = 0, cs = 0;
    if (tjDecompressHeader3(handle, data, size, &origW, &origH, &subsamp, &cs) != 0) {
        return Status::parseError("TurboJPEG header parse failed: " + std::string(tjGetErrorStr()));
    }

    if (outOrigW) *outOrigW = origW;
    if (outOrigH) *outOrigH = origH;

    if (scaleDenom <= 0) {
        scaleDenom = (origW >= 2048 || origH >= 2048) ? 2 : 1;
    }
    int decW = (origW + scaleDenom - 1) / scaleDenom;
    int decH = (origH + scaleDenom - 1) / scaleDenom;

    ImageBuffer buf;
    buf.width = decW;
    buf.height = decH;
    buf.channels = 3;
    buf.data.resize(static_cast<size_t>(decW) * decH * 3);

    int flags = TJFLAG_FASTDCT;
    if (tjDecompress2(handle, data, size, buf.data.data(), decW, 0, decH, TJPF_RGB, flags) != 0) {
        return Status::parseError("TurboJPEG decompress failed: " + std::string(tjGetErrorStr()));
    }

    return buf;
#else
    auto res = loadImageFromMemory(data, size);
    if (!res.isOk()) return res;
    if (outOrigW) *outOrigW = res.value().width;
    if (outOrigH) *outOrigH = res.value().height;
    return res;
#endif
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
    std::filesystem::path p = pathFromUtf8(destPath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::ofstream ofs(p, std::ios::binary);
    if (!ofs) {
        return Status::ioError("Failed to open output file: " + destPath);
    }

    auto writeFunc = [](void* context, void* data, int size) {
        auto* stream = static_cast<std::ostream*>(context);
        stream->write(reinterpret_cast<const char*>(data), size);
    };

    int rc = stbi_write_jpg_to_func(writeFunc, &ofs, img.width, img.height, img.channels, img.data.data(), quality);
    if (!rc || !ofs) {
        return Status::ioError("Failed to write JPEG thumbnail to " + destPath);
    }
    return Status::ok();
}

Status Generator::saveJpegFast(const ImageBuffer& img, const std::string& destPath, int quality) {
    std::filesystem::path p = pathFromUtf8(destPath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

#if IMAGINE_HAS_TURBOJPEG
    if (img.channels == 3 && !img.data.empty() && img.width > 0 && img.height > 0) {
        tjhandle handle = tjInitCompress();
        if (handle) {
            struct HandleGuard {
                tjhandle h;
                ~HandleGuard() { if (h) tjDestroy(h); }
            } guard{handle};

            unsigned char* jpegBuf = nullptr;
            unsigned long jpegSize = 0;
            int flags = TJFLAG_FASTDCT;

            if (tjCompress2(handle, img.data.data(), img.width, 0, img.height, TJPF_RGB,
                            &jpegBuf, &jpegSize, TJSAMP_420, quality, flags) == 0) {
                std::ofstream ofs(p, std::ios::binary);
                if (ofs) {
                    ofs.write(reinterpret_cast<const char*>(jpegBuf), static_cast<std::streamsize>(jpegSize));
                    tjFree(jpegBuf);
                    if (ofs) {
                        return Status::ok();
                    }
                } else {
                    tjFree(jpegBuf);
                }
            }
        }
    }
#endif

    return saveJpeg(img, destPath, quality);
}

Status Generator::savePng(const ImageBuffer& img, const std::string& destPath) {
    std::filesystem::path p = pathFromUtf8(destPath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::ofstream ofs(p, std::ios::binary);
    if (!ofs) {
        return Status::ioError("Failed to open output file: " + destPath);
    }

    auto writeFunc = [](void* context, void* data, int size) {
        auto* stream = static_cast<std::ostream*>(context);
        stream->write(reinterpret_cast<const char*>(data), size);
    };

    int stride = img.width * img.channels;
    int rc = stbi_write_png_to_func(writeFunc, &ofs, img.width, img.height, img.channels, img.data.data(), stride);
    if (!rc || !ofs) {
        return Status::ioError("Failed to write PNG thumbnail to " + destPath);
    }
    return Status::ok();
}

} // namespace imagine::thumbnail
