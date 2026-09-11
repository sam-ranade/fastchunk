#pragma once

#include "fastchunk/constants.h"
#include "fastchunk/core.h"

#include <algorithm>
#include <chrono>
#include <ostream>
#include <string_view>
#include <thread>

namespace fastchunk
{

class NdjsonExporter final : public IExporter, public IStreamingExporter
{
public:
    explicit NdjsonExporter(std::ostream& output, std::size_t max_attempts = 1,
        std::size_t initial_delay_ms = 0, std::size_t max_delay_ms = 0)
        : output_(output)
        , max_attempts_(std::max<std::size_t>(1, max_attempts))
        , initial_delay_ms_(initial_delay_ms)
        , max_delay_ms_(max_delay_ms)
    {
    }

    Result<void> export_chunks(std::span<const ChunkResult> chunks,
        const ExportMetadata& metadata) override
    {
        for (std::size_t index = 0; index < chunks.size(); ++index)
        {
            const auto& chunk = chunks[index];
            if (!write_with_retry(chunk, metadata))
                return failure(index, chunk, metadata);
        }
        return Result<void>();
    }

    Result<void>
    export_stream(IChunkStream& stream, const ExportMetadata& metadata,
        const CancellationToken* cancellation = nullptr) override
    {
        ChunkView view;
        std::size_t index = 0;
        while (true)
        {
            if (cancellation && cancellation->is_cancelled())
                return cancelled();
            auto next = stream.next(view);
            if (!next.has_value())
                return Result<void>(*next.error(),
                    std::vector<Diagnostic>(next.diagnostics().begin(),
                        next.diagnostics().end()));
            if (!next.value())
                break;
            ChunkResult chunk {
                .text = std::string(view.text),
                .tokens = std::vector<std::uint32_t>(view.tokens.begin(),
                    view.tokens.end()),
                .start_byte = view.start_byte,
                .end_byte = view.end_byte,
                .doc_id = std::string(view.doc_id),
                .chunk_id = std::string(view.chunk_id),
                .record_id = std::string(view.record_id),
                .tokenizer_name = std::string(view.tokenizer_name),
                .tokenizer_configuration = std::string(view.tokenizer_configuration),
                .source_start_byte = view.source_start_byte,
                .source_end_byte = view.source_end_byte,
                .source_offset_kind = view.source_offset_kind,
                .has_source_offsets = view.has_source_offsets,
                .chunk_index = view.chunk_index,
                .metadata = std::string(view.metadata)
            };
            if (!write_with_retry(chunk, metadata))
                return failure(index, chunk, metadata);
            ++index;
        }
        return Result<void>();
    }

private:
    static std::string escape(std::string_view value)
    {
        std::string result { "\"" };
        for (const char character : value)
        {
            switch (character)
            {
            case '\\':
                result += "\\\\";
                break;
            case '"':
                result += "\\\"";
                break;
            case '\n':
                result += "\\n";
                break;
            case '\r':
                result += "\\r";
                break;
            case '\t':
                result += "\\t";
                break;
            default:
                result += character;
                break;
            }
        }
        result += '"';
        return result;
    }

    static std::string json_object(std::string_view value)
    {
        return value.empty() ? "{}" : std::string(value);
    }

    bool write_chunk(const ChunkResult& chunk, const ExportMetadata& metadata)
    {
        output_ << "{\"schema_version\":" << constants::export_schema::version
                << ",\"doc_id\":"
                << escape(chunk.doc_id.empty() ? metadata.doc_id : chunk.doc_id)
                << ",\"chunk_id\":" << escape(chunk.chunk_id)
                << ",\"record_id\":" << escape(chunk.record_id)
                << ",\"text\":" << escape(chunk.text) << ",\"tokens\":[";
        for (std::size_t index = 0; index < chunk.tokens.size(); ++index)
        {
            if (index != 0)
                output_ << ',';
            output_ << chunk.tokens[index];
        }
        output_ << "]" << ",\"start_byte\":" << chunk.start_byte
                << ",\"end_byte\":" << chunk.end_byte << ",\"source_start_byte\":";
        if (chunk.has_source_offsets)
            output_ << chunk.source_start_byte;
        else
            output_ << "null";
        output_ << ",\"source_end_byte\":";
        if (chunk.has_source_offsets)
            output_ << chunk.source_end_byte;
        else
            output_ << "null";
        output_ << ",\"source_offset_kind\":"
                << escape(source_kind(chunk.source_offset_kind))
                << ",\"has_source_offsets\":"
                << (chunk.has_source_offsets ? "true" : "false")
                << ",\"chunk_index\":" << chunk.chunk_index
                << ",\"metadata\":" << json_object(chunk.metadata)
                << ",\"tokenizer_name\":"
                << escape(chunk.tokenizer_name.empty() ? metadata.tokenizer_name
                                                       : chunk.tokenizer_name)
                << ",\"tokenizer_configuration\":"
                << json_object(chunk.tokenizer_configuration.empty()
                           ? metadata.tokenizer_configuration
                           : chunk.tokenizer_configuration)
                << ",\"effective_configuration\":"
                << json_object(metadata.effective_configuration) << "}\n";
        return static_cast<bool>(output_);
    }

    bool write_with_retry(const ChunkResult& chunk, const ExportMetadata& metadata)
    {
        std::size_t delay = initial_delay_ms_;
        for (std::size_t attempt = 0; attempt < max_attempts_; ++attempt)
        {
            if (write_chunk(chunk, metadata))
                return true;
            output_.clear();
            if (attempt + 1 < max_attempts_ && delay != 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                delay = max_delay_ms_ == 0 ? delay : std::min(max_delay_ms_, delay * 2);
            }
        }
        return false;
    }

    static const char* source_kind(ChunkResult::SourceOffsetKind kind)
    {
        switch (kind)
        {
        case ChunkResult::SourceOffsetKind::exact:
            return constants::export_schema::exact.data();
        case ChunkResult::SourceOffsetKind::record:
            return constants::export_schema::record.data();
        default:
            return constants::export_schema::unavailable.data();
        }
    }

    Result<void> failure(std::size_t index, const ChunkResult& chunk,
        const ExportMetadata& metadata) const
    {
        const std::string doc_id = chunk.doc_id.empty() ? metadata.doc_id : chunk.doc_id;
        const std::string details = "[{\"record_index\":" + std::to_string(index)
            + ",\"doc_id\":" + escape(doc_id)
            + ",\"chunk_id\":" + escape(chunk.chunk_id)
            + ",\"code\":1400,\"message\":\"Failed to write NDJSON output.\"}]";
        return Result<void>(Error {
            .code = static_cast<std::uint32_t>(ErrorCode::export_failed),
            .name = std::string(constants::error_name::export_failed),
            .description = "An exporter could not write or deliver the requested output.",
            .message = "Failed to write NDJSON output.",
            .path = metadata.source_path,
            .details_json = details });
    }

    Result<void> cancelled() const
    {
        return Result<void>(Error {
            .code = static_cast<std::uint32_t>(ErrorCode::cancelled),
            .name = std::string(constants::error_name::cancelled),
            .description = "The caller requested cancellation before the operation completed.",
            .message = "NDJSON export cancelled." });
    }

    std::ostream& output_;
    std::size_t max_attempts_;
    std::size_t initial_delay_ms_;
    std::size_t max_delay_ms_;
};

} // namespace fastchunk