#include "fastchunk/constants.h"
#include "fastchunk/core.h"
#include "fastchunk/factory.h"
#include "fastchunk/fastchunk_c.h"
#include "fastchunk/tokenizers.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

// Helper to copy C++ Error into C fastchunk_error_t struct safely
void populate_c_error(const fastchunk::Error& cpp_err,
    fastchunk_error_t* out_error)
{
    if (!out_error)
        return;
    out_error->code = cpp_err.code;
    out_error->byte_offset = cpp_err.byte_offset.value_or(0);

    std::strncpy(out_error->name, cpp_err.name.c_str(),
        sizeof(out_error->name) - 1);
    out_error->name[sizeof(out_error->name) - 1] = '\0';

    std::strncpy(out_error->description, cpp_err.description.c_str(),
        sizeof(out_error->description) - 1);
    out_error->description[sizeof(out_error->description) - 1] = '\0';

    std::strncpy(out_error->message, cpp_err.message.c_str(),
        sizeof(out_error->message) - 1);
    out_error->message[sizeof(out_error->message) - 1] = '\0';

    std::strncpy(out_error->path, cpp_err.path.c_str(),
        sizeof(out_error->path) - 1);
    out_error->path[sizeof(out_error->path) - 1] = '\0';
}

// Wrapper types to bridge C++ objects through opaque void handles
struct CppReaderHandle
{
    std::unique_ptr<fastchunk::IReader> reader;
    std::vector<fastchunk_diagnostic_t> c_diagnostics;
};

struct CppTokenizerHandle
{
    std::shared_ptr<fastchunk::ITokenizer> tokenizer;
};

struct CppChunkerHandle
{
    std::unique_ptr<fastchunk::IChunker> chunker;
};

// Internal container to manage memory backing for allocated C chunk arrays
struct CppChunkArrayAllocation
{
    std::vector<fastchunk::ChunkResult> raw_chunks;
    std::vector<fastchunk_chunk_view_t> c_chunk_views;
};

} // namespace

