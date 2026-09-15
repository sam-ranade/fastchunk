#include "fastchunk_abi.h"
#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("C ABI tokenizer creation exposes complete errors", "[abi]")
{
    const fastchunk_bytes_t name {
        reinterpret_cast<const unsigned char*>("missing-backend"), 15
    };
    fastchunk_tokenizer_t* tokenizer = nullptr;
    fastchunk_error_t* error = nullptr;
    const auto status = fastchunk_tokenizer_create(
        &name, nullptr, &tokenizer, &error);

    CHECK(status != 0);
    CHECK(tokenizer == nullptr);
    REQUIRE(error != nullptr);
    fastchunk_error_view_t view {};
    REQUIRE(fastchunk_error_get(error, &view) == 0);
    CHECK(view.has_error == 1);
    CHECK(view.code == 1000);
    fastchunk_error_release(error);
}

TEST_CASE("C ABI NDJSON records preserve identity and source container",
    "[abi][ndjson]")
{
    const std::string input = "{\"id\":\"first\",\"text\":\"one two\"}\n"
                              "{\"id\":\"second\",\"text\":\"three four\"}\n";
    const fastchunk_bytes_t buffer {
        reinterpret_cast<const unsigned char*>(input.data()), input.size()
    };
    const fastchunk_bytes_t record_id_field {
        reinterpret_cast<const unsigned char*>("id"), 2
    };
    const fastchunk_bytes_t tokenizer_name {
        reinterpret_cast<const unsigned char*>("whitespace"), 10
    };

    fastchunk_reader_options_t reader_options {};
    reader_options.input_mode = FASTCHUNK_INPUT_ZERO_COPY;
    reader_options.invalid_utf8_policy = FASTCHUNK_UTF8_ERROR;
    reader_options.record_mode = FASTCHUNK_RECORD_NDJSON;
    reader_options.malformed_record_policy = FASTCHUNK_RECORD_ERROR;
    reader_options.record_id_field = record_id_field;

    fastchunk_reader_t* reader = nullptr;
    REQUIRE(fastchunk_reader_create(&reader, nullptr) == 0);
    REQUIRE(fastchunk_reader_open_buffer(reader, &buffer, &reader_options) == 0);

    fastchunk_tokenizer_t* tokenizer = nullptr;
    REQUIRE(fastchunk_tokenizer_create(&tokenizer_name, nullptr, &tokenizer,
                nullptr)
        == 0);

    fastchunk_chunker_options_t chunker_options { .max_tokens = 64,
        .overlap_tokens = 0,
        .metadata_collision_policy = FASTCHUNK_METADATA_ERROR };
    fastchunk_chunker_t* chunker = nullptr;
    REQUIRE(fastchunk_chunker_create(&chunker_options, tokenizer, &chunker,
                nullptr)
        == 0);

    fastchunk_result_t* result = nullptr;
    REQUIRE(fastchunk_chunker_run(chunker, reader, nullptr, &result) == 0);

    size_t count = 0;
    REQUIRE(fastchunk_result_chunk_count(result, &count) == 0);
    REQUIRE(count == 2);

    fastchunk_chunk_view_t first {};
    REQUIRE(fastchunk_result_get_chunk(result, 0, &first) == 0);
    CHECK(std::string(reinterpret_cast<const char*>(first.record_id.data),
              first.record_id.size)
        == "first");
    CHECK(first.source_offset_kind == FASTCHUNK_SOURCE_OFFSETS_RECORD);
    CHECK(first.has_source_offsets == 1);
    CHECK(first.source_start_byte == 0);

    fastchunk_result_release(result);
    fastchunk_chunker_destroy(chunker);
    fastchunk_tokenizer_destroy(tokenizer);
    fastchunk_reader_close(reader);
    fastchunk_reader_destroy(reader);
}

TEST_CASE("C ABI NDJSON rejects missing record IDs under error policy",
    "[abi][ndjson]")
{
    const std::string input = "{\"text\":\"missing id\"}\n";
    const fastchunk_bytes_t buffer {
        reinterpret_cast<const unsigned char*>(input.data()), input.size()
    };
    const fastchunk_bytes_t record_id_field {
        reinterpret_cast<const unsigned char*>("id"), 2
    };

    fastchunk_reader_options_t options {};
    options.input_mode = FASTCHUNK_INPUT_ZERO_COPY;
    options.invalid_utf8_policy = FASTCHUNK_UTF8_ERROR;
    options.record_mode = FASTCHUNK_RECORD_NDJSON;
    options.malformed_record_policy = FASTCHUNK_RECORD_ERROR;
    options.record_id_field = record_id_field;

    fastchunk_reader_t* reader = nullptr;
    REQUIRE(fastchunk_reader_create(&reader, nullptr) == 0);
    CHECK(fastchunk_reader_open_buffer(reader, &buffer, &options) != 0);
    fastchunk_reader_destroy(reader);
}

