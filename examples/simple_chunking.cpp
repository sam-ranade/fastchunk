#include "fastchunk/core.h"
#include "fastchunk/factory.h"
#include "fastchunk/tokenizers.h"
#include <iostream>

int main()
{
    // 1. Instantiate Mmap Reader
    auto reader = fastchunk::create_mmap_reader();
    fastchunk::InputOptions in_opts { .mode = fastchunk::InputMode::zero_copy,
        .invalid_utf8 = fastchunk::InvalidUtf8Policy::replace };

    // Open target text file zero-copy
    auto file_res = reader->open("sample.txt", in_opts);
    if (!file_res.has_value())
    {
        std::cerr << "Failed to open file: " << file_res.error()->message << "\n";
        return 1;
    }

    // 2. Initialize Tiktoken BPE Tokenizer
    auto tok_res = fastchunk::create_tokenizer_from_name("tiktoken", "cl100k_base");
    if (!tok_res.has_value())
    {
        std::cerr << "Failed to load tokenizer: " << tok_res.error()->message
                  << "\n";
        return 1;
    }

    // 3. Configure Chunker
    auto chunker = fastchunk::create_chunker();
    fastchunk::ChunkOptions chunk_opts { .max_tokens = 256, .overlap_tokens = 32 };

    fastchunk::ChunkInputContext context { .doc_id = "doc_001",
        .record_id = "rec_001" };

    // 4. Run Execution Loop
    auto chunk_res = chunker->chunk_buffer(file_res.value().data, chunk_opts,
        *tok_res.value(), context);

    if (!chunk_res.has_value())
    {
        std::cerr << "Chunking failed: " << chunk_res.error()->message << "\n";
        return 1;
    }

    // 5. Output Chunk Views
    std::cout << "Successfully generated " << chunk_res.value().size()
              << " chunks:\n";
    for (const auto& chunk : chunk_res.value())
    {
        std::cout << "[" << chunk.chunk_index << "] "
                  << "Bytes: " << chunk.start_byte << ".." << chunk.end_byte
                  << " | " << "Tokens: " << chunk.tokens.size() << " | "
                  << "Preview: "
                  << chunk.text.substr(0, std::min<size_t>(40, chunk.text.size()))
                  << "...\n";
    }

    return 0;
}