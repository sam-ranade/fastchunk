#pragma once

#include "fastchunk/core.h"
#include <memory>
#include <string>
#include <vector>

namespace fastchunk
{

class WhitespaceTokenizer final : public ITokenizer
{
public:
    Result<std::vector<EncodedToken>> encode(std::string_view text) override;
    std::string_view name() const noexcept override;
};

// Native BPE / Tiktoken Engine Implementation
class TiktokenTokenizer final : public ITokenizer
{
public:
    explicit TiktokenTokenizer(std::string model_name);
    ~TiktokenTokenizer() override;

    Result<std::vector<EncodedToken>> encode(std::string_view text) override;
    std::string_view name() const noexcept override;

private:
    std::string model_name_;
    void* backend_ { nullptr };
    std::string backend_error_;
    std::uint32_t backend_error_code_ {
        static_cast<std::uint32_t>(ErrorCode::tokenizer_unavailable)
    };
};

// HuggingFace Tokenizer Adapter
class HuggingFaceTokenizer final : public ITokenizer
{
public:
    explicit HuggingFaceTokenizer(const std::string& json_path);
    ~HuggingFaceTokenizer() override;

    Result<std::vector<EncodedToken>> encode(std::string_view text) override;
    std::string_view name() const noexcept override;

private:
    std::string model_path_;
    void* hf_handle_ {
        nullptr
    }; // Opaque handle to underlying C-FFI / tokenizers-cpp instance
    std::string backend_error_;
    std::uint32_t backend_error_code_ {
        static_cast<std::uint32_t>(ErrorCode::tokenizer_unavailable)
    };
};

// Factory interface for tokenizer instantiation
[[nodiscard]] Result<std::unique_ptr<ITokenizer>>
create_tokenizer_from_name(std::string_view name,
    std::string_view config_or_path = "");

} // namespace fastchunk
