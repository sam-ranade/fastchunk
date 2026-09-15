#include "fastchunk/tokenizers.h"
#include "fastchunk/constants.h"
#include <algorithm>
#include <cctype>
#include <filesystem>

#if defined(FASTCHUNK_HAS_TIKTOKEN)
#include "fastchunk_tiktoken.h"
#endif
#if defined(FASTCHUNK_HAS_HUGGINGFACE)
#include "fastchunk_huggingface.h"
#endif

namespace fastchunk
{

Result<std::vector<EncodedToken>>
WhitespaceTokenizer::encode(std::string_view text)
{
    std::vector<EncodedToken> tokens;
    std::size_t start = 0;
    std::uint32_t token_id = 0;

    while (start < text.size())
    {
        while (start < text.size() && std::isspace(
                   static_cast<unsigned char>(text[start])))
            ++start;
        if (start == text.size())
            break;

        std::size_t end = start;
        while (end < text.size() && !std::isspace(
                   static_cast<unsigned char>(text[end])))
            ++end;
        tokens.push_back(EncodedToken {
            .id = token_id++, .start_byte = start, .end_byte = end });
        start = end;
    }
    return Result<std::vector<EncodedToken>>(std::move(tokens));
}

std::string_view WhitespaceTokenizer::name() const noexcept
{
    return "whitespace";
}

// --- Tiktoken Implementation ---
TiktokenTokenizer::TiktokenTokenizer(std::string model_name)
    : model_name_(std::move(model_name))
{
#if defined(FASTCHUNK_HAS_TIKTOKEN)
    const auto encoding = model_name_ == constants::tokenizer::tiktoken
        ? std::string { constants::tokenizer::cl100k_base }
        : model_name_;
    if (encoding.empty())
    {
        backend_error_ = "Tiktoken requires encoding cl100k_base or o200k_base.";
        backend_error_code_ = static_cast<std::uint32_t>(ErrorCode::tokenizer_encoding_unsupported);
        return;
    }
    fastchunk_tiktoken_backend* handle = nullptr;
    char* error = nullptr;
    if (fastchunk_tiktoken_create(encoding.c_str(), &handle, &error) != 0)
    {
        backend_error_ = error ? error : "Tiktoken backend initialization failed.";
        backend_error_code_ = static_cast<std::uint32_t>(
            encoding == constants::tokenizer::cl100k_base
                || encoding == constants::tokenizer::o200k_base
            ? ErrorCode::tokenizer_failed
            : ErrorCode::tokenizer_encoding_unsupported);
        fastchunk_tiktoken_free_error(error);
        return;
    }
    backend_ = handle;
#else
    backend_error_ = "Tiktoken backend is disabled; reconfigure with FASTCHUNK_ENABLE_TIKTOKEN=ON.";
    backend_error_code_ = static_cast<std::uint32_t>(ErrorCode::tokenizer_unavailable);
#endif
}

TiktokenTokenizer::~TiktokenTokenizer()
{
#if defined(FASTCHUNK_HAS_TIKTOKEN)
    fastchunk_tiktoken_destroy(static_cast<fastchunk_tiktoken_backend*>(backend_));
#endif
}

Result<std::vector<EncodedToken>>
TiktokenTokenizer::encode(std::string_view text)
{
#if defined(FASTCHUNK_HAS_TIKTOKEN)
    if (!backend_)
        return Result<std::vector<EncodedToken>>(Error {
            .code = backend_error_code_,
            .name = backend_error_code_ == static_cast<std::uint32_t>(ErrorCode::tokenizer_encoding_unsupported)
                ? "tokenizer_encoding_unsupported" : "tokenizer_unavailable",
            .description = "The requested tokenizer backend is not registered in this build.",
            .message = backend_error_ });

    fastchunk_tiktoken_token* output = nullptr;
    std::size_t output_size = 0;
    char* error = nullptr;
    const auto status = fastchunk_tiktoken_encode(
        static_cast<fastchunk_tiktoken_backend*>(backend_),
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), &output,
        &output_size, &error);
    if (status != 0)
    {
        Error result_error {
            .code = static_cast<std::uint32_t>(status == 4
                    ? ErrorCode::invalid_utf8
                    : ErrorCode::tokenizer_failed),
            .name = status == 4 ? "invalid_utf8" : "tokenizer_failed",
            .description = status == 4
                ? "Input contains malformed UTF-8."
                : "The selected tokenizer failed while encoding input.",
            .message = error ? error : "Tiktoken encoding failed." };
        fastchunk_tiktoken_free_error(error);
        return Result<std::vector<EncodedToken>>(std::move(result_error));
    }
    std::vector<EncodedToken> tokens;
    tokens.reserve(output_size);
    for (std::size_t index = 0; index < output_size; ++index)
        tokens.push_back({ output[index].id, output[index].start_byte, output[index].end_byte });
    fastchunk_tiktoken_free_tokens(output, output_size);
    return Result<std::vector<EncodedToken>>(std::move(tokens));
#else
    (void)text;
    return Result<std::vector<EncodedToken>>(Error {
        .code = static_cast<std::uint32_t>(ErrorCode::tokenizer_unavailable),
        .name = "tokenizer_unavailable",
        .description = "The requested tokenizer backend is not registered in this build.",
        .message = backend_error_ });
#endif
}

