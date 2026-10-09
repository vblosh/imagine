#include "imagine/thumbnail/generator.hpp"
#include "imagine/common/types.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <atomic>
#include <thread>
#include <limits>

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

namespace {

std::filesystem::path makeTempPath(const std::filesystem::path& destPath) {
    static std::atomic<uint64_t> counter{0};
    auto name = destPath.filename().string();
    auto tmpName = name + ".tmp." +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
        "." + std::to_string(++counter);
    if (destPath.has_parent_path()) {
        return destPath.parent_path() / pathFromUtf8(tmpName);
    }
    return pathFromUtf8(tmpName);
}

Status commitFile(const std::filesystem::path& tmpPath, const std::filesystem::path& destPath) {
    std::error_code ec;
    std::filesystem::rename(tmpPath, destPath, ec);
    if (ec) {
        // On Windows std::filesystem::rename can fail if destination already exists
        std::error_code ecRem;
        std::filesystem::remove(destPath, ecRem);
        ec.clear();
        std::filesystem::rename(tmpPath, destPath, ec);
    }
    if (ec) {
        std::error_code ecClean;
        std::filesystem::remove(tmpPath, ecClean);
        return Status::ioError("Failed to commit file to " + pathToUtf8(destPath) + ": " + ec.message());
    }
    return Status::ok();
}

} // namespace

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
            int r = tjDecompressHeader3(h, data, static_cast<unsigned long>(size), &w, &h_dim, &subsamp, &cs);
            tjDestroy(h);
            if (r == 0) {
                return std::make_pair(w, h_dim);
            }
        }
    }
#endif

    if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return Status::invalidArgument("Memory buffer too large for image info");
    }

    int w = 0, h = 0, comp = 0;
    if (stbi_info_from_memory(data, static_cast<int>(size), &w, &h, &comp)) {
        return std::make_pair(w, h);
    }
    return Status::parseError("Failed to read image info from memory");
}

Result<ImageBuffer> Generator::loadImage(const std::string& filePath) {
    auto encoded = loadImageBytes(filePath);
    if (!encoded.isOk()) return encoded.status();
    return loadImageFromMemory(encoded.value().data(), encoded.value().size());
}

