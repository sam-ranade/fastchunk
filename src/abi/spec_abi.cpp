#include "fastchunk/constants.h"
#include "fastchunk/core.h"
#include "fastchunk/factory.h"
#include "fastchunk/tokenizers.h"
#include "fastchunk_abi.h"
#include "ndjson_parser.h"
#include "simd_utf8.h"

#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

struct fastchunk_error
{
    fastchunk::Error value;
};

struct fastchunk_reader
{
    std::unique_ptr<fastchunk::IReader> reader { fastchunk::create_mmap_reader() };
    fastchunk::FileBuffer buffer;
    std::vector<std::byte> owned_buffer;
    std::vector<fastchunk::NdjsonRecord> records;
    std::vector<fastchunk::Diagnostic> diagnostics;
    std::optional<fastchunk::Error> error;
    fastchunk::InputOptions options;
    bool active { false };
};

struct fastchunk_tokenizer
{
    std::shared_ptr<fastchunk::ITokenizer> tokenizer;
    std::string name;
    std::string configuration;
    std::vector<fastchunk::Diagnostic> diagnostics;
    std::optional<fastchunk::Error> error;
};

struct fastchunk_chunker
{
    std::unique_ptr<fastchunk::IChunker> chunker { fastchunk::create_chunker() };
    std::shared_ptr<fastchunk::ITokenizer> tokenizer;
    std::string tokenizer_name;
    std::string tokenizer_configuration;
    fastchunk::ChunkOptions options;
    std::vector<fastchunk::Diagnostic> diagnostics;
    std::optional<fastchunk::Error> error;
};

struct fastchunk_result
{
    std::vector<fastchunk::ChunkResult> chunks;
    std::vector<fastchunk::Diagnostic> diagnostics;
    std::optional<fastchunk::Error> error;
};

struct fastchunk_stream
{
    std::unique_ptr<fastchunk::IChunkStream> stream;
    std::vector<std::unique_ptr<fastchunk::IChunkStream>> record_streams;
    std::size_t record_index { 0 };
    fastchunk::ChunkView view;
    std::vector<fastchunk::Diagnostic> diagnostics;
    std::optional<fastchunk::Error> error;
    bool cancelled { false };
};

struct fastchunk_cancellation
{
    fastchunk::CancellationToken token;
};

