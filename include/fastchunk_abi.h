#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct fastchunk_reader fastchunk_reader_t;
    typedef struct fastchunk_tokenizer fastchunk_tokenizer_t;
    typedef struct fastchunk_chunker fastchunk_chunker_t;
    typedef struct fastchunk_result fastchunk_result_t;
    typedef struct fastchunk_stream fastchunk_stream_t;
    typedef struct fastchunk_cancellation fastchunk_cancellation_t;
    typedef struct fastchunk_error fastchunk_error_t;

    enum
    {
        FASTCHUNK_INPUT_ZERO_COPY = 0,
        FASTCHUNK_INPUT_COPY = 1,
        FASTCHUNK_UTF8_ERROR = 0,
        FASTCHUNK_UTF8_REPLACE = 1,
        FASTCHUNK_UTF8_SKIP = 2,
        FASTCHUNK_RECORD_NONE = 0,
        FASTCHUNK_RECORD_NDJSON = 1,
        FASTCHUNK_RECORD_ERROR = 0,
        FASTCHUNK_RECORD_SKIP = 1,
        FASTCHUNK_METADATA_ERROR = 0,
        FASTCHUNK_METADATA_IGNORE = 1,
        FASTCHUNK_SOURCE_OFFSETS_UNAVAILABLE = 0,
        FASTCHUNK_SOURCE_OFFSETS_EXACT = 1,
        FASTCHUNK_SOURCE_OFFSETS_RECORD = 2
    };

    typedef struct
    {
        const unsigned char* data;
        size_t size;
    } fastchunk_bytes_t;

    typedef struct
    {
        uint32_t input_mode;
        uint32_t invalid_utf8_policy;
        uint32_t record_mode;
        uint32_t malformed_record_policy;
        fastchunk_bytes_t record_id_field;
    } fastchunk_reader_options_t;

    typedef struct
    {
        size_t max_tokens;
        size_t overlap_tokens;
        uint32_t metadata_collision_policy;
    } fastchunk_chunker_options_t;

    typedef uint32_t fastchunk_status_t;

    uint32_t fastchunk_abi_version(void);

    typedef struct
    {
        fastchunk_bytes_t text;
        const uint32_t* token_ids;
        size_t token_count;
        size_t start_byte;
        size_t end_byte;
        fastchunk_bytes_t doc_id;
        fastchunk_bytes_t chunk_id;
        fastchunk_bytes_t metadata;
        fastchunk_bytes_t tokenizer_name;
        fastchunk_bytes_t tokenizer_configuration;
        fastchunk_bytes_t record_id;
        size_t source_start_byte;
        size_t source_end_byte;
        uint32_t source_offset_kind;
        uint8_t has_source_offsets;
        uint32_t chunk_index;
        uint8_t has_chunk;
    } fastchunk_chunk_view_t;

    typedef struct
    {
        uint8_t has_error;
        uint32_t code;
        fastchunk_bytes_t name;
        fastchunk_bytes_t description;
        fastchunk_bytes_t message;
        fastchunk_bytes_t dependency;
        fastchunk_bytes_t path;
        fastchunk_bytes_t details_json;
        size_t byte_offset;
        uint8_t has_byte_offset;
    } fastchunk_error_view_t;

    typedef struct
    {
        uint32_t severity;
        uint32_t code;
        fastchunk_bytes_t name;
        fastchunk_bytes_t description;
        fastchunk_bytes_t message;
        size_t source_byte_offset;
        uint8_t has_source_byte_offset;
    } fastchunk_diagnostic_view_t;

    // Handle allocations and initializations
    fastchunk_status_t fastchunk_reader_create(fastchunk_reader_t** reader,
        fastchunk_error_t** error);
    fastchunk_status_t
    fastchunk_reader_open_path(fastchunk_reader_t* reader,
        const fastchunk_bytes_t* path,
        const fastchunk_reader_options_t* options);
    fastchunk_status_t
    fastchunk_reader_open_buffer(fastchunk_reader_t* reader,
        const fastchunk_bytes_t* buffer,
        const fastchunk_reader_options_t* options);
    fastchunk_status_t fastchunk_reader_close(fastchunk_reader_t* reader);
    void fastchunk_reader_destroy(fastchunk_reader_t* reader);

    fastchunk_status_t fastchunk_tokenizer_create(
        const fastchunk_bytes_t* name, const fastchunk_bytes_t* configuration,
        fastchunk_tokenizer_t** tokenizer, fastchunk_error_t** error);
    void fastchunk_tokenizer_destroy(fastchunk_tokenizer_t* tokenizer);

    fastchunk_status_t
    fastchunk_chunker_create(const fastchunk_chunker_options_t* options,
        fastchunk_tokenizer_t* tokenizer,
        fastchunk_chunker_t** chunker,
        fastchunk_error_t** error);
    fastchunk_status_t fastchunk_chunker_run(fastchunk_chunker_t* chunker,
        fastchunk_reader_t* reader,
        fastchunk_cancellation_t* cancellation,
        fastchunk_result_t** result);
    fastchunk_status_t fastchunk_chunker_stream(
        fastchunk_chunker_t* chunker, fastchunk_reader_t* reader,
        fastchunk_cancellation_t* cancellation, fastchunk_stream_t** stream);
    void fastchunk_chunker_destroy(fastchunk_chunker_t* chunker);

    // Cancellation and execution controls
    fastchunk_status_t
    fastchunk_cancellation_create(fastchunk_cancellation_t** cancellation,
        fastchunk_error_t** error);
    void fastchunk_cancellation_request(fastchunk_cancellation_t* cancellation);
    void fastchunk_cancellation_destroy(fastchunk_cancellation_t* cancellation);

    fastchunk_status_t fastchunk_stream_next(fastchunk_stream_t* stream,
        uint8_t* has_chunk,
        fastchunk_chunk_view_t* output);
    fastchunk_status_t fastchunk_stream_cancel(fastchunk_stream_t* stream);
    fastchunk_status_t
    fastchunk_stream_is_cancelled(const fastchunk_stream_t* stream,
        uint8_t* cancelled);

    // Memory cleanup methods
    void fastchunk_result_release(fastchunk_result_t* result);
    void fastchunk_stream_release(fastchunk_stream_t* stream);
    void fastchunk_error_release(fastchunk_error_t* error);

    // Inspectors for result counts and errors
    fastchunk_status_t
    fastchunk_result_chunk_count(const fastchunk_result_t* result, size_t* count);
    fastchunk_status_t fastchunk_result_get_chunk(const fastchunk_result_t* result,
        size_t index,
        fastchunk_chunk_view_t* output);
    fastchunk_status_t fastchunk_error_get(const fastchunk_error_t* error,
        fastchunk_error_view_t* output);

    fastchunk_status_t fastchunk_reader_get_error(const fastchunk_reader_t* reader,
        fastchunk_error_view_t* output);
    fastchunk_status_t
    fastchunk_chunker_get_error(const fastchunk_chunker_t* chunker,
        fastchunk_error_view_t* output);
    fastchunk_status_t
    fastchunk_tokenizer_get_error(const fastchunk_tokenizer_t* tokenizer,
        fastchunk_error_view_t* output);
    fastchunk_status_t fastchunk_result_get_error(const fastchunk_result_t* result,
        fastchunk_error_view_t* output);
    fastchunk_status_t fastchunk_stream_get_error(const fastchunk_stream_t* stream,
        fastchunk_error_view_t* output);
    void fastchunk_reader_clear_error(fastchunk_reader_t* reader);
    void fastchunk_chunker_clear_error(fastchunk_chunker_t* chunker);
    void fastchunk_tokenizer_clear_error(fastchunk_tokenizer_t* tokenizer);
    void fastchunk_stream_clear_error(fastchunk_stream_t* stream);

    fastchunk_status_t
    fastchunk_reader_diagnostic_count(const fastchunk_reader_t* reader,
        size_t* count);
    fastchunk_status_t
    fastchunk_reader_diagnostic_get(const fastchunk_reader_t* reader, size_t index,
        fastchunk_diagnostic_view_t* output);
    fastchunk_status_t
    fastchunk_tokenizer_diagnostic_count(const fastchunk_tokenizer_t* tokenizer,
        size_t* count);
    fastchunk_status_t
    fastchunk_tokenizer_diagnostic_get(const fastchunk_tokenizer_t* tokenizer,
        size_t index,
        fastchunk_diagnostic_view_t* output);
    fastchunk_status_t
    fastchunk_chunker_diagnostic_count(const fastchunk_chunker_t* chunker,
        size_t* count);
    fastchunk_status_t
    fastchunk_chunker_diagnostic_get(const fastchunk_chunker_t* chunker,
        size_t index,
        fastchunk_diagnostic_view_t* output);
    fastchunk_status_t
    fastchunk_stream_diagnostic_count(const fastchunk_stream_t* stream,
        size_t* count);
    fastchunk_status_t
    fastchunk_stream_diagnostic_get(const fastchunk_stream_t* stream, size_t index,
        fastchunk_diagnostic_view_t* output);
    fastchunk_status_t
    fastchunk_result_diagnostic_count(const fastchunk_result_t* result,
        size_t* count);
    fastchunk_status_t
    fastchunk_result_diagnostic_get(const fastchunk_result_t* result, size_t index,
        fastchunk_diagnostic_view_t* output);

#ifdef __cplusplus
}
#endif
