#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include "imagine/common/error.hpp"

namespace imagine::clip {

struct TokenizerOutput {
    std::vector<int64_t> input_ids;
    std::vector<int64_t> attention_mask;
};

class Tokenizer {
public:
    Tokenizer() = default;
    Status load(const std::string& vocabPath, const std::string& mergesPath);
    bool isLoaded() const;
    TokenizerOutput encode(std::string_view text, int maxLength = 77) const;
private:
    std::vector<std::string> bpe(const std::vector<std::string>& initialWord) const;
    std::unordered_map<std::string, int64_t> vocab_;
    std::unordered_map<std::string, int> mergeRanks_;
    int64_t sotToken_{49406};
    int64_t eotToken_{49407};
    bool loaded_{false};
};

} // namespace imagine::clip
