#include "fastchunk/core.h"
#include "fastchunk/factory.h"
#include "fastchunk/tokenizers.h"
#include <catch2/catch_test_macros.hpp>

// Forward declaration of internal chunker for testing
namespace fastchunk
{
class ChunkerImpl;
}

// Mock Tokenizer implementation for unit testing
class MockTokenizer final : public fastchunk::ITokenizer
{
public:
    fastchunk::Result<std::vector<fastchunk::EncodedToken>>
    encode(std::string_view text) override
    {
        std::vector<fastchunk::EncodedToken> tokens;
        // Simple space-delimited tokenization mock
        std::size_t start = 0;
        std::uint32_t id = 1;

        for (std::size_t i = 0; i <= text.size(); ++i)
        {
            if (i == text.size() || text[i] == ' ')
            {
                if (i > start)
                {
                    tokens.push_back(fastchunk::EncodedToken {
                        .id = id++, .start_byte = start, .end_byte = i });
                }
                start = i + 1;
            }
        }
        return fastchunk::Result<std::vector<fastchunk::EncodedToken>>(
            std::move(tokens));
    }

    std::string_view name() const noexcept override { return "mock_tokenizer"; }
};

TEST_CASE("Whitespace tokenizer preserves UTF-8 byte boundaries", "[tokenizer]")
{
    auto tokenizer_result = fastchunk::create_tokenizer_from_name("whitespace");
    REQUIRE(tokenizer_result.has_value());

    const std::string input = "  alpha\r\nbeta\f\v世界  ";
    auto encoded = tokenizer_result.value()->encode(input);
    REQUIRE(encoded.has_value());
    REQUIRE(encoded.value().size() == 3);

    CHECK(encoded.value()[0].id == 0);
    CHECK(input.substr(encoded.value()[0].start_byte,
              encoded.value()[0].end_byte - encoded.value()[0].start_byte)
        == "alpha");
    CHECK(encoded.value()[1].id == 1);
    CHECK(input.substr(encoded.value()[1].start_byte,
              encoded.value()[1].end_byte - encoded.value()[1].start_byte)
        == "beta");
    CHECK(encoded.value()[2].id == 2);
    CHECK(input.substr(encoded.value()[2].start_byte,
              encoded.value()[2].end_byte - encoded.value()[2].start_byte)
        == "世界");
}

TEST_CASE("Chunker Window & Streaming Logic", "[core][chunker]")
{
    MockTokenizer tokenizer;
    std::unique_ptr<fastchunk::IChunker> chunker = fastchunk::create_chunker();

    std::string text = "one two three four five six seven eight nine ten";
    auto buffer = std::as_bytes(std::span(text));

    SECTION("Valid Sliding Window Chunking")
    {
        fastchunk::ChunkOptions options { .max_tokens = 4, .overlap_tokens = 1 };
        fastchunk::ChunkInputContext context { .doc_id = "doc_test_1",
            .tokenizer_name = "mock_tokenizer",
            .tokenizer_configuration = "{}" };

        auto result = chunker->chunk_buffer(buffer, options, tokenizer, context);
        REQUIRE(result.has_value());

        const auto& chunks = result.value();
        // 10 tokens total, max 4, overlap 1 step = 3 tokens advanced per step -> 3
        // chunks total
        REQUIRE(chunks.size() == 3);

        // Verify Chunk 0
        CHECK(chunks[0].chunk_index == 0);
        CHECK(chunks[0].tokens.size() == 4);
        CHECK(chunks[0].text == "one two three four");
        CHECK(chunks[0].tokenizer_name == "mock_tokenizer");
        CHECK(chunks[0].tokenizer_configuration == "{}");

        // Verify Overlap in Chunk 1
        CHECK(chunks[1].chunk_index == 1);
        CHECK(chunks[1].tokens.size() == 4);
        CHECK(chunks[1].text == "four five six seven");
        CHECK(chunks[1].tokens.front() == chunks[0].tokens.back()); // Token ID overlap check

        // Verify Final Chunk
        CHECK(chunks[2].chunk_index == 2);
        CHECK(chunks[2].text == "seven eight nine ten");
    }

    SECTION("Invalid Configuration Errors")
    {
        fastchunk::ChunkOptions options {
            .max_tokens = 3,
            .overlap_tokens = 3 // Overlap cannot equal or exceed max_tokens
        };
        fastchunk::ChunkInputContext context {};

        auto result = chunker->chunk_buffer(buffer, options, tokenizer, context);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error()->code == static_cast<std::uint32_t>(fastchunk::ErrorCode::invalid_configuration));
    }

    SECTION("Stream Cancellation Support")
    {
        fastchunk::ChunkOptions options { .max_tokens = 2, .overlap_tokens = 0 };
        fastchunk::ChunkInputContext context {};
        fastchunk::CancellationToken cancel_token;

        auto stream_res = chunker->stream_buffer(buffer, options, tokenizer,
            context, &cancel_token);
        REQUIRE(stream_res.has_value());

        auto stream = std::move(stream_res).value();
        fastchunk::ChunkView view;

        // Advance once
        auto next_1 = stream->next(view);
        REQUIRE(next_1.has_value());
        CHECK(next_1.value() == true);

        // Cancel mid-stream
        cancel_token.request_cancel();

        // Next call should abort with cancelled error code
        auto next_2 = stream->next(view);
        REQUIRE_FALSE(next_2.has_value());
        CHECK(next_2.error()->code == static_cast<std::uint32_t>(fastchunk::ErrorCode::cancelled));
    }

    SECTION("Indivisible Oversized Token Diagnostic Warning")
    {
        // Single contiguous block with no space -> 1 single token spanning 50 bytes
        std::string long_word = "a_very_long_indivisible_token_string_exceeding_byte_limit";
        auto long_buffer = std::as_bytes(std::span(long_word));

        fastchunk::ChunkOptions options {
            .max_tokens = 5, // Max limit 5 tokens, but word is 1 large token
            .overlap_tokens = 0
        };
        fastchunk::ChunkInputContext context {};

        auto stream_res = chunker->stream_buffer(long_buffer, options, tokenizer, context);
        REQUIRE(stream_res.has_value());

        auto stream = std::move(stream_res).value();
        fastchunk::ChunkView view;

        auto next_res = stream->next(view);
        REQUIRE(next_res.has_value());
        CHECK(next_res.value() == true);

        // Check that diagnostic was emitted
        auto diagnostics = next_res.diagnostics();
        REQUIRE(diagnostics.size() == 1);
        CHECK(
            diagnostics[0].code == static_cast<std::uint32_t>(fastchunk::DiagnosticCode::oversized_token));
        CHECK(view.text == long_word);
    }
}