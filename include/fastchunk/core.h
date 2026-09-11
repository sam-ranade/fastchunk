#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace fastchunk
{

// ============================================================================
// Error & Diagnostic Codes (Section 2.1)
// ============================================================================

enum class ErrorCode : std::uint32_t
{
    invalid_argument = 1000,
    invalid_configuration = 1001,
    configuration_parse_failed = 1002,
    unsupported_option = 1003,
    invalid_utf8 = 1100,
    file_not_found = 1200,
    permission_denied = 1201,
    mapping_failed = 1202,
    io_error = 1203,
    resource_limit_exceeded = 1204,
    cancelled = 1205,
    tokenizer_unavailable = 1300,
    tokenizer_dependency_missing = 1301,
    tokenizer_model_not_found = 1302,
    tokenizer_model_invalid = 1303,
    tokenizer_version_unsupported = 1304,
    tokenizer_encoding_unsupported = 1305,
    tokenizer_failed = 1306,
    export_failed = 1400,
    exporter_dependency_missing = 1401,
    exporter_configuration_invalid = 1402,
    internal_error = 9000
};

struct Error
{
    std::uint32_t code { 0 };
    std::string name;
    std::string description;
    std::string message;
    std::optional<std::size_t> byte_offset;
    std::string dependency;
    std::string path;
    std::string details_json;
};

enum class DiagnosticSeverity
{
    warning
};

enum class DiagnosticCode : std::uint32_t
{
    invalid_utf8_replaced = 2000,
    invalid_utf8_skipped = 2001,
    data_truncated = 2002,
    metadata_ignored = 2003,
    oversized_token = 2004,
    cpu_affinity_not_applied = 2005,
    malformed_record_skipped = 2006,
    telemetry_queue_overflow = 2007,
    telemetry_delivery_failed = 2008
};

struct Diagnostic
{
    DiagnosticSeverity severity { DiagnosticSeverity::warning };
    std::uint32_t code { 0 };
    std::string name;
    std::string description;
    std::string message;
    std::optional<std::size_t> source_byte_offset;
};

// ============================================================================
// Result<T> Monadic Container (Section 2.1)
// ============================================================================

template <typename T>
class Result
{
public:
    Result(T val, std::vector<Diagnostic> diags = {})
        : data_(std::move(val))
        , diagnostics_(std::move(diags))
    {
    }

    Result(Error err, std::vector<Diagnostic> diags = {})
        : data_(std::move(err))
        , diagnostics_(std::move(diags))
    {
    }

    [[nodiscard]] bool has_value() const noexcept
    {
        return std::holds_alternative<T>(data_);
    }

    [[nodiscard]] const T& value() const& { return std::get<T>(data_); }

    [[nodiscard]] T&& value() && { return std::get<T>(std::move(data_)); }

    [[nodiscard]] const Error* error() const noexcept
    {
        if (const auto* err = std::get_if<Error>(&data_))
        {
            return err;
        }
        return nullptr;
    }

    [[nodiscard]] std::span<const Diagnostic> diagnostics() const noexcept
    {
        return diagnostics_;
    }

private:
    std::variant<T, Error> data_;
    std::vector<Diagnostic> diagnostics_;
};

template <>
class Result<void>
{
public:
    Result(std::vector<Diagnostic> diags = {})
        : diagnostics_(std::move(diags))
    {
    }

    Result(Error err, std::vector<Diagnostic> diags = {})
        : error_(std::move(err))
        , diagnostics_(std::move(diags))
    {
    }

    [[nodiscard]] bool has_value() const noexcept { return !error_.has_value(); }

    [[nodiscard]] const Error* error() const noexcept
    {
        return error_ ? &(*error_) : nullptr;
    }

    [[nodiscard]] std::span<const Diagnostic> diagnostics() const noexcept
    {
        return diagnostics_;
    }

private:
    std::optional<Error> error_;
    std::vector<Diagnostic> diagnostics_;
};

// ============================================================================
// Ingestion Options & Interfaces (Section 2.1 & Section 3.2)
// ============================================================================

enum class InputMode
{
    zero_copy,
    copy
};
enum class InvalidUtf8Policy
{
    error,
    replace,
    skip
};

struct InputOptions
{
    InputMode mode { InputMode::zero_copy };
    InvalidUtf8Policy invalid_utf8 { InvalidUtf8Policy::error };
    enum class RecordMode
    {
        none,
        ndjson
    };
    enum class MalformedRecordPolicy
    {
        error,
        skip
    };
    RecordMode record_mode { RecordMode::none };
    MalformedRecordPolicy malformed_record { MalformedRecordPolicy::error };
    std::string record_id_field;
};

struct FileBuffer
{
    std::span<const std::byte> data;
    std::uint64_t file_size { 0 };
    std::vector<Diagnostic> diagnostics;
};

class IReader
{
public:
    virtual ~IReader() = default;
    [[nodiscard]] virtual Result<FileBuffer>
    open(const std::filesystem::path& filepath, const InputOptions& options) = 0;
    [[nodiscard]] virtual Result<void> close() = 0;
};

// ============================================================================
// Tokenization Contract (Section 2.2.3)
// ============================================================================

struct EncodedToken
{
    std::uint32_t id { 0 };
    std::size_t start_byte { 0 };
    std::size_t end_byte { 0 };
};

class ITokenizer
{
public:
    virtual ~ITokenizer() = default;
    [[nodiscard]] virtual Result<std::vector<EncodedToken>>
    encode(std::string_view text) = 0;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
};

// ============================================================================
// Chunking Models & Streaming Contracts (Section 2.2)
// ============================================================================

struct ChunkResult
{
    enum class SourceOffsetKind
    {
        unavailable,
        exact,
        record
    };

    std::string text;
    std::vector<std::uint32_t> tokens;
    std::size_t start_byte { 0 };
    std::size_t end_byte { 0 };
    std::string doc_id;
    std::string chunk_id;
    std::string record_id;
    std::string tokenizer_name;
    std::string tokenizer_configuration;
    std::size_t source_start_byte { 0 };
    std::size_t source_end_byte { 0 };
    SourceOffsetKind source_offset_kind { SourceOffsetKind::unavailable };
    bool has_source_offsets { false };
    std::uint32_t chunk_index { 0 };
    std::string metadata;
};

struct ChunkView
{
    std::string_view text;
    std::span<const std::uint32_t> tokens;
    std::string_view doc_id;
    std::string_view chunk_id;
    std::size_t start_byte { 0 };
    std::size_t end_byte { 0 };
    std::string_view record_id;
    std::size_t source_start_byte { 0 };
    std::size_t source_end_byte { 0 };
    ChunkResult::SourceOffsetKind source_offset_kind {
        ChunkResult::SourceOffsetKind::unavailable
    };
    bool has_source_offsets { false };
    std::uint32_t chunk_index { 0 };
    std::string_view metadata;
    std::string_view tokenizer_name;
    std::string_view tokenizer_configuration;
};

struct ChunkOptions
{
    std::size_t max_tokens { 0 };
    std::size_t overlap_tokens { 0 };
    enum class MetadataCollisionPolicy
    {
        error,
        ignore
    };
    MetadataCollisionPolicy metadata_collision { MetadataCollisionPolicy::error };
};

struct ChunkInputContext
{
    std::string doc_id;
    std::string record_id;
    std::string tokenizer_name;
    std::string tokenizer_configuration;
    std::size_t source_start_byte { 0 };
    std::size_t source_end_byte { 0 };
    ChunkResult::SourceOffsetKind source_offset_kind {
        ChunkResult::SourceOffsetKind::unavailable
    };
    std::string metadata;
};

class CancellationToken
{
public:
    void request_cancel() noexcept
    {
        cancelled_.store(true, std::memory_order_relaxed);
    }
    void cancel() noexcept { request_cancel(); }
    void reset() noexcept { cancelled_.store(false, std::memory_order_relaxed); }
    [[nodiscard]] bool is_cancelled() const noexcept
    {
        return cancelled_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> cancelled_ { false };
};

class IChunkStream
{
public:
    virtual ~IChunkStream() = default;
    [[nodiscard]] virtual Result<bool> next(ChunkView& output) = 0;
    virtual void cancel() noexcept = 0;
};

struct ExportMetadata
{
    std::string doc_id;
    std::string source_path;
    std::string custom_json_metadata { "{}" };
    std::string tokenizer_name;
    std::string tokenizer_configuration { "{}" };
    std::string effective_configuration { "{}" };
};

class IExporter
{
public:
    virtual ~IExporter() = default;
    [[nodiscard]] virtual Result<void>
    export_chunks(std::span<const ChunkResult> chunks,
        const ExportMetadata& metadata)
        = 0;
};

class IStreamingExporter
{
public:
    virtual ~IStreamingExporter() = default;
    [[nodiscard]] virtual Result<void>
    export_stream(IChunkStream& stream, const ExportMetadata& metadata,
        const CancellationToken* cancellation = nullptr)
        = 0;
};

class IChunker
{
public:
    virtual ~IChunker() = default;
    [[nodiscard]] virtual Result<std::vector<ChunkResult>>
    chunk_buffer(std::span<const std::byte> buffer, const ChunkOptions& options,
        ITokenizer& tokenizer, const ChunkInputContext& context,
        const CancellationToken* cancellation = nullptr)
        = 0;

    [[nodiscard]] virtual Result<std::unique_ptr<IChunkStream>>
    stream_buffer(std::span<const std::byte> buffer, const ChunkOptions& options,
        ITokenizer& tokenizer, const ChunkInputContext& context,
        const CancellationToken* cancellation = nullptr)
        = 0;
};

} // namespace fastchunk
