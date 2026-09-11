#include "fastchunk/core.h"
#include "fastchunk/factory.h"
#include "fastchunk/tokenizers.h"
#include "parallel_chunker.h"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace
{

std::vector<std::byte> generate_benchmark_payload(std::size_t target_bytes,
    std::uint32_t seed)
{
    static constexpr std::string_view words[] {
        "throughput", "tokenizer", "boundary", "zero-copy", "retrieval",
        "pipeline", "concurrency", "multibyte", "benchmark", "metadata"
    };
    std::mt19937 generator(seed);
    std::uniform_int_distribution<std::size_t> word_index(0,
        std::size(words) - 1);
    std::vector<std::byte> payload;
    payload.reserve(target_bytes);
    while (payload.size() < target_bytes)
    {
        std::string line;
        line.reserve(160);
        for (std::size_t index = 0; index < 18; ++index)
        {
            if (index != 0)
                line.push_back(' ');
            line += words[word_index(generator)];
        }
        line += "\n";
        const auto remaining = target_bytes - payload.size();
        const auto count = std::min(remaining, line.size());
        const auto* bytes = reinterpret_cast<const std::byte*>(line.data());
        payload.insert(payload.end(), bytes, bytes + count);
    }
    return payload;
}

}

void print_percentile_stats(std::vector<double>& latencies_ms,
    double total_bytes)
{
    std::sort(latencies_ms.begin(), latencies_ms.end());
    size_t n = latencies_ms.size();

    double avg_ms = std::accumulate(latencies_ms.begin(), latencies_ms.end(), 0.0) / n;
    double p50 = latencies_ms[static_cast<size_t>(n * 0.50)];
    double p95 = latencies_ms[static_cast<size_t>(n * 0.95)];
    double p99 = latencies_ms[static_cast<size_t>(n * 0.99)];

    double total_sec = std::accumulate(latencies_ms.begin(), latencies_ms.end(), 0.0) / 1000.0;
    double mb_s = (total_bytes * n / (1024.0 * 1024.0)) / total_sec;

    std::cout << "--------------------------------------------\n";
    std::cout << "Throughput:  " << mb_s << " MB/s\n";
    std::cout << "Average:     " << avg_ms << " ms\n";
    std::cout << "p50:         " << p50 << " ms\n";
    std::cout << "p95:         " << p95 << " ms\n";
    std::cout << "p99:         " << p99 << " ms\n";
    std::cout << "--------------------------------------------\n";
}

int main(int argc, char** argv)
{
    std::string test_file;
    std::size_t generated_mb = 0;
    int iterations = 50;
    std::uint32_t seed = 42;

    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index];
        if (argument == "--generated-mb" && index + 1 < argc)
            generated_mb = std::stoull(argv[++index]);
        else if (argument == "--iterations" && index + 1 < argc)
            iterations = std::stoi(argv[++index]);
        else if (argument == "--seed" && index + 1 < argc)
            seed = static_cast<std::uint32_t>(std::stoul(argv[++index]));
        else if (test_file.empty())
            test_file = argument;
    }

    std::vector<std::byte> generated_payload;
    if (generated_mb != 0)
        generated_payload = generate_benchmark_payload(generated_mb * 1024ULL * 1024ULL, seed);

    std::cout << "=== FastChunk Performance Benchmark Utility ===\n";
    std::cout << "Input: " << (generated_payload.empty() ? test_file : "generated in-memory payload") << "\n";
    std::cout << "Iterations:  " << iterations << "\n\n";

    std::vector<std::byte> file_payload;
    std::span<const std::byte> input;
    if (!generated_payload.empty())
    {
        input = generated_payload;
    }
    else
    {
        if (test_file.empty())
            test_file = "benchmark_corpus.txt";
        auto reader = fastchunk::create_mmap_reader();
        fastchunk::InputOptions in_opts { .mode = fastchunk::InputMode::zero_copy };
        auto open_res = reader->open(test_file, in_opts);
        if (!open_res.has_value())
        {
            std::cerr << "Benchmark Error: Cannot map target file " << test_file << "\n";
            return 1;
        }
        auto buffer = open_res.value();
        input = buffer.data;
    }

    auto tok_res = fastchunk::create_tokenizer_from_name("whitespace");
    if (!tok_res.has_value())
    {
        std::cerr << "Benchmark Error: Cannot create whitespace tokenizer\n";
        return 1;
    }
    auto chunker = fastchunk::create_chunker();

    fastchunk::ChunkOptions chk_opts { .max_tokens = 512, .overlap_tokens = 64 };
    fastchunk::ChunkInputContext ctx { .doc_id = "bench_doc" };

    // --- Benchmark 1: Single-Threaded Chunk Processing ---
    std::cout << "[1/2] Running Single-Threaded Benchmark...\n";
    std::vector<double> st_latencies;
    st_latencies.reserve(iterations);

    for (int i = 0; i < iterations; ++i)
    {
        auto start = std::chrono::high_resolution_clock::now();

        auto res = chunker->chunk_buffer(input, chk_opts, *tok_res.value(), ctx);

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - start;
        st_latencies.push_back(elapsed.count());
    }
    print_percentile_stats(st_latencies, static_cast<double>(input.size()));

    // --- Benchmark 2: Multi-Threaded Parallel Execution ---
    std::cout << "\n[2/2] Running Multi-Threaded Parallel Chunker Benchmark...\n";
    std::vector<double> mt_latencies;
    mt_latencies.reserve(iterations);

    for (int i = 0; i < iterations; ++i)
    {
        auto start = std::chrono::high_resolution_clock::now();

        auto res = fastchunk::ParallelChunker::chunk_buffer_parallel(
            input, chk_opts, *tok_res.value(), ctx, *chunker);

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end - start;
        mt_latencies.push_back(elapsed.count());
    }
    print_percentile_stats(mt_latencies, static_cast<double>(input.size()));

    return 0;
}