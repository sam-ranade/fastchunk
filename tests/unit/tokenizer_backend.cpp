#include "fastchunk/tokenizers.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Native Tiktoken parity for cl100k_base", "[tokenizer][tiktoken]")
{
    auto tokenizer = fastchunk::create_tokenizer_from_name("cl100k_base");
    REQUIRE(tokenizer.has_value());

    auto encoded = tokenizer.value()->encode("Hello, world!");
    if (!encoded.has_value())
    {
        WARN("Tiktoken backend is disabled in this build: "
            << encoded.error()->message);
        return;
    }

    REQUIRE(encoded.value().size() == 4);
    CHECK(encoded.value()[0].id == 9906);
    CHECK(encoded.value()[1].id == 11);
    CHECK(encoded.value()[2].id == 1917);
    CHECK(encoded.value()[3].id == 0);
    CHECK(encoded.value()[0].start_byte == 0);
    CHECK(encoded.value()[0].end_byte == 5);
    CHECK(encoded.value()[2].start_byte == 6);
    CHECK(encoded.value()[2].end_byte == 12);
}