Result<std::vector<uint8_t>> Generator::loadImageBytes(const std::string& filePath) {
    auto fsPath = pathFromUtf8(filePath);
    std::ifstream file(fsPath, std::ios::binary);
    if (!file) {
        return Status::ioError("Failed to open image file: " + filePath);
    }
    std::error_code ec;
    auto fsize = std::filesystem::file_size(fsPath, ec);
    if (!ec && fsize > 0) {
        std::vector<uint8_t> bytes(static_cast<size_t>(fsize));
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(fsize));
        if (file.bad()) return Status::ioError("Failed to read image file: " + filePath);
        bytes.resize(static_cast<size_t>(file.gcount()));
        return bytes;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (file.bad()) return Status::ioError("Failed to read image file: " + filePath);
    return bytes;
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

    if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return Status::invalidArgument("Memory buffer too large for image decode");
    }

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 3);
    if (!pixels) {
        return Status::parseError("Failed to decode image from memory: " + std::string(stbi_failure_reason()));
    }

    struct StbiGuard {
        unsigned char* p;
        ~StbiGuard() { if (p) stbi_image_free(p); }
    } guard{pixels};

    if (w <= 0 || h <= 0) {
        return Status::parseError("Invalid image dimensions decoded from memory");
    }

    size_t numPixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (numPixels > std::numeric_limits<size_t>::max() / 3) {
        return Status::internal("Image dimensions too large to allocate");
    }
    size_t totalBytes = numPixels * 3;

    ImageBuffer buf;
    buf.width = w;
    buf.height = h;
    buf.channels = 3;
    buf.data.assign(pixels, pixels + totalBytes);

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
    if (tjDecompressHeader3(handle, data, static_cast<unsigned long>(size), &origW, &origH, &subsamp, &cs) != 0) {
        return Status::parseError("TurboJPEG header parse failed: " + std::string(tjGetErrorStr2(handle)));
    }

    if (outOrigW) *outOrigW = origW;
    if (outOrigH) *outOrigH = origH;

    if (scaleDenom <= 0) {
        scaleDenom = (origW >= 2048 || origH >= 2048) ? 2 : 1;
    }
    int decW = (origW + scaleDenom - 1) / scaleDenom;
    int decH = (origH + scaleDenom - 1) / scaleDenom;

    if (decW <= 0 || decH <= 0) {
        return Status::parseError("Invalid decoded dimensions from TurboJPEG");
    }

    ImageBuffer buf;
    buf.width = decW;
    buf.height = decH;
    buf.channels = 3;
    buf.data.resize(static_cast<size_t>(decW) * static_cast<size_t>(decH) * 3);

    int flags = TJFLAG_FASTDCT;
    if (tjDecompress2(handle, data, static_cast<unsigned long>(size), buf.data.data(), decW, 0, decH, TJPF_RGB, flags) != 0) {
        return Status::parseError("TurboJPEG decompress failed: " + std::string(tjGetErrorStr2(handle)));
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
    if (orientation < 1 || orientation > 8 || src.width <= 0 || src.height <= 0 ||
        src.channels <= 0 || src.data.empty()) {
        return src;
    }

    const int inW = src.width;
    const int inH = src.height;
    const int c = src.channels;
    const size_t expectedSize = static_cast<size_t>(inW) * static_cast<size_t>(inH) * static_cast<size_t>(c);
    if (src.data.size() < expectedSize) return src;
    if (orientation == 1) return src;

    ImageBuffer dst;
    dst.channels = c;
    const bool swapsDimensions = orientation >= 5;
    dst.width = swapsDimensions ? inH : inW;
    dst.height = swapsDimensions ? inW : inH;
    dst.data.resize(expectedSize);

    const size_t rowBytes = static_cast<size_t>(inW) * c;

    switch (orientation) {
        case 2: { // mirror horizontal
            if (c == 3) {
                for (int y = 0; y < inH; ++y) {
                    const uint8_t* srcRow = src.data.data() + static_cast<size_t>(y) * rowBytes;
                    uint8_t* dstRow = dst.data.data() + static_cast<size_t>(y) * rowBytes;
                    for (int x = 0; x < inW; ++x) {
                        int dx = inW - 1 - x;
                        dstRow[dx * 3]     = srcRow[x * 3];
                        dstRow[dx * 3 + 1] = srcRow[x * 3 + 1];
                        dstRow[dx * 3 + 2] = srcRow[x * 3 + 2];
                    }
                }
            } else {
                for (int y = 0; y < inH; ++y) {
                    const uint8_t* srcRow = src.data.data() + static_cast<size_t>(y) * rowBytes;
                    uint8_t* dstRow = dst.data.data() + static_cast<size_t>(y) * rowBytes;
                    for (int x = 0; x < inW; ++x) {
                        int dx = inW - 1 - x;
                        std::memcpy(dstRow + static_cast<size_t>(dx) * c, srcRow + static_cast<size_t>(x) * c, c);
                    }
                }
            }
            break;
        }
        case 3: { // rotate 180
            if (c == 3) {
                for (int y = 0; y < inH; ++y) {
                    const uint8_t* srcRow = src.data.data() + static_cast<size_t>(y) * rowBytes;
                    uint8_t* dstRow = dst.data.data() + static_cast<size_t>(inH - 1 - y) * rowBytes;
                    for (int x = 0; x < inW; ++x) {
                        int dx = inW - 1 - x;
                        dstRow[dx * 3]     = srcRow[x * 3];
                        dstRow[dx * 3 + 1] = srcRow[x * 3 + 1];
                        dstRow[dx * 3 + 2] = srcRow[x * 3 + 2];
                    }
                }
            } else {
                for (int y = 0; y < inH; ++y) {
                    const uint8_t* srcRow = src.data.data() + static_cast<size_t>(y) * rowBytes;
                    uint8_t* dstRow = dst.data.data() + static_cast<size_t>(inH - 1 - y) * rowBytes;
                    for (int x = 0; x < inW; ++x) {
                        int dx = inW - 1 - x;
                        std::memcpy(dstRow + static_cast<size_t>(dx) * c, srcRow + static_cast<size_t>(x) * c, c);
                    }
                }
            }
            break;
        }
        case 4: { // mirror vertical
            for (int y = 0; y < inH; ++y) {
                const uint8_t* srcRow = src.data.data() + static_cast<size_t>(y) * rowBytes;
                uint8_t* dstRow = dst.data.data() + static_cast<size_t>(inH - 1 - y) * rowBytes;
                std::memcpy(dstRow, srcRow, rowBytes);
            }
            break;
        }
        case 5: { // transpose: dx = y, dy = x (dst.width = inH)
            constexpr int BLOCK = 32;
            for (int by = 0; by < inH; by += BLOCK) {
                int max_y = std::min(by + BLOCK, inH);
                for (int bx = 0; bx < inW; bx += BLOCK) {
                    int max_x = std::min(bx + BLOCK, inW);
                    for (int y = by; y < max_y; ++y) {
                        const uint8_t* srcPtr = src.data.data() + (static_cast<size_t>(y) * inW + bx) * c;
                        for (int x = bx; x < max_x; ++x, srcPtr += c) {
                            uint8_t* dstPtr = dst.data.data() + (static_cast<size_t>(x) * dst.width + y) * c;
                            std::memcpy(dstPtr, srcPtr, c);
                        }
                    }
                }
            }
            break;
        }
        case 6: { // rotate 90 CW: dx = inH - 1 - y, dy = x (dst.width = inH)
            constexpr int BLOCK = 32;
            for (int by = 0; by < inH; by += BLOCK) {
                int max_y = std::min(by + BLOCK, inH);
                for (int bx = 0; bx < inW; bx += BLOCK) {
                    int max_x = std::min(bx + BLOCK, inW);
                    for (int y = by; y < max_y; ++y) {
                        const uint8_t* srcPtr = src.data.data() + (static_cast<size_t>(y) * inW + bx) * c;
                        int dx = inH - 1 - y;
                        for (int x = bx; x < max_x; ++x, srcPtr += c) {
                            uint8_t* dstPtr = dst.data.data() + (static_cast<size_t>(x) * dst.width + dx) * c;
                            std::memcpy(dstPtr, srcPtr, c);
                        }
                    }
                }
            }
            break;
        }
        case 7: { // transverse: dx = inH - 1 - y, dy = inW - 1 - x (dst.width = inH)
            constexpr int BLOCK = 32;
            for (int by = 0; by < inH; by += BLOCK) {
                int max_y = std::min(by + BLOCK, inH);
                for (int bx = 0; bx < inW; bx += BLOCK) {
                    int max_x = std::min(bx + BLOCK, inW);
                    for (int y = by; y < max_y; ++y) {
                        const uint8_t* srcPtr = src.data.data() + (static_cast<size_t>(y) * inW + bx) * c;
                        int dx = inH - 1 - y;
                        for (int x = bx; x < max_x; ++x, srcPtr += c) {
                            int dy = inW - 1 - x;
                            uint8_t* dstPtr = dst.data.data() + (static_cast<size_t>(dy) * dst.width + dx) * c;
                            std::memcpy(dstPtr, srcPtr, c);
                        }
                    }
                }
            }
            break;
        }
        case 8: { // rotate 270 CW: dx = y, dy = inW - 1 - x (dst.width = inH)
            constexpr int BLOCK = 32;
            for (int by = 0; by < inH; by += BLOCK) {
                int max_y = std::min(by + BLOCK, inH);
                for (int bx = 0; bx < inW; bx += BLOCK) {
                    int max_x = std::min(bx + BLOCK, inW);
                    for (int y = by; y < max_y; ++y) {
                        const uint8_t* srcPtr = src.data.data() + (static_cast<size_t>(y) * inW + bx) * c;
                        int dx = y;
                        for (int x = bx; x < max_x; ++x, srcPtr += c) {
                            int dy = inW - 1 - x;
                            uint8_t* dstPtr = dst.data.data() + (static_cast<size_t>(dy) * dst.width + dx) * c;
                            std::memcpy(dstPtr, srcPtr, c);
                        }
                    }
                }
            }
            break;
        }
        default: break;
    }
    return dst;
}