extern "C"
{

    // ============================================================================
    // Reader API Implementation
    // ============================================================================

    fastchunk_reader_t* fastchunk_reader_create_mmap(void)
    {
        auto handle = new (std::nothrow) CppReaderHandle();
        if (!handle)
            return nullptr;
        handle->reader = fastchunk::create_mmap_reader();
        return reinterpret_cast<fastchunk_reader_t*>(handle);
    }

    bool fastchunk_reader_open(fastchunk_reader_t* reader, const char* filepath,
        const fastchunk_input_options_t* options,
        fastchunk_file_buffer_t* out_buffer,
        fastchunk_error_t* out_error)
    {

        if (!reader || !filepath || !options || !out_buffer)
        {
            if (out_error)
            {
                out_error->code = static_cast<uint32_t>(fastchunk::ErrorCode::invalid_argument);
                std::strncpy(out_error->message,
                    "Null pointer passed to fastchunk_reader_open",
                    sizeof(out_error->message) - 1);
            }
            return false;
        }

        auto* cpp_handle = reinterpret_cast<CppReaderHandle*>(reader);

        fastchunk::InputOptions in_opts {
            .mode = static_cast<fastchunk::InputMode>(options->mode),
            .invalid_utf8 = static_cast<fastchunk::InvalidUtf8Policy>(options->invalid_utf8)
        };

        auto res = cpp_handle->reader->open(filepath, in_opts);
        if (!res.has_value())
        {
            populate_c_error(*res.error(), out_error);
            return false;
        }

        const auto& buf = res.value();
        cpp_handle->c_diagnostics.clear();
        cpp_handle->c_diagnostics.reserve(buf.diagnostics.size());

        for (const auto& diag : buf.diagnostics)
        {
            fastchunk_diagnostic_t c_diag {};
            c_diag.severity = static_cast<fastchunk_diagnostic_severity_t>(diag.severity);
            c_diag.code = diag.code;
            c_diag.byte_offset = diag.source_byte_offset.value_or(0);
            std::strncpy(c_diag.name, diag.name.c_str(), sizeof(c_diag.name) - 1);
            std::strncpy(c_diag.description, diag.description.c_str(),
                sizeof(c_diag.description) - 1);
            std::strncpy(c_diag.message, diag.message.c_str(),
                sizeof(c_diag.message) - 1);
            cpp_handle->c_diagnostics.push_back(c_diag);
        }

        out_buffer->data = reinterpret_cast<const uint8_t*>(buf.data.data());
        out_buffer->file_size = buf.file_size;
        out_buffer->diagnostics = cpp_handle->c_diagnostics.data();
        out_buffer->diagnostics_count = cpp_handle->c_diagnostics.size();

        return true;
    }

    void fastchunk_reader_close(fastchunk_reader_t* reader)
    {
        if (reader)
        {
            auto* cpp_handle = reinterpret_cast<CppReaderHandle*>(reader);
            if (cpp_handle->reader)
            {
                (void)cpp_handle->reader->close();
            }
            cpp_handle->c_diagnostics.clear();
        }
    }

    void fastchunk_reader_destroy(fastchunk_reader_t* reader)
    {
        if (reader)
        {
            auto* cpp_handle = reinterpret_cast<CppReaderHandle*>(reader);
            delete cpp_handle;
        }
    }

    // ============================================================================
    // Tokenizer API Implementation
    // ============================================================================

    fastchunk_tokenizer_t*
    fastchunk_tokenizer_create(const char* name, const char* config,
        fastchunk_error_t* out_error)
    {

        std::string tok_name = name ? name : std::string(fastchunk::constants::tokenizer::default_name);
        std::string tok_cfg = config ? config : "";

        auto res = fastchunk::create_tokenizer_from_name(tok_name, tok_cfg);
        if (!res.has_value())
        {
            populate_c_error(*res.error(), out_error);
            return nullptr;
        }

        auto handle = new (std::nothrow) CppTokenizerHandle();
        if (!handle)
            return nullptr;
        handle->tokenizer = res.value();
        return reinterpret_cast<fastchunk_tokenizer_t*>(handle);
    }

    void fastchunk_tokenizer_destroy(fastchunk_tokenizer_t* tokenizer)
    {
        if (tokenizer)
        {
            auto* handle = reinterpret_cast<CppTokenizerHandle*>(tokenizer);
            delete handle;
        }
    }

    // ============================================================================
    // Chunker API Implementation
    // ============================================================================

    fastchunk_chunker_t* fastchunk_chunker_create(void)
    {
        auto handle = new (std::nothrow) CppChunkerHandle();
        if (!handle)
            return nullptr;
        handle->chunker = fastchunk::create_chunker();
        return reinterpret_cast<fastchunk_chunker_t*>(handle);
    }

    bool fastchunk_chunker_chunk_buffer(
        fastchunk_chunker_t* chunker, const uint8_t* buffer_data,
        size_t buffer_size, const fastchunk_chunk_options_t* options,
        fastchunk_tokenizer_t* tokenizer, const char* doc_id, const char* record_id,
        fastchunk_chunk_array_t* out_chunks, fastchunk_error_t* out_error)
    {

        if (!chunker || !buffer_data || !options || !tokenizer || !out_chunks)
        {
            if (out_error)
            {
                out_error->code = static_cast<uint32_t>(fastchunk::ErrorCode::invalid_argument);
                std::strncpy(out_error->message,
                    "Null parameter passed to fastchunk_chunker_chunk_buffer",
                    sizeof(out_error->message) - 1);
            }
            return false;
        }

        auto* cpp_chunker = reinterpret_cast<CppChunkerHandle*>(chunker);
        auto* cpp_tok = reinterpret_cast<CppTokenizerHandle*>(tokenizer);

        fastchunk::ChunkOptions chk_opts { .max_tokens = options->max_tokens,
            .overlap_tokens = options->overlap_tokens };

        fastchunk::ChunkInputContext ctx { .doc_id = doc_id ? doc_id : "",
            .record_id = record_id ? record_id : "" };

        std::span<const std::byte> input_span(
            reinterpret_cast<const std::byte*>(buffer_data), buffer_size);

        auto res = cpp_chunker->chunker->chunk_buffer(input_span, chk_opts,
            *cpp_tok->tokenizer, ctx);
        if (!res.has_value())
        {
            populate_c_error(*res.error(), out_error);
            return false;
        }

        // Allocate container holding both owned result objects and C-view structs
        auto alloc = new (std::nothrow) CppChunkArrayAllocation();
        if (!alloc)
            return false;

        alloc->raw_chunks = std::move(res).value();
        alloc->c_chunk_views.reserve(alloc->raw_chunks.size());

        for (const auto& item : alloc->raw_chunks)
        {
            fastchunk_chunk_view_t view {};
            view.text_ptr = item.text.data();
            view.text_len = item.text.size();
            view.tokens_ptr = item.tokens.data();
            view.tokens_len = item.tokens.size();
            view.start_byte = item.start_byte;
            view.end_byte = item.end_byte;
            view.chunk_index = item.chunk_index;
            view.doc_id = item.doc_id.c_str();
            view.record_id = item.record_id.c_str();

            alloc->c_chunk_views.push_back(view);
        }

        out_chunks->chunks = alloc->c_chunk_views.data();
        out_chunks->count = alloc->c_chunk_views.size();

        return true;
    }

    void fastchunk_free_chunk_array(fastchunk_chunk_array_t* array)
    {
        if (array && array->chunks)
        {
            // Reconstruct pointer from contiguous array to free memory safely
            const auto* first_view = array->chunks;
            const auto* alloc = reinterpret_cast<const CppChunkArrayAllocation*>(
                reinterpret_cast<const char*>(first_view) - offsetof(CppChunkArrayAllocation, c_chunk_views));
            delete alloc;
            array->chunks = nullptr;
            array->count = 0;
        }
    }

    void fastchunk_chunker_destroy(fastchunk_chunker_t* chunker)
    {
        if (chunker)
        {
            auto* handle = reinterpret_cast<CppChunkerHandle*>(chunker);
            delete handle;
        }
    }

    fastchunk_cancellation_token_t* fastchunk_cancellation_token_create(void)
    {
        auto* tok = new (std::nothrow) fastchunk::CancellationToken();
        return reinterpret_cast<fastchunk_cancellation_token_t*>(tok);
    }

    void fastchunk_cancellation_token_cancel(
        fastchunk_cancellation_token_t* token)
    {
        if (token)
        {
            reinterpret_cast<fastchunk::CancellationToken*>(token)->cancel();
        }
    }

    bool fastchunk_cancellation_token_is_cancelled(
        fastchunk_cancellation_token_t* token)
    {
        if (!token)
            return false;
        return reinterpret_cast<fastchunk::CancellationToken*>(token)
            ->is_cancelled();
    }

    void fastchunk_cancellation_token_destroy(
        fastchunk_cancellation_token_t* token)
    {
        if (token)
        {
            delete reinterpret_cast<fastchunk::CancellationToken*>(token);
        }
    }

} // extern "C"