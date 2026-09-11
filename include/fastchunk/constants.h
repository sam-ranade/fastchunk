#pragma once

#include <cstdint>
#include <string_view>

namespace fastchunk::constants
{
namespace tokenizer
{
    inline constexpr std::string_view tiktoken = "tiktoken";
    inline constexpr std::string_view cl100k_base = "cl100k_base";
    inline constexpr std::string_view o200k_base = "o200k_base";
    inline constexpr std::string_view huggingface = "huggingface";
    inline constexpr std::string_view huggingface_alias = "hf";
    inline constexpr std::string_view huggingface_runtime_name = "huggingface_tokenizer";
    inline constexpr std::string_view default_name = tiktoken;
    inline constexpr std::uint32_t end_of_text_token = 100257;
    inline constexpr std::uint32_t fim_prefix_token = 100258;
    inline constexpr std::uint32_t fim_middle_token = 100259;
    inline constexpr std::uint32_t fim_suffix_token = 100260;
    inline constexpr std::uint32_t huggingface_first_token = 500;
}

namespace error_name
{
    inline constexpr std::string_view invalid_argument = "invalid_argument";
    inline constexpr std::string_view invalid_configuration = "invalid_configuration";
    inline constexpr std::string_view unsupported_option = "unsupported_option";
    inline constexpr std::string_view invalid_utf8 = "invalid_utf8";
    inline constexpr std::string_view file_not_found = "file_not_found";
    inline constexpr std::string_view mapping_failed = "mapping_failed";
    inline constexpr std::string_view io_error = "io_error";
    inline constexpr std::string_view resource_limit_exceeded = "resource_limit_exceeded";
    inline constexpr std::string_view cancelled = "cancelled";
    inline constexpr std::string_view export_failed = "export_failed";
}

namespace diagnostic_name
{
    inline constexpr std::string_view invalid_utf8_replaced = "invalid_utf8_replaced";
    inline constexpr std::string_view invalid_utf8_skipped = "invalid_utf8_skipped";
    inline constexpr std::string_view oversized_token = "oversized_token";
    inline constexpr std::string_view malformed_record_skipped = "malformed_record_skipped";
}

namespace export_schema
{
    inline constexpr std::uint32_t version = 1;
    inline constexpr std::string_view unavailable = "unavailable";
    inline constexpr std::string_view exact = "exact";
    inline constexpr std::string_view record = "record";
}

namespace records
{
    inline constexpr std::string_view generated_id_prefix = "record:";
}
}