ImageBuffer Generator::rotateAngle(const ImageBuffer& src, int degrees) {
    int norm = ((degrees % 360) + 360) % 360;
    if (norm == 90) {
        return rotate(src, 6); // 90 CW
    } else if (norm == 180) {
        return rotate(src, 3); // 180
    } else if (norm == 270) {
        return rotate(src, 8); // 270 CW (90 CCW)
    }
    return src;
}

Result<ImageBuffer> Generator::crop(const ImageBuffer& src, int x, int y, int width, int height) {
    if (src.data.empty() || src.width <= 0 || src.height <= 0 || src.channels <= 0) {
        return Status::invalidArgument("Empty source image for cropping");
    }
    if (width <= 0 || height <= 0) {
        return Status::invalidArgument("Invalid crop dimensions");
    }

    int srcW = src.width;
    int srcH = src.height;
    int c = src.channels;

    size_t expectedSrcSize = static_cast<size_t>(srcW) * static_cast<size_t>(srcH) * static_cast<size_t>(c);
    if (src.data.size() < expectedSrcSize) {
        return Status::invalidArgument("Source image buffer smaller than dimensions require");
    }

    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x >= srcW || y >= srcH || width <= 0 || height <= 0) {
        return Status::invalidArgument("Crop rectangle outside image bounds");
    }

    width = std::min(width, srcW - x);
    height = std::min(height, srcH - y);

    ImageBuffer dst;
    dst.width = width;
    dst.height = height;
    dst.channels = c;
    dst.data.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(c));

    for (int row = 0; row < height; ++row) {
        const uint8_t* srcRow = src.data.data() + ((static_cast<size_t>(y + row) * srcW + x) * c);
        uint8_t* dstRow = dst.data.data() + (static_cast<size_t>(row) * width * c);
        std::memcpy(dstRow, srcRow, static_cast<size_t>(width) * c);
    }

    return dst;
}

