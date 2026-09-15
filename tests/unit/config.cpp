#include "fastchunk/config.h"
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

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

TEST_CASE("Configuration rejects unknown nested keys", "[config]")
{
    const auto path = std::filesystem::temp_directory_path()
        / "fastchunk_unknown_config_key.yaml";
    std::ofstream(path) << "schema_version: 1\n"
                           "input:\n"
                           "  path: input.txt\n"
                           "  unexpected: true\n"
                           "chunking:\n"
                           "  max_tokens: 16\n"
                           "  overlap_tokens: 2\n"
                           "export:\n"
                           "  type: none\n";

    auto result = fastchunk::load_configuration(path.string());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()->code
        == static_cast<std::uint32_t>(fastchunk::ErrorCode::invalid_configuration));
    CHECK(result.error()->message.find("input") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("Configuration rejects unimplemented restricted roots", "[config]")
{
    auto configuration = fastchunk::default_configuration();
    configuration.input.path = "input.txt";
    configuration.input.restricted_root = "/tmp/root";
    configuration.chunking.max_tokens = 16;
    configuration.chunking.overlap_tokens = 2;
    configuration.export_options.type = "none";

    auto result = configuration.validate();
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error()->code
        == static_cast<std::uint32_t>(fastchunk::ErrorCode::unsupported_option));
}
