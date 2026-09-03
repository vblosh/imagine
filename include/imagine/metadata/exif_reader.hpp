#pragma once

#include <string>
#include "imagine/common/types.hpp"
#include "imagine/common/error.hpp"

namespace imagine::metadata {

class ExifReader {
public:
    static Result<ExifData> readFromBuffer(const uint8_t* data, size_t size);
    static Result<ExifData> readFromFile(const std::string& filePath);

    static int64_t parseExifDate(const std::string& dateStr);
};

} // namespace imagine::metadata
