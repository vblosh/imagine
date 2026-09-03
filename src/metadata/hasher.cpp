#include "imagine/metadata/hasher.hpp"
#include <openssl/evp.h>
#include <fstream>
#include <vector>
#include <sstream>
#include <iomanip>

namespace imagine::metadata {

static std::string hexEncode(const unsigned char* hash, unsigned int len) {
    std::ostringstream oss;
    for (unsigned int i = 0; i < len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

std::string Hasher::computeBytesSha256(const uint8_t* data, size_t size) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    const EVP_MD* md = EVP_sha256();
    EVP_DigestInit_ex(ctx, md, nullptr);
    EVP_DigestUpdate(ctx, data, size);

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;
    EVP_DigestFinal_ex(ctx, hash, &hashLen);
    EVP_MD_CTX_free(ctx);

    return hexEncode(hash, hashLen);
}

Result<std::string> Hasher::computeFileSha256(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
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
