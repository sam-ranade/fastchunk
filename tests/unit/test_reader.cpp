#include "fastchunk/core.h"
#include "fastchunk/factory.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

// Forward declaration of internal reader for testing
namespace fastchunk
{
class MmapReader;
}

TEST_CASE("MmapReader Ingestion Boundaries", "[io][reader]")
{
    namespace fs = std::filesystem;
    auto temp_dir = fs::temp_directory_path() / "fastchunk_tests";
    fs::create_directories(temp_dir);

    SECTION("Valid UTF-8 File Mapping (Zero-Copy)")
    {
        auto file_path = temp_dir / "valid_utf8.txt";
        {
            std::ofstream out(file_path, std::ios::binary);
            out << "Hello, fastchunk zero-copy engine!";
        }

        fastchunk::InputOptions options { .mode = fastchunk::InputMode::zero_copy,
            .invalid_utf8 = fastchunk::InvalidUtf8Policy::error };

        // Instantiate concrete implementation via standard API pattern
        std::unique_ptr<fastchunk::IReader> reader = fastchunk::create_mmap_reader();
        auto result = reader->open(file_path, options);

        REQUIRE(result.has_value());
        auto buffer = result.value();
        CHECK(buffer.file_size == 34);
        CHECK(buffer.data.size() == 34);

        std::string_view read_view(
            reinterpret_cast<const char*>(buffer.data.data()), buffer.data.size());
        CHECK(read_view == "Hello, fastchunk zero-copy engine!");

        CHECK(reader->close().has_value());
        fs::remove(file_path);
    }

    SECTION("Invalid UTF-8 Detection under Error Policy")
    {
        auto file_path = temp_dir / "invalid_utf8.bin";
        {
            std::ofstream out(file_path, std::ios::binary);
            // "Hello " + Invalid UTF-8 sequence (0xFF, 0xC0)
            out << "Hello \xFF\xC0 World";
        }

        fastchunk::InputOptions options { .mode = fastchunk::InputMode::zero_copy,
            .invalid_utf8 = fastchunk::InvalidUtf8Policy::error };

        std::unique_ptr<fastchunk::IReader> reader = fastchunk::create_mmap_reader();
        auto result = reader->open(file_path, options);

        REQUIRE_FALSE(result.has_value());
        const auto* err = result.error();
        REQUIRE(err != nullptr);
        CHECK(err->code == static_cast<std::uint32_t>(fastchunk::ErrorCode::invalid_utf8));
        CHECK(err->byte_offset.has_value());
        CHECK(err->byte_offset.value() == 6);

        fs::remove(file_path);
    }

    SECTION("Conflicting Options (Zero-Copy with UTF-8 Replacement)")
    {
        auto file_path = temp_dir / "dummy.txt";
        {
            std::ofstream out(file_path, std::ios::binary);
            out << "test";
        }

        fastchunk::InputOptions options {
            .mode = fastchunk::InputMode::zero_copy,
            .invalid_utf8 = fastchunk::InvalidUtf8Policy::replace // Invalid combination
        };

        std::unique_ptr<fastchunk::IReader> reader = fastchunk::create_mmap_reader();
        auto result = reader->open(file_path, options);

        REQUIRE_FALSE(result.has_value());
        CHECK(result.error()->code == static_cast<std::uint32_t>(fastchunk::ErrorCode::invalid_configuration));

        fs::remove(file_path);
    }

    SECTION("Empty File Handling")
    {
        auto file_path = temp_dir / "empty.txt";
        {
            std::ofstream out(file_path, std::ios::binary);
        }

        fastchunk::InputOptions options {};
        std::unique_ptr<fastchunk::IReader> reader = fastchunk::create_mmap_reader();
        auto result = reader->open(file_path, options);

        REQUIRE(result.has_value());
        CHECK(result.value().file_size == 0);
        CHECK(result.value().data.empty());

        fs::remove(file_path);
    }

    fs::remove_all(temp_dir);
}