#pragma once

#include <span>
#include <string>
#include <vector>
#include "imagine/common/types.hpp"
#include "imagine/common/error.hpp"

namespace imagine::metadata {

class ExifReader {
public:
    static Result<ExifData> readFromBuffer(const uint8_t* data, size_t size);
    static Result<ExifData> readFromFile(const std::string& filePath);

    // Transfer source EXIF to edited JPEG/PNG, updating orientation and dimensions.
    static Status preserveForEdit(std::span<const uint8_t> source, std::vector<uint8_t>& edited,
                                  int width, int height);
    static int64_t parseExifDate(const std::string& dateStr);
};

} // namespace imagine::metadata