Result<ImageBuffer> Generator::resize(const ImageBuffer& src, int maxDimension) {
    if (src.data.empty() || src.width <= 0 || src.height <= 0 || src.channels <= 0) {
        return Status::invalidArgument("Empty source image for resizing");
    }
    if (maxDimension <= 0) {
        return Status::invalidArgument("Invalid maxDimension for resizing");
    }

    size_t expectedSrcSize = static_cast<size_t>(src.width) * static_cast<size_t>(src.height) * static_cast<size_t>(src.channels);
    if (src.data.size() < expectedSrcSize) {
        return Status::invalidArgument("Source image buffer smaller than dimensions require");
    }

    int inW = src.width;
    int inH = src.height;

    int outW = inW;
    int outH = inH;

    if (inW > maxDimension || inH > maxDimension) {
        if (inW >= inH) {
            outW = maxDimension;
            outH = std::max(1, static_cast<int>(std::round(static_cast<double>(inH) * maxDimension / inW)));
        } else {
            outH = maxDimension;
            outW = std::max(1, static_cast<int>(std::round(static_cast<double>(inW) * maxDimension / inH)));
        }
    } else {
        // No need to upscale
        return src;
    }

    ImageBuffer dst;
    dst.width = outW;
    dst.height = outH;
    dst.channels = src.channels;
    dst.data.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH) * static_cast<size_t>(src.channels));

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
    if (img.data.empty() || img.width <= 0 || img.height <= 0 || img.channels <= 0) {
        return Status::invalidArgument("Empty or invalid image buffer for saving JPEG");
    }
    quality = std::clamp(quality, 1, 100);

    std::filesystem::path p = pathFromUtf8(destPath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::filesystem::path tmpPath = makeTempPath(p);
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        if (!ofs) {
            return Status::ioError("Failed to open output file: " + destPath);
        }

        auto writeFunc = [](void* context, void* data, int size) {
            auto* stream = static_cast<std::ostream*>(context);
            stream->write(reinterpret_cast<const char*>(data), size);
        };

        int rc = stbi_write_jpg_to_func(writeFunc, &ofs, img.width, img.height, img.channels, img.data.data(), quality);
        ofs.flush();
        if (!rc || !ofs) {
            ofs.close();
            std::error_code ecClean;
            std::filesystem::remove(tmpPath, ecClean);
            return Status::ioError("Failed to write JPEG thumbnail to " + destPath);
        }
    }

    return commitFile(tmpPath, p);
}

Status Generator::saveJpegFast(const ImageBuffer& img, const std::string& destPath, int quality) {
    if (img.data.empty() || img.width <= 0 || img.height <= 0 || img.channels <= 0) {
        return Status::invalidArgument("Empty or invalid image buffer for saving JPEG");
    }
    quality = std::clamp(quality, 1, 100);

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
                struct BufferGuard {
                    unsigned char* b;
                    ~BufferGuard() { if (b) tjFree(b); }
                } bufGuard{jpegBuf};

                std::filesystem::path tmpPath = makeTempPath(p);
                {
                    std::ofstream ofs(tmpPath, std::ios::binary);
                    if (ofs) {
                        ofs.write(reinterpret_cast<const char*>(jpegBuf), static_cast<std::streamsize>(jpegSize));
                        ofs.flush();
                        if (ofs) {
                            ofs.close();
                            return commitFile(tmpPath, p);
                        }
                    }
                }
                std::error_code ecClean;
                std::filesystem::remove(tmpPath, ecClean);
            }
        }
    }
