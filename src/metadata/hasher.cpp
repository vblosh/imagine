#include "imagine/metadata/hasher.hpp"
#include "imagine/common/types.hpp"
#include <openssl/evp.h>
#include <fstream>
#include <vector>

namespace imagine::metadata {

static std::string hexEncode(const unsigned char* hash, unsigned int len) {
    static constexpr char kHexDigits[] = "0123456789abcdef";
    std::string out(static_cast<size_t>(len) * 2, '\0');
    for (unsigned int i = 0; i < len; ++i) {
        out[static_cast<size_t>(i) * 2]     = kHexDigits[(hash[i] >> 4) & 0x0F];
        out[static_cast<size_t>(i) * 2 + 1] = kHexDigits[hash[i] & 0x0F];
    }
    return out;
}

std::string Hasher::computeBytesSha256(const uint8_t* data, size_t size) {
    if (!data && size != 0) {
        return "";
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    const EVP_MD* md = EVP_sha256();
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }
    if (size > 0 && EVP_DigestUpdate(ctx, data, size) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hashLen) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }
    EVP_MD_CTX_free(ctx);

    return hexEncode(hash, hashLen);
}

Result<std::string> Hasher::computeFileSha256(const std::string& filePath) {
    std::ifstream file(pathFromUtf8(filePath), std::ios::binary);
    if (!file.is_open()) {
        return Status::ioError("Unable to open file for hashing: " + filePath);
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return Status::internal("Failed to allocate EVP_MD_CTX");
    }

    const EVP_MD* md = EVP_sha256();
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return Status::internal("Failed to initialize SHA-256 digest");
    }

    std::vector<char> buffer(64 * 1024); // 64KB buffer
    while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        if (EVP_DigestUpdate(ctx, buffer.data(), file.gcount()) != 1) {
            EVP_MD_CTX_free(ctx);
            return Status::internal("Failed to update SHA-256 digest");
        }
    }
    if (file.bad() || !file.eof()) {
        EVP_MD_CTX_free(ctx);
        return Status::ioError("Error reading file during hashing: " + filePath);
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hashLen) != 1) {
        EVP_MD_CTX_free(ctx);
        return Status::internal("Failed to finalize SHA-256 digest");
    }
    EVP_MD_CTX_free(ctx);

    return hexEncode(hash, hashLen);
}

} // namespace imagine::metadata
