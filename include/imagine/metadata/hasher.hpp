#pragma once

#include <string>
#include <cstdint>
#include "imagine/common/error.hpp"

namespace imagine::metadata {

class Hasher {
public:
    static Result<std::string> computeFileSha256(const std::string& filePath);
    static std::string computeBytesSha256(const uint8_t* data, size_t size);
};

} // namespace imagine::metadata
