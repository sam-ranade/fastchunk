#pragma once

#include "fastchunk/core.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fastchunk
{

struct Configuration
{
    int schema_version { 1 };

    struct Input
    {
        std::string path;
        std::string restricted_root;
        InputMode mode { InputMode::zero_copy };
        InvalidUtf8Policy invalid_utf8 { InvalidUtf8Policy::error };
        bool recursive { false };
        bool include_hidden { false };
        InputOptions::RecordMode record_mode { InputOptions::RecordMode::none };
        std::string record_id_field;
        InputOptions::MalformedRecordPolicy on_malformed_record {
            InputOptions::MalformedRecordPolicy::error
        };
    } input;

    struct Tokenizer
    {
        std::string type;
        std::string encoding;
        std::string tokenizer_path;
    } tokenizer;

    struct Chunking
    {
        std::optional<std::size_t> max_tokens;
        std::optional<std::size_t> overlap_tokens;
        ChunkOptions::MetadataCollisionPolicy metadata_collision {
            ChunkOptions::MetadataCollisionPolicy::error
        };
    } chunking;

    struct Concurrency
    {
        std::size_t workers { 0 };
        std::string result_order { "ordered" };
        struct CpuAffinity
        {
            std::string mode { "none" };
            std::vector<std::size_t> cpus;
            bool required { false };
        } cpu_affinity;
    } concurrency;

    struct Limits
    {
        std::optional<std::size_t> max_file_size_mb;
        std::optional<std::size_t> max_record_size_mb;
        std::optional<std::size_t> max_metadata_size_mb;
        std::optional<std::size_t> max_chunks_per_document;
        std::optional<std::size_t> max_files_per_directory;
        std::optional<std::size_t> max_memory_mb;
        std::optional<std::size_t> max_tokenizer_model_size_mb;
    } limits;

    struct Export
    {
        struct Retries
        {
            std::size_t max_attempts { 1 };
            std::size_t initial_delay_ms { 100 };
            std::size_t max_delay_ms { 5000 };
        } retries;
        std::string type { "ndjson" };
        std::string path;
        std::string on_error { "fail_batch" };
    } export_options;

    struct Telemetry
    {
        bool enabled { false };
        std::string exporter { "none" };
        std::size_t queue_size { 8192 };
        std::string on_failure { "warn" };
        bool include_document_attributes { false };
        std::string endpoint;
        std::string token_env;
        std::size_t shutdown_timeout_ms { 5000 };
    } telemetry;

    bool json_errors { false };

    [[nodiscard]] Result<void> validate() const;
    [[nodiscard]] std::string effective_json() const;
};

[[nodiscard]] Configuration default_configuration();
[[nodiscard]] Result<Configuration> load_configuration(std::string_view path);

} // namespace fastchunk
