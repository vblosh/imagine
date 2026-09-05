#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "imagine/common/error.hpp"

namespace imagine::thumbnail {

struct ImageBuffer {
    int width{0};
    int height{0};
    int channels{0};
    std::vector<uint8_t> data;
};

class Generator {
public:
    static Result<ImageBuffer> loadImage(const std::string& filePath);
    static Result<ImageBuffer> loadImageFromMemory(const uint8_t* data, size_t size);

    static Result<ImageBuffer> resize(const ImageBuffer& src, int maxDimension);
    static ImageBuffer rotate(const ImageBuffer& src, int orientation);

    static Status saveJpeg(const ImageBuffer& img, const std::string& destPath, int quality = 85);
    static Status savePng(const ImageBuffer& img, const std::string& destPath);

    static Result<std::pair<int, int>> getImageDimensions(const std::string& filePath);
    static Result<std::pair<int, int>> getImageDimensionsFromMemory(const uint8_t* data, size_t size);
};

} // namespace imagine::thumbnail
