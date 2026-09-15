#pragma once

#include "fastchunk/core.h"
#include <format>
#include <future>
#include <algorithm>
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
        if (buffer.empty())
            return Result<std::vector<ChunkResult>>(std::vector<ChunkResult> {});

        // Tokenize once over the complete input. Partitioning raw bytes before
        // tokenization changes tokenizer context and breaks exact overlap.
        auto encoded = tokenizer.encode(std::string_view(
            reinterpret_cast<const char*>(buffer.data()), buffer.size()));
        if (!encoded.has_value())
            return Result<std::vector<ChunkResult>>(*encoded.error());

        struct Window
        {
            std::size_t first;
            std::size_t last;
            bool oversized;
        };
        std::vector<Window> windows;
        const auto& tokens = encoded.value();
        for (std::size_t start = 0; start < tokens.size();)
        {
            const auto end = std::min(start + options.max_tokens, tokens.size());
            const bool oversized = end == start + 1
                && tokens[start].end_byte - tokens[start].start_byte > options.max_tokens;
            windows.push_back({ start, oversized ? start + 1 : end, oversized });
            if (end == tokens.size() || oversized)
                start = oversized ? start + 1 : end;
            else
                start = end - options.overlap_tokens;
        }

        if (windows.size() < 2 || num_threads == 1)
            return chunker_engine.chunk_buffer(buffer, options, tokenizer, context);

        const std::size_t actual_tasks = std::min(num_threads, windows.size());
        std::vector<std::future<Result<std::vector<ChunkResult>>>> futures;
        futures.reserve(actual_tasks);

        const std::size_t windows_per_task =
            (windows.size() + actual_tasks - 1) / actual_tasks;
        for (std::size_t task = 0; task < actual_tasks; ++task)
        {
            const auto window_begin = task * windows_per_task;
            const auto window_end = std::min(windows.size(),
                window_begin + windows_per_task);
            if (window_begin >= window_end)
                continue;

            futures.push_back(
                std::async(std::launch::async, [buffer, &options, &tokens, &windows,
                    context, window_begin, window_end]()
                    {
                        std::vector<ChunkResult> local;
                        local.reserve(window_end - window_begin);
                        const auto* text = reinterpret_cast<const char*>(buffer.data());
                        for (std::size_t index = window_begin; index < window_end; ++index)
                        {
                            const auto window = windows[index];
                            const auto start_byte = tokens[window.first].start_byte;
                            const auto end_byte = tokens[window.last - 1].end_byte;
                            ChunkResult result {
                                .text = std::string(text + start_byte, end_byte - start_byte),
                                .start_byte = start_byte,
                                .end_byte = end_byte,
                                .doc_id = context.doc_id,
                                .record_id = context.record_id,
                                .tokenizer_name = context.tokenizer_name,
                                .tokenizer_configuration = context.tokenizer_configuration,
                                .source_start_byte = context.source_start_byte,
                                .source_end_byte = context.source_end_byte,
                                .source_offset_kind = context.source_offset_kind,
                                .has_source_offsets = context.source_offset_kind
                                    != ChunkResult::SourceOffsetKind::unavailable,
                                .chunk_index = static_cast<std::uint32_t>(index),
                                .metadata = context.metadata };
                            result.tokens.reserve(window.last - window.first);
                            for (std::size_t token = window.first; token < window.last; ++token)
                                result.tokens.push_back(tokens[token].id);
                            result.chunk_id = std::format(
                                "{}:chunk:{:06d}", context.doc_id.empty() ? "doc" : context.doc_id,
                                static_cast<unsigned>(index));
                            local.push_back(std::move(result));
                        }
                        return Result<std::vector<ChunkResult>>(std::move(local));
                    }));
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
                item.chunk_id = std::format(
                    "{}:chunk:{:06d}", context.doc_id.empty() ? "doc" : context.doc_id,
                    static_cast<unsigned>(item.chunk_index));
                global_results.push_back(std::move(item));
            }
        }

        return Result<std::vector<ChunkResult>>(std::move(global_results));
    }
};

} // namespace fastchunk