namespace
{

constexpr fastchunk_status_t k_ok = 0;

fastchunk_status_t set_error(std::optional<fastchunk::Error>& slot,
    const fastchunk::Error& error)
{
    slot = error;
    return error.code;
}

fastchunk_status_t invalid(std::optional<fastchunk::Error>& slot,
    const char* message)
{
    return set_error(
        slot, fastchunk::Error { .code = static_cast<std::uint32_t>(fastchunk::ErrorCode::invalid_argument), .name = std::string(fastchunk::constants::error_name::invalid_argument), .description = "A public argument is missing, malformed, or out of range.", .message = message });
}

void clear(std::optional<fastchunk::Error>& error) { error.reset(); }

fastchunk_bytes_t bytes(std::string_view value)
{
    return { reinterpret_cast<const unsigned char*>(value.data()), value.size() };
}

fastchunk_bytes_t bytes(const std::string& value)
{
    return bytes(std::string_view(value));
}

void copy_error(const fastchunk::Error& source,
    fastchunk_error_view_t& output)
{
    output = {};
    output.has_error = 1;
    output.code = source.code;
    output.name = bytes(source.name);
    output.description = bytes(source.description);
    output.message = bytes(source.message);
    output.dependency = bytes(source.dependency);
    output.path = bytes(source.path);
    output.details_json = bytes(source.details_json);
    if (source.byte_offset)
    {
        output.has_byte_offset = 1;
        output.byte_offset = *source.byte_offset;
    }
}

void copy_diagnostic(const fastchunk::Diagnostic& source,
    fastchunk_diagnostic_view_t& output)
{
    output = {};
    output.severity = static_cast<std::uint32_t>(source.severity);
    output.code = source.code;
    output.name = bytes(source.name);
    output.description = bytes(source.description);
    output.message = bytes(source.message);
    if (source.source_byte_offset)
    {
        output.has_source_byte_offset = 1;
        output.source_byte_offset = *source.source_byte_offset;
    }
}

fastchunk_status_t
diagnostic_at(const std::vector<fastchunk::Diagnostic>& diagnostics,
    size_t index, fastchunk_diagnostic_view_t* output)
{
    if (!output)
        return static_cast<fastchunk_status_t>(
            fastchunk::ErrorCode::invalid_argument);
    if (index >= diagnostics.size())
        return static_cast<fastchunk_status_t>(
            fastchunk::ErrorCode::invalid_argument);
    copy_diagnostic(diagnostics[index], *output);
    return k_ok;
}

fastchunk_status_t error_at(const std::optional<fastchunk::Error>& error,
    fastchunk_error_view_t* output)
{
    if (!output)
        return static_cast<fastchunk_status_t>(
            fastchunk::ErrorCode::invalid_argument);
    *output = {};
    if (error)
        copy_error(*error, *output);
    return k_ok;
}

fastchunk_status_t
validate_reader_options(const fastchunk_reader_options_t& options,
    std::optional<fastchunk::Error>& error)
{
    if (options.input_mode > FASTCHUNK_INPUT_COPY || options.invalid_utf8_policy > FASTCHUNK_UTF8_SKIP || options.record_mode > FASTCHUNK_RECORD_NDJSON || options.malformed_record_policy > FASTCHUNK_RECORD_SKIP)
    {
        return invalid(error, "Reader options contain an unknown enum value.");
    }
    if (options.input_mode == FASTCHUNK_INPUT_ZERO_COPY && options.invalid_utf8_policy != FASTCHUNK_UTF8_ERROR)
    {
        return set_error(
            error,
            fastchunk::Error {
                .code = static_cast<std::uint32_t>(
                    fastchunk::ErrorCode::invalid_configuration),
                .name = std::string(fastchunk::constants::error_name::invalid_configuration),
                .description = "Configuration values conflict or violate a documented rule.",
                .message = "zero_copy mode only supports invalid_utf8=error." });
    }
    if (options.record_mode != FASTCHUNK_RECORD_NONE)
    {
        if (options.record_mode != FASTCHUNK_RECORD_NDJSON)
        {
            return invalid(error, "Reader options contain an unknown record mode.");
        }
    }
    return k_ok;
}

fastchunk::InputOptions
to_cpp_options(const fastchunk_reader_options_t& options)
{
    fastchunk::InputOptions result;
    result.mode = static_cast<fastchunk::InputMode>(options.input_mode);
    result.invalid_utf8 = static_cast<fastchunk::InvalidUtf8Policy>(options.invalid_utf8_policy);
    result.record_mode = static_cast<fastchunk::InputOptions::RecordMode>(options.record_mode);
    result.malformed_record = static_cast<fastchunk::InputOptions::MalformedRecordPolicy>(
        options.malformed_record_policy);
    if (options.record_id_field.data)
    {
        result.record_id_field.assign(
            reinterpret_cast<const char*>(options.record_id_field.data),
            options.record_id_field.size);
    }
    return result;
}

void store_diagnostics(std::vector<fastchunk::Diagnostic>& destination,
    std::span<const fastchunk::Diagnostic> source)
{
    destination.assign(source.begin(), source.end());
}

fastchunk_status_t parse_records(fastchunk_reader& reader,
    std::optional<fastchunk::Error>& error)
{
    reader.records.clear();
    if (reader.options.record_mode != fastchunk::InputOptions::RecordMode::ndjson)
    {
        return k_ok;
    }
    auto parsed = fastchunk::NdjsonParser::parse(reader.buffer.data,
        reader.options.record_id_field,
        reader.options.malformed_record);
    if (!parsed.has_value())
    {
        return set_error(error, *parsed.error());
    }
    reader.records = std::move(parsed).value();
    store_diagnostics(reader.diagnostics, parsed.diagnostics());
    return k_ok;
}

std::vector<std::byte>
repair_utf8(std::span<const std::byte> input,
    fastchunk::InvalidUtf8Policy policy,
    std::vector<fastchunk::Diagnostic>& diagnostics)
{
    std::vector<std::byte> output;
    output.reserve(input.size());
    const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
    constexpr unsigned char replacement[] = { 0xEF, 0xBF, 0xBD };
    for (std::size_t i = 0; i < input.size();)
    {
        std::size_t length = 0;
        if (bytes[i] <= 0x7F)
            length = 1;
        else if ((bytes[i] & 0xE0) == 0xC0 && i + 1 < input.size() && bytes[i] >= 0xC2 && (bytes[i + 1] & 0xC0) == 0x80)
            length = 2;
        else if ((bytes[i] & 0xF0) == 0xE0 && i + 2 < input.size() && (bytes[i + 1] & 0xC0) == 0x80 && (bytes[i + 2] & 0xC0) == 0x80 && !(bytes[i] == 0xED && bytes[i + 1] >= 0xA0))
            length = 3;
        else if ((bytes[i] & 0xF8) == 0xF0 && i + 3 < input.size() && bytes[i] <= 0xF4 && (bytes[i + 1] & 0xC0) == 0x80 && (bytes[i + 2] & 0xC0) == 0x80 && (bytes[i + 3] & 0xC0) == 0x80 && !(bytes[i] == 0xF4 && bytes[i + 1] > 0x8F))
            length = 4;
        if (length != 0)
        {
            output.insert(output.end(),
                input.begin() + static_cast<std::ptrdiff_t>(i),
                input.begin() + static_cast<std::ptrdiff_t>(i + length));
            i += length;
            continue;
        }
        diagnostics.push_back(fastchunk::Diagnostic {
            .severity = fastchunk::DiagnosticSeverity::warning,
            .code = static_cast<std::uint32_t>(
                policy == fastchunk::InvalidUtf8Policy::replace
                    ? fastchunk::DiagnosticCode::invalid_utf8_replaced
                    : fastchunk::DiagnosticCode::invalid_utf8_skipped),
            .name = policy == fastchunk::InvalidUtf8Policy::replace
                ? "invalid_utf8_replaced"
                : "invalid_utf8_skipped",
            .description = "Malformed UTF-8 was repaired according to the configured policy.",
            .message = "Malformed UTF-8 byte skipped or replaced in input buffer.",
            .source_byte_offset = i });
        if (policy == fastchunk::InvalidUtf8Policy::replace)
        {
            output.insert(output.end(),
                reinterpret_cast<const std::byte*>(replacement),
                reinterpret_cast<const std::byte*>(replacement + 3));
        }
        ++i;
    }
    return output;
}

} // namespace

