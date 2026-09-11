#include "fastchunk/constants.h"
#include "fastchunk/core.h"

#include <algorithm>
#include <format>

namespace fastchunk
{

class ChunkStreamImpl final : public IChunkStream
{
public:
    ChunkStreamImpl(std::span<const std::byte> buffer, ChunkOptions options,
        std::vector<EncodedToken> encoded_tokens,
        ChunkInputContext context,
        const CancellationToken* cancel_token)
        : buffer_(buffer)
        , options_(options)
        , tokens_(std::move(encoded_tokens))
        , context_(std::move(context))
        , cancel_token_(cancel_token)
    {

        // Allocate contiguous token ID memory once to fulfill streaming zero-alloc
        // guarantees
        token_ids_.reserve(tokens_.size());
        for (const auto& tok : tokens_)
        {
            token_ids_.push_back(tok.id);
        }
    }

    Result<bool> next(ChunkView& output) override
    {
        if (is_cancelled_ || (cancel_token_ && cancel_token_->is_cancelled()))
        {
            is_cancelled_ = true;
            return Result<bool>(Error {
                .code = static_cast<std::uint32_t>(ErrorCode::cancelled),
                .name = std::string(constants::error_name::cancelled),
                .description = "The caller requested cancellation before the "
                               "operation completed.",
                .message = "Streaming pipeline execution cancelled by client caller." });
        }

        if (start_token_idx_ >= tokens_.size())
        {
            return Result<bool>(false); // End-of-stream
        }

        std::size_t window_end = std::min(start_token_idx_ + options_.max_tokens, tokens_.size());
        bool is_oversized = false;

        // Handle single indivisible oversized token condition (Section 2.2)
        if (start_token_idx_ == window_end - 1 && (tokens_[start_token_idx_].end_byte - tokens_[start_token_idx_].start_byte) > options_.max_tokens)
        {
            window_end = start_token_idx_ + 1;
            is_oversized = true;
        }

        std::size_t chunk_start_byte = tokens_[start_token_idx_].start_byte;
        std::size_t chunk_end_byte = tokens_[window_end - 1].end_byte;

        const char* str_ptr = reinterpret_cast<const char*>(buffer_.data());

        // Construct zero-copy views without triggering heap allocations
        active_text_ = std::string_view(str_ptr + chunk_start_byte,
            chunk_end_byte - chunk_start_byte);

        // Format doc identity string deterministically
        formatted_chunk_id_ = std::format(
            "{}:chunk:{:06d}", context_.doc_id.empty() ? "doc" : context_.doc_id,
            chunk_index_);

        output.text = active_text_;
        output.tokens = std::span<const std::uint32_t>(
            token_ids_.data() + start_token_idx_, window_end - start_token_idx_);
        output.doc_id = context_.doc_id;
        output.chunk_id = formatted_chunk_id_;
        output.start_byte = chunk_start_byte;
        output.end_byte = chunk_end_byte;
        output.record_id = context_.record_id;
        output.source_start_byte = context_.source_start_byte;
        output.source_end_byte = context_.source_end_byte;
        output.source_offset_kind = context_.source_offset_kind;
        output.has_source_offsets = (context_.source_offset_kind != ChunkResult::SourceOffsetKind::unavailable);
        output.chunk_index = chunk_index_++;
        output.metadata = context_.metadata;
        output.tokenizer_name = context_.tokenizer_name;
        output.tokenizer_configuration = context_.tokenizer_configuration;

        // Advance sliding window indices
        if (window_end == tokens_.size())
        {
            start_token_idx_ = window_end; // Triggers end-of-stream on future invocations
        }
        else if (is_oversized)
        {
            start_token_idx_ = window_end; // Skip overlap calculations for oversized
                                           // indivisible token
        }
        else
        {
            start_token_idx_ = window_end - options_.overlap_tokens;
        }

        std::vector<Diagnostic> diagnostics;
        if (is_oversized)
        {
            diagnostics.push_back(Diagnostic {
                .severity = DiagnosticSeverity::warning,
                .code = static_cast<std::uint32_t>(DiagnosticCode::oversized_token),
                .name = "oversized_token",
                .description = "A tokenizer token exceeded max_tokens and was "
                               "preserved as one intact chunk.",
                .message = "Indivisible single token bounds exceeded configured "
                           "window max_tokens limit.",
                .source_byte_offset = chunk_start_byte });
        }

        return Result<bool>(true, std::move(diagnostics));
    }

