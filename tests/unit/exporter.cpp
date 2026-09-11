#include "fastchunk/ndjson_exporter.h"
#include <catch2/catch_test_macros.hpp>

#include <sstream>

TEST_CASE("NDJSON exporter writes required schema fields", "[exporter]")
{
    std::ostringstream output;
    fastchunk::NdjsonExporter exporter(output);
    const fastchunk::ChunkResult chunk {
        .text = "hello",
        .tokens = { 1, 2 },
        .start_byte = 0,
        .end_byte = 5,
        .doc_id = "doc",
        .chunk_id = "doc:chunk:000000",
        .record_id = "record",
        .tokenizer_name = "mock",
        .tokenizer_configuration = "{}",
        .source_start_byte = 0,
        .source_end_byte = 10,
        .source_offset_kind = fastchunk::ChunkResult::SourceOffsetKind::record,
        .has_source_offsets = true,
        .chunk_index = 0,
        .metadata = "{}"
    };
    fastchunk::ExportMetadata metadata { .effective_configuration = "{\"max_tokens\":2}" };

    REQUIRE(exporter
                .export_chunks(std::span<const fastchunk::ChunkResult>(&chunk, 1),
                    metadata)
                .has_value());
    const auto serialized = output.str();
    CHECK(serialized.find("\"schema_version\":1") != std::string::npos);
    CHECK(serialized.find("\"source_offset_kind\":\"record\"") != std::string::npos);
    CHECK(serialized.find("\"metadata\":{}") != std::string::npos);
    CHECK(serialized.find("\"effective_configuration\":{\"max_tokens\":2}") != std::string::npos);
    CHECK(serialized.back() == '\n');
}

TEST_CASE("NDJSON exporter nulls unavailable source offsets", "[exporter]")
{
    std::ostringstream output;
    fastchunk::NdjsonExporter exporter(output);
    fastchunk::ChunkResult chunk;
    chunk.text = "x";
    REQUIRE(
        exporter
            .export_chunks(std::span<const fastchunk::ChunkResult>(&chunk, 1), {})
            .has_value());
    CHECK(output.str().find("\"source_start_byte\":null") != std::string::npos);
    CHECK(output.str().find("\"source_end_byte\":null") != std::string::npos);
    CHECK(output.str().find("\"source_offset_kind\":\"unavailable\"") != std::string::npos);
}