extern "C"
{

    uint32_t fastchunk_abi_version(void) { return 1; }

    fastchunk_status_t fastchunk_reader_create(fastchunk_reader_t** output,
        fastchunk_error_t** error)
    {
        if (error)
            *error = nullptr;
        if (!output)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        *output = new (std::nothrow) fastchunk_reader();
        if (!*output)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::resource_limit_exceeded);
        return k_ok;
    }

    fastchunk_status_t
    fastchunk_reader_open_path(fastchunk_reader_t* reader,
        const fastchunk_bytes_t* path,
        const fastchunk_reader_options_t* options)
    {
        if (!reader)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        if (!path || !options || !path->data || path->size == 0)
            return invalid(reader->error, "Reader path and options are required.");
        clear(reader->error);
        if (reader->active)
            return invalid(reader->error, "Reader already has an active input.");
        auto status = validate_reader_options(*options, reader->error);
        if (status != k_ok)
            return status;
        std::filesystem::path file_path(
            std::string(reinterpret_cast<const char*>(path->data), path->size));
        auto result = reader->reader->open(file_path, to_cpp_options(*options));
        if (!result.has_value())
            return set_error(reader->error, *result.error());
        reader->buffer = result.value();
        store_diagnostics(reader->diagnostics, result.diagnostics());
        reader->options = to_cpp_options(*options);
        status = parse_records(*reader, reader->error);
        if (status != k_ok)
            return status;
        reader->active = true;
        return k_ok;
    }

    fastchunk_status_t
    fastchunk_reader_open_buffer(fastchunk_reader_t* reader,
        const fastchunk_bytes_t* buffer,
        const fastchunk_reader_options_t* options)
    {
        if (!reader || !buffer || !options || (!buffer->data && buffer->size != 0))
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        clear(reader->error);
        if (reader->active)
            return invalid(reader->error, "Reader already has an active input.");
        auto status = validate_reader_options(*options, reader->error);
        if (status != k_ok)
            return status;
        reader->owned_buffer.clear();
        std::span<const std::byte> input(
            reinterpret_cast<const std::byte*>(buffer->data), buffer->size);
        if (options->input_mode == FASTCHUNK_INPUT_COPY && options->invalid_utf8_policy != FASTCHUNK_UTF8_ERROR)
        {
            reader->owned_buffer = repair_utf8(
                input,
                static_cast<fastchunk::InvalidUtf8Policy>(options->invalid_utf8_policy),
                reader->diagnostics);
            reader->buffer.data = std::span<const std::byte>(
                reader->owned_buffer.data(), reader->owned_buffer.size());
        }
        else if (options->input_mode == FASTCHUNK_INPUT_COPY)
        {
            reader->owned_buffer.assign(input.begin(), input.end());
            reader->buffer.data = std::span<const std::byte>(
                reader->owned_buffer.data(), reader->owned_buffer.size());
        }
        else
        {
            reader->buffer.data = std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(buffer->data), buffer->size);
        }
        std::size_t offset = 0;
        if (options->invalid_utf8_policy == FASTCHUNK_UTF8_ERROR && !fastchunk::SimdUtf8Validator::validate(reader->buffer.data.data(), reader->buffer.data.size(), offset))
        {
            reader->buffer = {};
            return set_error(
                reader->error,
                fastchunk::Error {
                    .code = static_cast<std::uint32_t>(fastchunk::ErrorCode::invalid_utf8),
                    .name = "invalid_utf8",
                    .description = "Input contains malformed UTF-8; the byte offset "
                                   "identifies the first error when available.",
                    .message = "Malformed UTF-8 sequence found in input buffer.",
                    .byte_offset = offset });
        }
        reader->buffer.file_size = reader->buffer.data.size();
        reader->diagnostics.clear();
        reader->options = to_cpp_options(*options);
        status = parse_records(*reader, reader->error);
        if (status != k_ok)
            return status;
        reader->active = true;
        return k_ok;
    }

    fastchunk_status_t fastchunk_reader_close(fastchunk_reader_t* reader)
    {
        if (!reader)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        clear(reader->error);
        auto result = reader->reader->close();
        reader->buffer = {};
        reader->owned_buffer.clear();
        reader->diagnostics.clear();
        reader->records.clear();
        reader->active = false;
        return result.has_value() ? k_ok : set_error(reader->error, *result.error());
    }

    void fastchunk_reader_destroy(fastchunk_reader_t* reader) { delete reader; }

    fastchunk_status_t fastchunk_tokenizer_create(
        const fastchunk_bytes_t* name, const fastchunk_bytes_t* configuration,
        fastchunk_tokenizer_t** output, fastchunk_error_t** error)
    {
        if (error)
            *error = nullptr;
        if (!name || !output || !name->data || name->size == 0)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        std::string tokenizer_name(reinterpret_cast<const char*>(name->data),
            name->size);
        std::string configuration_value;
        if (configuration && configuration->data)
            configuration_value.assign(
                reinterpret_cast<const char*>(configuration->data),
                configuration->size);
        auto result = fastchunk::create_tokenizer_from_name(tokenizer_name,
            configuration_value);
        if (!result.has_value())
            return result.error()->code;
        auto handle = new (std::nothrow) fastchunk_tokenizer();
        if (!handle)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::resource_limit_exceeded);
        handle->tokenizer = std::shared_ptr<fastchunk::ITokenizer>(std::move(result).value());
        handle->name = std::move(tokenizer_name);
        handle->configuration = std::move(configuration_value);
        *output = handle;
        return k_ok;
    }

    void fastchunk_tokenizer_destroy(fastchunk_tokenizer_t* tokenizer)
    {
        delete tokenizer;
    }

    fastchunk_status_t
    fastchunk_chunker_create(const fastchunk_chunker_options_t* options,
        fastchunk_tokenizer_t* tokenizer,
        fastchunk_chunker_t** output,
        fastchunk_error_t** error)
    {
        if (error)
            *error = nullptr;
        if (!options || !tokenizer || !output)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        if (options->max_tokens == 0 || options->overlap_tokens >= options->max_tokens)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_configuration);
        auto handle = new (std::nothrow) fastchunk_chunker();
        if (!handle)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::resource_limit_exceeded);
        handle->options.max_tokens = options->max_tokens;
        handle->options.overlap_tokens = options->overlap_tokens;
        handle->options.metadata_collision = static_cast<fastchunk::ChunkOptions::MetadataCollisionPolicy>(
            options->metadata_collision_policy);
        handle->tokenizer = tokenizer->tokenizer;
        handle->tokenizer_name = tokenizer->name;
        handle->tokenizer_configuration = tokenizer->configuration;
        *output = handle;
        return k_ok;
    }

    fastchunk_status_t fastchunk_chunker_run(fastchunk_chunker_t* chunker,
        fastchunk_reader_t* reader,
        fastchunk_cancellation_t* cancellation,
        fastchunk_result_t** output)
    {
        if (!chunker || !reader || !reader->active || !output)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        clear(chunker->error);
        const auto* token = cancellation ? &cancellation->token : nullptr;
        fastchunk::ChunkInputContext context;
        context.tokenizer_name = chunker->tokenizer_name;
        context.tokenizer_configuration = chunker->tokenizer_configuration;
        auto owned = new (std::nothrow) fastchunk_result();
        if (!owned)
            return set_error(
                chunker->error,
                fastchunk::Error {
                    .code = static_cast<std::uint32_t>(
                        fastchunk::ErrorCode::resource_limit_exceeded),
                    .name = "resource_limit_exceeded",
                    .description = "A configured memory, size, or processing limit was exceeded.",
                    .message = "Unable to allocate result." });
        if (reader->options.record_mode == fastchunk::InputOptions::RecordMode::ndjson)
        {
            for (const auto& record : reader->records)
            {
                auto record_buffer = reader->buffer.data.subspan(record.start_byte,
                    record.line_text.size());
                context.record_id = record.record_id;
                context.source_start_byte = record.start_byte;
                context.source_end_byte = record.end_byte;
                context.source_offset_kind = fastchunk::ChunkResult::SourceOffsetKind::record;
                auto result = chunker->chunker->chunk_buffer(
                    record_buffer, chunker->options, *chunker->tokenizer, context, token);
                if (!result.has_value())
                {
                    owned->error = *result.error();
                    *output = owned;
                    return owned->error->code;
                }
                owned->chunks.insert(owned->chunks.end(), result.value().begin(),
                    result.value().end());
                auto diagnostics = result.diagnostics();
                owned->diagnostics.insert(owned->diagnostics.end(), diagnostics.begin(),
                    diagnostics.end());
            }
        }
        else
        {
            auto result = chunker->chunker->chunk_buffer(reader->buffer.data, chunker->options,
                *chunker->tokenizer, context, token);
            if (!result.has_value())
            {
                owned->error = *result.error();
                *output = owned;
                return owned->error->code;
            }
            owned->chunks = std::move(result).value();
            store_diagnostics(owned->diagnostics, result.diagnostics());
        }
        *output = owned;
        return k_ok;
    }

    fastchunk_status_t fastchunk_chunker_stream(
        fastchunk_chunker_t* chunker, fastchunk_reader_t* reader,
        fastchunk_cancellation_t* cancellation, fastchunk_stream_t** output)
    {
        if (!chunker || !reader || !reader->active || !output)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        auto stream = new (std::nothrow) fastchunk_stream();
        if (!stream)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::resource_limit_exceeded);
        if (reader->options.record_mode == fastchunk::InputOptions::RecordMode::ndjson)
        {
            for (const auto& record : reader->records)
            {
                fastchunk::ChunkInputContext context;
                context.record_id = record.record_id;
                context.source_start_byte = record.start_byte;
                context.source_end_byte = record.end_byte;
                context.source_offset_kind = fastchunk::ChunkResult::SourceOffsetKind::record;
                context.tokenizer_name = chunker->tokenizer_name;
                context.tokenizer_configuration = chunker->tokenizer_configuration;
                auto record_buffer = reader->buffer.data.subspan(record.start_byte,
                    record.line_text.size());
                auto result = chunker->chunker->stream_buffer(
                    record_buffer, chunker->options, *chunker->tokenizer, context,
                    cancellation ? &cancellation->token : nullptr);
                if (!result.has_value())
                {
                    delete stream;
                    return set_error(chunker->error, *result.error());
                }
                stream->record_streams.push_back(std::move(result).value());
            }
        }
        else
        {
            fastchunk::ChunkInputContext context;
            context.tokenizer_name = chunker->tokenizer_name;
            context.tokenizer_configuration = chunker->tokenizer_configuration;
            auto result = chunker->chunker->stream_buffer(
                reader->buffer.data, chunker->options, *chunker->tokenizer, context,
                cancellation ? &cancellation->token : nullptr);
            if (!result.has_value())
            {
                delete stream;
                return set_error(chunker->error, *result.error());
            }
            stream->stream = std::move(result).value();
        }
        *output = stream;
        return k_ok;
    }

    void fastchunk_chunker_destroy(fastchunk_chunker_t* chunker) { delete chunker; }

    fastchunk_status_t
    fastchunk_cancellation_create(fastchunk_cancellation_t** output,
        fastchunk_error_t** error)
    {
        if (error)
            *error = nullptr;
        if (!output)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        *output = new (std::nothrow) fastchunk_cancellation();
        return *output ? k_ok
                       : static_cast<fastchunk_status_t>(
                             fastchunk::ErrorCode::resource_limit_exceeded);
    }
    void fastchunk_cancellation_request(fastchunk_cancellation_t* cancellation)
    {
        if (cancellation)
            cancellation->token.request_cancel();
    }
    void fastchunk_cancellation_destroy(fastchunk_cancellation_t* cancellation)
    {
        delete cancellation;
    }

    fastchunk_status_t fastchunk_stream_next(fastchunk_stream_t* stream,
        uint8_t* has_chunk,
        fastchunk_chunk_view_t* output)
    {
        if (!stream || !has_chunk || !output)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        fastchunk::Result<bool> result(false);
        while (true)
        {
            auto* active_stream = stream->stream.get();
            if (!active_stream && stream->record_index < stream->record_streams.size())
            {
                active_stream = stream->record_streams[stream->record_index].get();
            }
            if (!active_stream)
            {
                *has_chunk = 0;
                *output = {};
                return k_ok;
            }
            result = active_stream->next(stream->view);
            if (!result.has_value())
                return set_error(stream->error, *result.error());
            const auto diagnostics = result.diagnostics();
            stream->diagnostics.insert(stream->diagnostics.end(), diagnostics.begin(),
                diagnostics.end());
            if (result.value())
                break;
            if (stream->stream)
            {
                stream->stream.reset();
            }
            else
            {
                ++stream->record_index;
            }
        }
        *has_chunk = 1;
        *output = {};
        if (!*has_chunk)
            return k_ok;
        output->text = bytes(stream->view.text);
        output->token_ids = stream->view.tokens.data();
        output->token_count = stream->view.tokens.size();
        output->start_byte = stream->view.start_byte;
        output->end_byte = stream->view.end_byte;
        output->doc_id = bytes(stream->view.doc_id);
        output->chunk_id = bytes(stream->view.chunk_id);
        output->record_id = bytes(stream->view.record_id);
        output->metadata = bytes(stream->view.metadata);
        output->tokenizer_name = bytes(stream->view.tokenizer_name);
        output->tokenizer_configuration = bytes(stream->view.tokenizer_configuration);
        output->source_start_byte = stream->view.source_start_byte;
        output->source_end_byte = stream->view.source_end_byte;
        output->source_offset_kind = static_cast<uint32_t>(stream->view.source_offset_kind);
        output->has_source_offsets = stream->view.has_source_offsets ? 1 : 0;
        output->chunk_index = stream->view.chunk_index;
        output->has_chunk = 1;
        return k_ok;
    }
    fastchunk_status_t fastchunk_stream_cancel(fastchunk_stream_t* stream)
    {
        if (!stream)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        if (stream->stream)
            stream->stream->cancel();
        for (auto& record_stream : stream->record_streams)
        {
            if (record_stream)
                record_stream->cancel();
        }
        stream->cancelled = true;
        return k_ok;
    }
    fastchunk_status_t
    fastchunk_stream_is_cancelled(const fastchunk_stream_t* stream,
        uint8_t* cancelled)
    {
        if (!stream || !cancelled)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        *cancelled = stream->cancelled ? 1 : 0;
        return k_ok;
    }
    void fastchunk_stream_release(fastchunk_stream_t* stream) { delete stream; }
    void fastchunk_result_release(fastchunk_result_t* result) { delete result; }
    void fastchunk_error_release(fastchunk_error_t* error) { delete error; }

    fastchunk_status_t
    fastchunk_result_chunk_count(const fastchunk_result_t* result, size_t* count)
    {
        if (!result || !count)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        *count = result->chunks.size();
        return result->error ? result->error->code : k_ok;
    }
    fastchunk_status_t fastchunk_result_get_chunk(const fastchunk_result_t* result,
        size_t index,
        fastchunk_chunk_view_t* output)
    {
        if (!result || !output || index >= result->chunks.size())
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        const auto& chunk = result->chunks[index];
        *output = {};
        output->text = bytes(chunk.text);
        output->token_ids = chunk.tokens.data();
        output->token_count = chunk.tokens.size();
        output->start_byte = chunk.start_byte;
        output->end_byte = chunk.end_byte;
        output->doc_id = bytes(chunk.doc_id);
        output->chunk_id = bytes(chunk.chunk_id);
        output->record_id = bytes(chunk.record_id);
        output->metadata = bytes(chunk.metadata);
        output->tokenizer_name = bytes(chunk.tokenizer_name);
        output->tokenizer_configuration = bytes(chunk.tokenizer_configuration);
        output->source_start_byte = chunk.source_start_byte;
        output->source_end_byte = chunk.source_end_byte;
        output->source_offset_kind = static_cast<uint32_t>(chunk.source_offset_kind);
        output->has_source_offsets = chunk.has_source_offsets ? 1 : 0;
        output->chunk_index = chunk.chunk_index;
        output->has_chunk = 1;
        return k_ok;
    }
    fastchunk_status_t fastchunk_error_get(const fastchunk_error_t* error,
        fastchunk_error_view_t* output)
    {
        if (!error || !output)
            return static_cast<fastchunk_status_t>(
                fastchunk::ErrorCode::invalid_argument);
        copy_error(error->value, *output);
        return k_ok;
    }

