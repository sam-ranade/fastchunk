#include "fastchunk/config.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Configuration validation requires effective settings", "[config]")
{
    auto configuration = fastchunk::default_configuration();
    configuration.input.path = "input.txt";
    CHECK_FALSE(configuration.validate().has_value());

    configuration.chunking.max_tokens = 8;
    configuration.chunking.overlap_tokens = 8;
    CHECK_FALSE(configuration.validate().has_value());

    configuration.chunking.overlap_tokens = 2;
    configuration.export_options.path = "output.ndjson";
    CHECK(configuration.validate().has_value());
}

TEST_CASE("Configuration defaults accept whitespace tokenizer", "[config]")
{
    auto configuration = fastchunk::default_configuration();
    configuration.input.path = "input.txt";
    configuration.tokenizer.type = "whitespace";
    configuration.chunking.max_tokens = 16;
    configuration.chunking.overlap_tokens = 4;
    configuration.export_options.path = "output.ndjson";
    CHECK(configuration.validate().has_value());
}

TEST_CASE("Configuration rejects unavailable CPU affinity", "[config]")
{
    auto configuration = fastchunk::default_configuration();
    configuration.input.path = "input.txt";
    configuration.chunking.max_tokens = 16;
    configuration.chunking.overlap_tokens = 4;
    configuration.export_options.path = "output.ndjson";
    configuration.concurrency.cpu_affinity.mode = "isolated";

    auto result = configuration.validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()->code
        == static_cast<std::uint32_t>(fastchunk::ErrorCode::unsupported_option));
}
