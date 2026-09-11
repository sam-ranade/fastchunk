#pragma once

#include "fastchunk/cancellation.h"
#include "fastchunk/constants.h"
#include "fastchunk/core.h"
#include <memory>
#include <span>
#include <vector>

namespace fastchunk
{

class StreamChunker
{
public:
    explicit StreamChunker(ChunkOptions options,
        std::shared_ptr<ITokenizer> tokenizer)
        : options_(options)
        , tokenizer_(std::move(tokenizer))
    {
    }

    // Processes an incoming data block, maintaining overlapping context state
    Result<std::vector<ChunkResult>>
    push_bytes(std::span<const std::byte> input_block,
        const ChunkInputContext& context,
        CancellationTokenPtr cancellation_token = nullptr)
    {

        if (cancellation_token && cancellation_token->is_cancelled())
        {
            return Result<std::vector<ChunkResult>>(Error {
                .code = static_cast<std::uint32_t>(ErrorCode::cancelled),
                .name = std::string(constants::error_name::cancelled),
                .description = "The operation was aborted via CancellationToken.",
                .message = "Streaming chunk execution cancelled by caller." });
        }

        // Append incoming block to internal residual state
        residual_buffer_.insert(residual_buffer_.end(), input_block.begin(),
            input_block.end());

        std::vector<ChunkResult> chunks;
        if (residual_buffer_.size() < options_.max_tokens)
        {
            return chunks; // Wait for sufficient data accumulate
        }

        // Process chunk execution over accumulated state
        auto chunker = create_chunker();
        auto result = chunker->chunk_buffer(
            std::span<const std::byte>(residual_buffer_.data(),
                residual_buffer_.size()),
            options_, *tokenizer_, context);

        if (!result.has_value())
        {
            return result;
        }

        chunks = std::move(result).value();

        // Retain overlapping tail bytes in residual_buffer_
        if (!chunks.empty())
        {
            const auto& last_chunk = chunks.back();
            std::size_t retain_bytes = residual_buffer_.size() > last_chunk.end_byte
                ? residual_buffer_.size() - last_chunk.end_byte
                : 0;

            std::vector<std::byte> next_residual(
                residual_buffer_.end() - static_cast<std::ptrdiff_t>(retain_bytes),
                residual_buffer_.end());
            residual_buffer_ = std::move(next_residual);
        }

        return chunks;
    }

    // Flushes remaining residual bytes on stream completion
    Result<std::vector<ChunkResult>> flush(const ChunkInputContext& context)
    {
        if (residual_buffer_.empty())
        {
            return std::vector<ChunkResult> {};
        }

        auto chunker = create_chunker();
        auto result = chunker->chunk_buffer(
            std::span<const std::byte>(residual_buffer_.data(),
                residual_buffer_.size()),
            options_, *tokenizer_, context);

        residual_buffer_.clear();
        return result;
    }

private:
    ChunkOptions options_;
    std::shared_ptr<ITokenizer> tokenizer_;
    std::vector<std::byte> residual_buffer_;
};

} // namespace fastchunk