#define HANDLE_ERROR_ACCESSORS(prefix, type)                         \
    fastchunk_status_t fastchunk_##prefix##_get_error(               \
        const type* handle, fastchunk_error_view_t* output)          \
    {                                                                \
        return handle ? error_at(handle->error, output)              \
                      : static_cast<fastchunk_status_t>(             \
                            fastchunk::ErrorCode::invalid_argument); \
    }                                                                \
    void fastchunk_##prefix##_clear_error(type* handle)              \
    {                                                                \
        if (handle)                                                  \
            handle->error.reset();                                   \
    }
    HANDLE_ERROR_ACCESSORS(reader, fastchunk_reader_t)
    HANDLE_ERROR_ACCESSORS(tokenizer, fastchunk_tokenizer_t)
    HANDLE_ERROR_ACCESSORS(chunker, fastchunk_chunker_t)
    HANDLE_ERROR_ACCESSORS(stream, fastchunk_stream_t)
    fastchunk_status_t fastchunk_result_get_error(const fastchunk_result_t* handle,
        fastchunk_error_view_t* output)
    {
        return handle ? error_at(handle->error, output)
                      : static_cast<fastchunk_status_t>(
                            fastchunk::ErrorCode::invalid_argument);
    }

#define DIAGNOSTIC_ACCESSORS(prefix, type)                                       \
    fastchunk_status_t fastchunk_##prefix##_diagnostic_count(const type* handle, \
        size_t* count)                                                           \
    {                                                                            \
        if (!handle || !count)                                                   \
            return static_cast<fastchunk_status_t>(                              \
                fastchunk::ErrorCode::invalid_argument);                         \
        *count = handle->diagnostics.size();                                     \
        return k_ok;                                                             \
    }                                                                            \
    fastchunk_status_t fastchunk_##prefix##_diagnostic_get(                      \
        const type* handle, size_t index, fastchunk_diagnostic_view_t* output)   \
    {                                                                            \
        return handle ? diagnostic_at(handle->diagnostics, index, output)        \
                      : static_cast<fastchunk_status_t>(                         \
                            fastchunk::ErrorCode::invalid_argument);             \
    }
    DIAGNOSTIC_ACCESSORS(reader, fastchunk_reader_t)
    DIAGNOSTIC_ACCESSORS(tokenizer, fastchunk_tokenizer_t)
    DIAGNOSTIC_ACCESSORS(chunker, fastchunk_chunker_t)
    DIAGNOSTIC_ACCESSORS(stream, fastchunk_stream_t)
    DIAGNOSTIC_ACCESSORS(result, fastchunk_result_t)

} // extern "C"