std::string_view TiktokenTokenizer::name() const noexcept
{
    return model_name_;
}

// --- HuggingFace Implementation ---
HuggingFaceTokenizer::HuggingFaceTokenizer(const std::string& json_path)
    : model_path_(json_path)
{
#if defined(FASTCHUNK_HAS_HUGGINGFACE)
    fastchunk_huggingface_backend* handle = nullptr;
    char* error = nullptr;
    if (fastchunk_huggingface_create(json_path.c_str(), &handle, &error) != 0)
    {
        backend_error_ = error ? error : "Hugging Face tokenizer initialization failed.";
        backend_error_code_ = std::filesystem::exists(json_path)
            ? static_cast<std::uint32_t>(ErrorCode::tokenizer_model_invalid)
            : static_cast<std::uint32_t>(ErrorCode::tokenizer_model_not_found);
        fastchunk_huggingface_free_error(error);
        return;
    }
    hf_handle_ = handle;
#else
    backend_error_ = "Hugging Face backend is disabled; reconfigure with FASTCHUNK_ENABLE_HUGGINGFACE=ON.";
    backend_error_code_ = static_cast<std::uint32_t>(ErrorCode::tokenizer_unavailable);
#endif
}

HuggingFaceTokenizer::~HuggingFaceTokenizer()
{
#if defined(FASTCHUNK_HAS_HUGGINGFACE)
    fastchunk_huggingface_destroy(static_cast<fastchunk_huggingface_backend*>(hf_handle_));
#endif
}

Result<std::vector<EncodedToken>>
HuggingFaceTokenizer::encode(std::string_view text)
{
#if defined(FASTCHUNK_HAS_HUGGINGFACE)
    if (!hf_handle_)
    {
        return Result<std::vector<EncodedToken>>(Error {
            .code = backend_error_code_,
            .name = backend_error_code_ == static_cast<std::uint32_t>(ErrorCode::tokenizer_model_not_found)
                ? "tokenizer_model_not_found" : "tokenizer_model_invalid",
            .description = "The tokenizer files are present but invalid or incomplete.",
            .message = backend_error_,
            .path = model_path_ });
    }
    fastchunk_huggingface_token* output = nullptr;
    std::size_t output_size = 0;
    char* error = nullptr;
    const auto status = fastchunk_huggingface_encode(
        static_cast<fastchunk_huggingface_backend*>(hf_handle_),
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), &output,
        &output_size, &error);
    if (status != 0)
    {
        Error result_error {
            .code = static_cast<std::uint32_t>(status == 4
                    ? ErrorCode::invalid_utf8
                    : ErrorCode::tokenizer_failed),
            .name = status == 4 ? "invalid_utf8" : "tokenizer_failed",
            .description = status == 4
                ? "Input contains malformed UTF-8."
                : "The selected tokenizer failed while encoding input.",
            .message = error ? error : "Hugging Face encoding failed.",
            .path = model_path_ };
        fastchunk_huggingface_free_error(error);
        return Result<std::vector<EncodedToken>>(std::move(result_error));
    }
    std::vector<EncodedToken> tokens;
    tokens.reserve(output_size);
    for (std::size_t index = 0; index < output_size; ++index)
        tokens.push_back({ output[index].id, output[index].start_byte, output[index].end_byte });
    fastchunk_huggingface_free_tokens(output, output_size);
    return Result<std::vector<EncodedToken>>(std::move(tokens));
#else
    (void)text;
    return Result<std::vector<EncodedToken>>(Error {
        .code = static_cast<std::uint32_t>(ErrorCode::tokenizer_unavailable),
        .name = "tokenizer_unavailable",
        .description = "The requested tokenizer backend is not registered in this build.",
        .message = backend_error_,
        .path = model_path_ });
#endif
}

std::string_view HuggingFaceTokenizer::name() const noexcept
{
    return constants::tokenizer::huggingface_runtime_name;
}

// --- Tokenizer Factory ---
Result<std::unique_ptr<ITokenizer>>
create_tokenizer_from_name(std::string_view name,
    std::string_view config_or_path)
{
    if (name == "whitespace")
    {
        return Result<std::unique_ptr<ITokenizer>>(
            std::make_unique<WhitespaceTokenizer>());
    }
    else if (name == constants::tokenizer::tiktoken || name == constants::tokenizer::cl100k_base || name == constants::tokenizer::o200k_base)
    {
        return Result<std::unique_ptr<ITokenizer>>(
            std::make_unique<TiktokenTokenizer>(std::string(
                config_or_path.empty() ? name : config_or_path)));
    }
    else if (name == constants::tokenizer::huggingface || name == constants::tokenizer::huggingface_alias)
    {
        return Result<std::unique_ptr<ITokenizer>>(
            std::make_unique<HuggingFaceTokenizer>(std::string(config_or_path)));
    }

    return Result<std::unique_ptr<ITokenizer>>(Error {
        .code = static_cast<std::uint32_t>(ErrorCode::invalid_argument),
        .name = std::string(constants::error_name::invalid_argument),
        .description = "A public argument is missing, malformed, or out of range.",
        .message = "Unknown or unsupported tokenizer backend engine requested." });
}

} // namespace fastchunk