TEST_CASE("C ABI NDJSON reports malformed records and CRLF boundaries",
    "[abi][ndjson]")
{
    const std::string input = "{\"id\":\"first\"}\r\n"
                              "not-json\n"
                              "{\"id\":\"last\"}";
    const fastchunk_bytes_t buffer {
        reinterpret_cast<const unsigned char*>(input.data()), input.size()
    };
    const fastchunk_bytes_t record_id_field {
        reinterpret_cast<const unsigned char*>("id"), 2
    };
    fastchunk_reader_options_t options {};
    options.input_mode = FASTCHUNK_INPUT_ZERO_COPY;
    options.invalid_utf8_policy = FASTCHUNK_UTF8_ERROR;
    options.record_mode = FASTCHUNK_RECORD_NDJSON;
    options.malformed_record_policy = FASTCHUNK_RECORD_SKIP;
    options.record_id_field = record_id_field;

    fastchunk_reader_t* reader = nullptr;
    REQUIRE(fastchunk_reader_create(&reader, nullptr) == 0);
    REQUIRE(fastchunk_reader_open_buffer(reader, &buffer, &options) == 0);

    size_t diagnostics = 0;
    REQUIRE(fastchunk_reader_diagnostic_count(reader, &diagnostics) == 0);
    CHECK(diagnostics == 1);

    fastchunk_tokenizer_t* tokenizer = nullptr;
    const fastchunk_bytes_t tokenizer_name {
        reinterpret_cast<const unsigned char*>("whitespace"), 10
    };
    REQUIRE(fastchunk_tokenizer_create(&tokenizer_name, nullptr, &tokenizer,
                nullptr)
        == 0);
    fastchunk_chunker_options_t chunker_options { 64, 0, FASTCHUNK_METADATA_ERROR };
    fastchunk_chunker_t* chunker = nullptr;
    REQUIRE(fastchunk_chunker_create(&chunker_options, tokenizer, &chunker,
                nullptr)
        == 0);
    fastchunk_result_t* result = nullptr;
    REQUIRE(fastchunk_chunker_run(chunker, reader, nullptr, &result) == 0);

    size_t count = 0;
    REQUIRE(fastchunk_result_chunk_count(result, &count) == 0);
    CHECK(count == 2);
    fastchunk_chunk_view_t first {};
    REQUIRE(fastchunk_result_get_chunk(result, 0, &first) == 0);
    CHECK(first.source_end_byte == 16);

    fastchunk_result_release(result);
    fastchunk_chunker_destroy(chunker);
    fastchunk_tokenizer_destroy(tokenizer);
    fastchunk_reader_destroy(reader);
}

TEST_CASE("C ABI NDJSON generates IDs and cancellation is safe",
    "[abi][ndjson]")
{
    const std::string input = "{\"value\":1}\n{\"value\":2}\n";
    const fastchunk_bytes_t buffer {
        reinterpret_cast<const unsigned char*>(input.data()), input.size()
    };
    fastchunk_reader_options_t reader_options {};
    reader_options.input_mode = FASTCHUNK_INPUT_ZERO_COPY;
    reader_options.invalid_utf8_policy = FASTCHUNK_UTF8_ERROR;
    reader_options.record_mode = FASTCHUNK_RECORD_NDJSON;
    reader_options.malformed_record_policy = FASTCHUNK_RECORD_ERROR;

    fastchunk_reader_t* reader = nullptr;
    REQUIRE(fastchunk_reader_create(&reader, nullptr) == 0);
    REQUIRE(fastchunk_reader_open_buffer(reader, &buffer, &reader_options) == 0);
    const fastchunk_bytes_t tokenizer_name {
        reinterpret_cast<const unsigned char*>("whitespace"), 10
    };
    fastchunk_tokenizer_t* tokenizer = nullptr;
    REQUIRE(fastchunk_tokenizer_create(&tokenizer_name, nullptr, &tokenizer,
                nullptr)
        == 0);
    fastchunk_chunker_options_t chunker_options { 64, 0, FASTCHUNK_METADATA_ERROR };
    fastchunk_chunker_t* chunker = nullptr;
    REQUIRE(fastchunk_chunker_create(&chunker_options, tokenizer, &chunker,
                nullptr)
        == 0);
    fastchunk_stream_t* stream = nullptr;
    REQUIRE(fastchunk_chunker_stream(chunker, reader, nullptr, &stream) == 0);
    REQUIRE(fastchunk_stream_cancel(stream) == 0);
    uint8_t cancelled = 0;
    REQUIRE(fastchunk_stream_is_cancelled(stream, &cancelled) == 0);
    CHECK(cancelled == 1);

    fastchunk_stream_release(stream);
    fastchunk_chunker_destroy(chunker);
    fastchunk_tokenizer_destroy(tokenizer);
    fastchunk_reader_destroy(reader);
}