#endif

    return saveJpeg(img, destPath, quality);
}

Status Generator::savePng(const ImageBuffer& img, const std::string& destPath) {
    if (img.data.empty() || img.width <= 0 || img.height <= 0 || img.channels <= 0) {
        return Status::invalidArgument("Empty or invalid image buffer for saving PNG");
    }

    std::filesystem::path p = pathFromUtf8(destPath);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }

    std::filesystem::path tmpPath = makeTempPath(p);
    {
        std::ofstream ofs(tmpPath, std::ios::binary);
        if (!ofs) {
            return Status::ioError("Failed to open output file: " + destPath);
        }

        auto writeFunc = [](void* context, void* data, int size) {
            auto* stream = static_cast<std::ostream*>(context);
            stream->write(reinterpret_cast<const char*>(data), size);
        };

        int stride = img.width * img.channels;
        int rc = stbi_write_png_to_func(writeFunc, &ofs, img.width, img.height, img.channels, img.data.data(), stride);
        ofs.flush();
        if (!rc || !ofs) {
            ofs.close();
            std::error_code ecClean;
            std::filesystem::remove(tmpPath, ecClean);
            return Status::ioError("Failed to write PNG thumbnail to " + destPath);
        }
    }

    return commitFile(tmpPath, p);
}

Result<ImageBuffer> Generator::generateProceduralThumbnail(
    const std::string& mediaType,
    const std::string& title,
    int size
) {
    (void)title;
    if (size <= 0) size = 256;
    ImageBuffer img;
    img.width = size;
    img.height = size;
    img.channels = 3;
    img.data.resize(static_cast<size_t>(size) * static_cast<size_t>(size) * 3);

    bool isAudio = (mediaType == "audio");

    uint8_t r1 = isAudio ? 24 : 18;
    uint8_t g1 = isAudio ? 18 : 24;
    uint8_t b1 = isAudio ? 36 : 38;

    uint8_t r2 = isAudio ? 48 : 28;
    uint8_t g2 = isAudio ? 28 : 42;
    uint8_t b2 = isAudio ? 72 : 68;

    int cx = size / 2;
    int cy = size / 2;
    int maxR = size / 3;

    for (int y = 0; y < size; ++y) {
        float ty = static_cast<float>(y) / size;
        for (int x = 0; x < size; ++x) {
            float tx = static_cast<float>(x) / size;
            float t = (tx + ty) * 0.5f;

            uint8_t pr = static_cast<uint8_t>(r1 + (r2 - r1) * t);
            uint8_t pg = static_cast<uint8_t>(g1 + (g2 - g1) * t);
            uint8_t pb = static_cast<uint8_t>(b1 + (b2 - b1) * t);

            int dx = x - cx;
            int dy = y - cy;
            int distSq = dx * dx + dy * dy;
            int dist = static_cast<int>(std::sqrt(distSq));

            if (isAudio) {
                if (dist <= maxR) {
                    if (dist <= maxR * 0.12f) {
                        pr = 15; pg = 12; pb = 20;
                    } else if (dist <= maxR * 0.38f) {
                        pr = 145; pg = 70; pb = 210;
                    } else {
                        int groove = (dist % 7 < 2) ? 48 : 34;
                        pr = groove; pg = groove - 2; pb = groove + 4;
                    }
                }
            } else {
                if (std::abs(dx) <= maxR && std::abs(dy) <= maxR) {
                    pr = static_cast<uint8_t>(std::min(255, pr + 15));
                    pg = static_cast<uint8_t>(std::min(255, pg + 20));
                    pb = static_cast<uint8_t>(std::min(255, pb + 28));

                    int triLeft = -maxR / 3;
                    int triRight = maxR / 2;
                    int triHalfH = maxR / 2;

                    if (dx >= triLeft && dx <= triRight) {
                        int currentHalfH = triHalfH * (triRight - dx) / std::max(1, (triRight - triLeft));
                        if (std::abs(dy) <= currentHalfH) {
                            pr = 70; pg = 160; pb = 245;
                        }
                    }
                }
            }

            size_t idx = (static_cast<size_t>(y) * size + x) * 3;
            img.data[idx] = pr;
            img.data[idx + 1] = pg;
            img.data[idx + 2] = pb;
        }
    }

    return img;
}

} // namespace imagine::thumbnail
