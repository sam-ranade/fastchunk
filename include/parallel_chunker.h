#pragma once

#include "fastchunk/core.h"
#include <future>
#include <thread>
#include <vector>

namespace fastchunk
{

class ParallelChunker
{
public:
    // Runs parallel chunk processing over multi-record/multi-document buffers
    static Result<std::vector<ChunkResult>> chunk_buffer_parallel(
        std::span<const std::byte> buffer, const ChunkOptions& options,
        ITokenizer& tokenizer, const ChunkInputContext& context,
        IChunker& chunker_engine,
        std::size_t num_threads = std::thread::hardware_concurrency())
    {

        if (num_threads == 0)
            num_threads = 1;
        std::size_t total_size = buffer.size();

        // Small inputs run sequentially on calling thread
        if (total_size < 1024 * 1024 || num_threads == 1)
        { // < 1MB threshold
            return chunker_engine.chunk_buffer(buffer, options, tokenizer, context);
        }

        // Determine slice offsets on newline boundaries to avoid breaking records
        std::vector<std::size_t> slice_offsets;
        slice_offsets.push_back(0);

        std::size_t chunk_size = total_size / num_threads;
        const auto* bytes = reinterpret_cast<const char*>(buffer.data());

        for (std::size_t t = 1; t < num_threads; ++t)
        {
            std::size_t target = t * chunk_size;
            while (target < total_size && bytes[target] != '\n')
            {
                target++;
            }
            if (target < total_size)
            {
                slice_offsets.push_back(target + 1); // Point right after newline
            }
        }
        slice_offsets.push_back(total_size);

        std::size_t actual_tasks = slice_offsets.size() - 1;
        std::vector<std::future<Result<std::vector<ChunkResult>>>> futures;
        futures.reserve(actual_tasks);

        // Dispatch async worker tasks
        for (std::size_t i = 0; i < actual_tasks; ++i)
        {
            std::size_t start = slice_offsets[i];
            std::size_t end = slice_offsets[i + 1];

            futures.push_back(
                std::async(std::launch::async, [&chunker_engine, buffer, start, end, &options, &tokenizer, context, i]()
                    {
                    auto sub_span = buffer.subspan(start, end - start);
                    ChunkInputContext local_ctx = context;
                    local_ctx.doc_id = context.doc_id + "_part_" + std::to_string(i);

                    return chunker_engine.chunk_buffer(sub_span, options, tokenizer,
                        local_ctx); }));
        }

        // Collect and merge results in strict deterministic order
        std::vector<ChunkResult> global_results;
        std::size_t current_chunk_idx = 0;

        for (auto& fut : futures)
        {
            auto res = fut.get();
            if (!res.has_value())
            {
                return res; // Propagate error
            }

            auto chunks = std::move(res).value();
            for (auto& item : chunks)
            {
                item.chunk_index = current_chunk_idx++;
                global_results.push_back(std::move(item));
            }
        }

        return Result<std::vector<ChunkResult>>(std::move(global_results));
    }
};

} // namespace fastchunk