    void cancel() noexcept override { is_cancelled_ = true; }

private:
    std::span<const std::byte> buffer_;
    ChunkOptions options_;
    std::vector<EncodedToken> tokens_;
    std::vector<std::uint32_t> token_ids_;
    ChunkInputContext context_;
    const CancellationToken* cancel_token_ { nullptr };

    std::size_t start_token_idx_ { 0 };
    std::uint32_t chunk_index_ { 0 };
    bool is_cancelled_ { false };

    std::string_view active_text_;
    std::string formatted_chunk_id_;
};

class ChunkerImpl final : public IChunker
{
public:
    Result<std::vector<ChunkResult>>
    chunk_buffer(std::span<const std::byte> buffer, const ChunkOptions& options,
        ITokenizer& tokenizer, const ChunkInputContext& context,
        const CancellationToken* cancellation = nullptr) override
    {

        auto stream_res = stream_buffer(buffer, options, tokenizer, context, cancellation);
        if (!stream_res.has_value())
        {
            return Result<std::vector<ChunkResult>>(*stream_res.error());
        }

        auto stream = std::move(stream_res).value();
        std::vector<ChunkResult> results;
        ChunkView view;

        while (true)
        {
            auto next_res = stream->next(view);
            if (!next_res.has_value())
            {
                return Result<std::vector<ChunkResult>>(*next_res.error());
            }
            if (!next_res.value())
            {
                break;
            }

            results.push_back(ChunkResult {
                .text = std::string(view.text),
                .tokens = std::vector<std::uint32_t>(view.tokens.begin(),
                    view.tokens.end()),
                .start_byte = view.start_byte,
                .end_byte = view.end_byte,
                .doc_id = std::string(view.doc_id),
                .chunk_id = std::string(view.chunk_id),
                .record_id = std::string(view.record_id),
                .tokenizer_name = std::string(view.tokenizer_name),
                .tokenizer_configuration = std::string(view.tokenizer_configuration),
                .source_start_byte = view.source_start_byte,
                .source_end_byte = view.source_end_byte,
                .source_offset_kind = view.source_offset_kind,
                .has_source_offsets = view.has_source_offsets,
                .chunk_index = view.chunk_index,
                .metadata = std::string(view.metadata) });
        }

        return Result<std::vector<ChunkResult>>(std::move(results));
    }

    Result<std::unique_ptr<IChunkStream>>
    stream_buffer(std::span<const std::byte> buffer, const ChunkOptions& options,
        ITokenizer& tokenizer, const ChunkInputContext& context,
        const CancellationToken* cancellation = nullptr) override
    {

        if (options.max_tokens == 0)
        {
            return Result<std::unique_ptr<IChunkStream>>(Error {
                .code = static_cast<std::uint32_t>(ErrorCode::invalid_argument),
                .name = std::string(constants::error_name::invalid_argument),
                .description = "A public argument is missing, malformed, or out of range.",
                .message = "max_tokens parameter must be strictly positive (> 0)." });
        }

        if (options.overlap_tokens >= options.max_tokens)
        {
            return Result<std::unique_ptr<IChunkStream>>(Error {
                .code = static_cast<std::uint32_t>(ErrorCode::invalid_configuration),
                .name = std::string(constants::error_name::invalid_configuration),
                .description = "Configuration values conflict or violate a documented rule.",
                .message = "overlap_tokens must be strictly less than max_tokens." });
        }

        std::string_view text_view(reinterpret_cast<const char*>(buffer.data()),
            buffer.size());
        auto encode_res = tokenizer.encode(text_view);
        if (!encode_res.has_value())
        {
            return Result<std::unique_ptr<IChunkStream>>(*encode_res.error());
        }

        auto stream = std::make_unique<ChunkStreamImpl>(
            buffer, options, std::move(encode_res).value(), context, cancellation);

        return Result<std::unique_ptr<IChunkStream>>(std::move(stream));
    }
};

std::unique_ptr<IChunker> create_chunker()
{
    return std::make_unique<ChunkerImpl>();
}

} // namespace fastchunk