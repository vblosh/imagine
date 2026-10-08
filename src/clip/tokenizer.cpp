#include "tokenizer.hpp"
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>
#include <nlohmann/json.hpp>

namespace imagine::clip {

static std::unordered_map<uint8_t, uint32_t> getByteEncoder() {
    std::unordered_map<uint8_t, uint32_t> b2u;
    int n = 0;
    for (int b = 0; b < 256; b++) {
        if ((b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255)) {
            b2u[b] = b;
        } else {
            b2u[b] = 256 + n;
            n++;
        }
    }
    return b2u;
}

static std::string utf8_encode(uint32_t codepoint) {
    std::string out;
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | ((codepoint >> 6) & 0x1f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | ((codepoint >> 12) & 0x0f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | ((codepoint >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
    return out;
}

Status Tokenizer::load(const std::string& vocabPath, const std::string& mergesPath) {
    try {
        std::ifstream vf(vocabPath);
        if (!vf.is_open()) return Status::internal("Failed to open vocab file: " + vocabPath);
        nlohmann::json j;
        vf >> j;
        for (auto it = j.begin(); it != j.end(); ++it) {
            vocab_[it.key()] = it.value().get<int64_t>();
        }
        auto sot = vocab_.find("<|startoftext|>");
        if (sot != vocab_.end()) sotToken_ = sot->second;
        auto eot = vocab_.find("<|endoftext|>");
        if (eot != vocab_.end()) eotToken_ = eot->second;

        std::ifstream mf(mergesPath);
        if (!mf.is_open()) return Status::internal("Failed to open merges file: " + mergesPath);
        std::string line;
        std::getline(mf, line); // Skip header
        int rank = 0;
        while (std::getline(mf, line)) {
            if (line.empty()) continue;
            size_t space = line.find(' ');
            if (space != std::string::npos) {
                std::string first = line.substr(0, space);
                std::string second = line.substr(space + 1);
                merges_.push_back({first, second});
                mergeRanks_[first + " " + second] = rank++;
            }
        }
        loaded_ = true;
        return Status::ok();
    } catch (const std::exception& e) {
        return Status::internal(std::string("Tokenizer load error: ") + e.what());
    }
}

bool Tokenizer::isLoaded() const {
    return loaded_;
}

std::vector<std::string> Tokenizer::bpe(const std::string& token) const {
    std::vector<std::string> word;
    for (char c : token) {
        word.push_back(std::string(1, c));
    }
    if (word.empty()) return word;
    word.back() += "</w>";

    while (word.size() > 1) {
        int minRank = std::numeric_limits<int>::max();
        size_t minIdx = 0;
        bool found = false;
        
        for (size_t i = 0; i < word.size() - 1; ++i) {
            std::string pair = word[i] + " " + word[i+1];
            auto it = mergeRanks_.find(pair);
            if (it != mergeRanks_.end() && it->second < minRank) {
                minRank = it->second;
                minIdx = i;
                found = true;
            }
        }
        if (!found) break;

        std::vector<std::string> nextWord;
        for (size_t i = 0; i < minIdx; ++i) nextWord.push_back(word[i]);
        nextWord.push_back(word[minIdx] + word[minIdx+1]);
        for (size_t i = minIdx + 2; i < word.size(); ++i) nextWord.push_back(word[i]);
        word = nextWord;
    }
    return word;
}

TokenizerOutput Tokenizer::encode(std::string_view text, int maxLength) const {
    TokenizerOutput out;
    if (!loaded_) return out;

    std::string lowerText(text);
    std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), ::tolower);
    std::regex re(R"('s|'t|'re|'ve|'m|'ll|'d|[\w]+|[^\s\w]+)");
    auto words_begin = std::sregex_iterator(lowerText.begin(), lowerText.end(), re);
    auto words_end = std::sregex_iterator();

    auto b2u = getByteEncoder();

    out.input_ids.push_back(sotToken_);
    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::string word = i->str();
        std::string encodedWord;
        for (char c : word) {
            encodedWord += utf8_encode(b2u[static_cast<uint8_t>(c)]);
        }
        
        std::vector<std::string> bpeTokens = bpe(encodedWord);
        for (const auto& bt : bpeTokens) {
            auto it = vocab_.find(bt);
            if (it != vocab_.end()) {
                if (out.input_ids.size() < static_cast<size_t>(maxLength - 1)) {
                    out.input_ids.push_back(it->second);
                }
            }
        }
        if (out.input_ids.size() >= static_cast<size_t>(maxLength - 1)) break;
    }
    
    out.input_ids.push_back(eotToken_);
    
    out.attention_mask.resize(maxLength, 0);
    for (size_t i = 0; i < out.input_ids.size(); ++i) {
        out.attention_mask[i] = 1;
    }
    
    while (out.input_ids.size() < static_cast<size_t>(maxLength)) {
        out.input_ids.push_back(0); // padding token
    }

    return out;
}

} // namespace imagine::clip
