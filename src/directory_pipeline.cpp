#include "fastchunk/directory_pipeline.h"

#include "fastchunk/factory.h"
#include "parallel_chunker.h"
#include "fastchunk/tokenizers.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <thread>

namespace fastchunk
{
namespace
{

    Error pipeline_error(ErrorCode code, std::string message, std::string path = {})
    {
        return Error { .code = static_cast<std::uint32_t>(code),
            .name = code == ErrorCode::cancelled ? "cancelled" : "io_error",
            .description = "Directory pipeline execution failed.",
            .message = std::move(message),
            .path = std::move(path) };
    }

    bool hidden_path(const std::filesystem::path& path)
    {
        for (const auto& component : path)
            if (!component.empty() && component.string().front() == '.')
                return true;
        return false;
    }

} // namespace

Result<DocumentResult> DirectoryPipeline::process_file(
    const std::filesystem::path& path,
    const Configuration& configuration,
    const CancellationToken* cancellation)
{
    DocumentResult document { .path = path };
    if (cancellation && cancellation->is_cancelled())
    {
        document.error = pipeline_error(ErrorCode::cancelled,
            "Directory pipeline cancelled.", path.string());
        return Result<DocumentResult>(std::move(document));
    }

    if (configuration.limits.max_file_size_mb)
    {
        std::error_code status;
        const auto size = std::filesystem::file_size(path, status);
        const auto limit = *configuration.limits.max_file_size_mb * 1024ULL * 1024ULL;
        if (status || size > limit)
        {
            document.error = pipeline_error(ErrorCode::resource_limit_exceeded,
                "Input file exceeds max_file_size_mb.", path.string());
            return Result<DocumentResult>(std::move(document));
        }
    }

    auto reader = create_mmap_reader();
    InputOptions input_options {
        .mode = configuration.input.mode,
        .invalid_utf8 = configuration.input.invalid_utf8,
        .record_mode = configuration.input.record_mode,
        .malformed_record = configuration.input.on_malformed_record,
        .record_id_field = configuration.input.record_id_field
    };
    auto input = reader->open(path, input_options);
    if (!input.has_value())
    {
        document.error = *input.error();
        return Result<DocumentResult>(std::move(document));
    }
    document.diagnostics.assign(input.diagnostics().begin(), input.diagnostics().end());
    if (configuration.limits.max_memory_mb && input.value().data.size() > *configuration.limits.max_memory_mb * 1024ULL * 1024ULL)
    {
        document.error = pipeline_error(ErrorCode::resource_limit_exceeded,
            "Input exceeds max_memory_mb.", path.string());
        return Result<DocumentResult>(std::move(document));
    }
    if (configuration.limits.max_record_size_mb && configuration.input.record_mode == InputOptions::RecordMode::ndjson)
    {
        const auto limit = *configuration.limits.max_record_size_mb * 1024ULL * 1024ULL;
        const auto bytes = input.value().data;
        std::size_t record_start = 0;
        for (std::size_t index = 0; index <= bytes.size(); ++index)
        {
            if (index == bytes.size() || bytes[index] == std::byte { '\n' })
            {
                if (index - record_start > limit)
                {
                    document.error = pipeline_error(ErrorCode::resource_limit_exceeded,
                        "NDJSON record exceeds max_record_size_mb.", path.string());
                    return Result<DocumentResult>(std::move(document));
                }
                record_start = index + 1;
            }
        }
    }

    const auto tokenizer_configuration = configuration.tokenizer.encoding.empty()
        ? configuration.tokenizer.tokenizer_path
        : configuration.tokenizer.encoding;
    if (configuration.limits.max_tokenizer_model_size_mb && !configuration.tokenizer.tokenizer_path.empty())
    {
        std::error_code model_status;
        const auto model_size = std::filesystem::file_size(configuration.tokenizer.tokenizer_path, model_status);
        if (model_status || model_size > *configuration.limits.max_tokenizer_model_size_mb * 1024ULL * 1024ULL)
        {
            document.error = pipeline_error(ErrorCode::resource_limit_exceeded,
                "Tokenizer model exceeds max_tokenizer_model_size_mb.", configuration.tokenizer.tokenizer_path);
            return Result<DocumentResult>(std::move(document));
        }
    }
    auto tokenizer = create_tokenizer_from_name(configuration.tokenizer.type,
        tokenizer_configuration);
    if (!tokenizer.has_value())
    {
        document.error = *tokenizer.error();
        return Result<DocumentResult>(std::move(document));
    }
    ChunkOptions options {
        .max_tokens = *configuration.chunking.max_tokens,
        .overlap_tokens = *configuration.chunking.overlap_tokens,
        .metadata_collision = configuration.chunking.metadata_collision
    };
    ChunkInputContext context {
        .doc_id = path.string(),
        .tokenizer_name = configuration.tokenizer.type,
        .tokenizer_configuration = configuration.effective_json()
    };
    if (configuration.limits.max_metadata_size_mb && context.metadata.size() > *configuration.limits.max_metadata_size_mb * 1024ULL * 1024ULL)
    {
        document.error = pipeline_error(ErrorCode::resource_limit_exceeded,
            "Metadata exceeds max_metadata_size_mb.", path.string());
        return Result<DocumentResult>(std::move(document));
    }
    const auto workers = configuration.concurrency.workers == 0
        ? std::thread::hardware_concurrency()
        : configuration.concurrency.workers;
    auto chunks = [&]() -> Result<std::vector<ChunkResult>>
    {
        if (workers > 1 && input.value().data.size() >= 1024 * 1024)
            return ParallelChunker::chunk_buffer_parallel(input.value().data,
                options, *tokenizer.value(), context, *create_chunker(), workers);
        return create_chunker()->chunk_buffer(input.value().data, options,
            *tokenizer.value(), context, cancellation);
    }();
    if (!chunks.has_value())
    {
        document.error = *chunks.error();
        return Result<DocumentResult>(std::move(document));
    }
    if (configuration.limits.max_chunks_per_document && chunks.value().size() > *configuration.limits.max_chunks_per_document)
    {
        document.error = pipeline_error(ErrorCode::resource_limit_exceeded,
            "Document exceeds max_chunks_per_document.", path.string());
        return Result<DocumentResult>(std::move(document));
    }
    document.diagnostics.insert(document.diagnostics.end(),
        chunks.diagnostics().begin(), chunks.diagnostics().end());
    document.chunks = std::move(chunks).value();
    return Result<DocumentResult>(std::move(document));
}

Result<std::vector<DocumentResult>> DirectoryPipeline::run(
    const Configuration& configuration,
    const CancellationToken* cancellation,
    ITelemetryCollector* telemetry) const
{
    auto validation = configuration.validate();
    if (!validation.has_value())
        return Result<std::vector<DocumentResult>>(*validation.error());

    const std::filesystem::path root(configuration.input.path);
    std::error_code status;
    if (!std::filesystem::is_directory(root, status))
    {
        const auto started = std::chrono::steady_clock::now();
        auto document = process_file(root, configuration, cancellation);
        if (!document.has_value())
            return Result<std::vector<DocumentResult>>(*document.error());
        if (telemetry)
        {
            telemetry->record({ .metric_name = "fastchunk_bytes_processed_total",
                .value = static_cast<double>(document.value().chunks.size()),
                .unit = "By",
                .document_path = root.string() });
            telemetry->record({ .metric_name = "fastchunk_chunks_generated_total",
                .value = static_cast<double>(document.value().chunks.size()),
                .unit = "chunks",
                .document_path = root.string(),
                .chunk_count = document.value().chunks.size(),
                .duration_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()) });
        }
        return Result<std::vector<DocumentResult>>(
            std::vector<DocumentResult> { std::move(document).value() });
    }

    std::vector<std::filesystem::path> paths;
    auto add_path = [&](const auto& entry)
    {
        if (!entry.is_regular_file(status))
            return;
        if (!configuration.input.include_hidden && hidden_path(entry.path()))
            return;
        paths.push_back(entry.path());
    };
    if (configuration.input.recursive)
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
            add_path(entry);
    }
    else
    {
        for (const auto& entry : std::filesystem::directory_iterator(root))
            add_path(entry);
    }
    std::sort(paths.begin(), paths.end());
    if (configuration.limits.max_files_per_directory && paths.size() > *configuration.limits.max_files_per_directory)
        return Result<std::vector<DocumentResult>>(pipeline_error(
            ErrorCode::resource_limit_exceeded,
            "Directory exceeds max_files_per_directory.", root.string()));

    const std::size_t worker_count = configuration.concurrency.workers == 0
        ? std::max<std::size_t>(1, std::thread::hardware_concurrency())
        : configuration.concurrency.workers;
    std::vector<DocumentResult> results;
    results.reserve(paths.size());
    for (std::size_t start = 0; start < paths.size(); start += worker_count)
    {
        std::vector<std::future<Result<DocumentResult>>> batch;
        const auto end = std::min(paths.size(), start + worker_count);
        for (std::size_t index = start; index < end; ++index)
        {
            if (cancellation && cancellation->is_cancelled())
                return Result<std::vector<DocumentResult>>(pipeline_error(
                    ErrorCode::cancelled, "Directory pipeline cancelled.", root.string()));
            batch.push_back(std::async(std::launch::async, process_file, paths[index],
                std::cref(configuration), cancellation));
        }
        for (auto& future : batch)
        {
            auto document = future.get();
            if (!document.has_value())
                return Result<std::vector<DocumentResult>>(*document.error());
            if (telemetry)
            {
                std::error_code file_status;
                const auto bytes = std::filesystem::file_size(document.value().path, file_status);
                telemetry->record({ .metric_name = "fastchunk_bytes_processed_total",
                    .value = file_status ? 0.0 : static_cast<double>(bytes),
                    .unit = "By",
                    .document_path = document.value().path.string() });
                telemetry->record({ .metric_name = "fastchunk_chunks_generated_total",
                    .value = static_cast<double>(document.value().chunks.size()),
                    .unit = "chunks",
                    .document_path = document.value().path.string(),
                    .chunk_count = document.value().chunks.size() });
            }
            results.push_back(std::move(document).value());
        }
    }
    if (configuration.concurrency.result_order == "ordered")
        std::sort(results.begin(), results.end(), [](const auto& left, const auto& right)
            { return left.path < right.path; });
    return Result<std::vector<DocumentResult>>(std::move(results));
}

Result<void> DirectoryPipeline::export_streaming(
    const Configuration& configuration,
    IStreamingExporter& exporter,
    const CancellationToken* cancellation,
    ITelemetryCollector* telemetry) const
{
    auto validation = configuration.validate();
    if (!validation.has_value())
        return validation;

    const std::filesystem::path root(configuration.input.path);
    std::vector<std::filesystem::path> paths;
    std::error_code status;
    if (std::filesystem::is_directory(root, status))
    {
        auto add_path = [&](const auto& entry)
        {
            if (!entry.is_regular_file(status))
                return;
            if (!configuration.input.include_hidden && hidden_path(entry.path()))
                return;
            paths.push_back(entry.path());
        };
        if (configuration.input.recursive)
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
                add_path(entry);
        else
            for (const auto& entry : std::filesystem::directory_iterator(root))
                add_path(entry);
        std::sort(paths.begin(), paths.end());
    }
    else
    {
        paths.push_back(root);
    }

    for (const auto& path : paths)
    {
        if (cancellation && cancellation->is_cancelled())
            return Result<void>(pipeline_error(ErrorCode::cancelled,
                "Directory pipeline cancelled.", root.string()));
        auto reader = create_mmap_reader();
        InputOptions input_options {
            .mode = configuration.input.mode,
            .invalid_utf8 = configuration.input.invalid_utf8,
            .record_mode = configuration.input.record_mode,
            .malformed_record = configuration.input.on_malformed_record,
            .record_id_field = configuration.input.record_id_field
        };
        auto input = reader->open(path, input_options);
        if (!input.has_value())
            return Result<void>(*input.error());
        if (configuration.limits.max_memory_mb && input.value().data.size() > *configuration.limits.max_memory_mb * 1024ULL * 1024ULL)
            return Result<void>(pipeline_error(ErrorCode::resource_limit_exceeded,
                "Input exceeds max_memory_mb.", path.string()));
        const auto tokenizer_configuration = configuration.tokenizer.encoding.empty()
            ? configuration.tokenizer.tokenizer_path
            : configuration.tokenizer.encoding;
        if (configuration.limits.max_tokenizer_model_size_mb && !configuration.tokenizer.tokenizer_path.empty())
        {
            std::error_code model_status;
            const auto model_size = std::filesystem::file_size(configuration.tokenizer.tokenizer_path, model_status);
            if (model_status || model_size > *configuration.limits.max_tokenizer_model_size_mb * 1024ULL * 1024ULL)
                return Result<void>(pipeline_error(ErrorCode::resource_limit_exceeded,
                    "Tokenizer model exceeds max_tokenizer_model_size_mb.", configuration.tokenizer.tokenizer_path));
        }
        auto tokenizer = create_tokenizer_from_name(configuration.tokenizer.type,
            tokenizer_configuration);
        if (!tokenizer.has_value())
            return Result<void>(*tokenizer.error());
        ChunkOptions options {
            .max_tokens = *configuration.chunking.max_tokens,
            .overlap_tokens = *configuration.chunking.overlap_tokens,
            .metadata_collision = configuration.chunking.metadata_collision
        };
        ChunkInputContext context {
            .doc_id = path.string(),
            .tokenizer_name = configuration.tokenizer.type,
            .tokenizer_configuration = configuration.effective_json()
        };
        ExportMetadata metadata {
            .doc_id = path.string(),
            .source_path = path.string(),
            .tokenizer_name = configuration.tokenizer.type,
            .tokenizer_configuration = configuration.effective_json(),
            .effective_configuration = configuration.effective_json()
        };
        const auto started = std::chrono::steady_clock::now();
        const auto workers = configuration.concurrency.workers == 0
            ? std::thread::hardware_concurrency()
            : configuration.concurrency.workers;
        if (configuration.input.record_mode == InputOptions::RecordMode::none
            && workers > 1 && input.value().data.size() >= 1024 * 1024)
        {
            if (auto* owned_exporter = dynamic_cast<IExporter*>(&exporter))
            {
                auto chunks = ParallelChunker::chunk_buffer_parallel(
                    input.value().data, options, *tokenizer.value(), context,
                    *create_chunker(), workers);
                if (!chunks.has_value())
                    return Result<void>(*chunks.error());
                auto exported = owned_exporter->export_chunks(chunks.value(), metadata);
                if (!exported.has_value())
                    return exported;
                continue;
            }
        }
        auto stream = create_chunker()->stream_buffer(input.value().data, options,
            *tokenizer.value(), context, cancellation);
        if (!stream.has_value())
            return Result<void>(*stream.error());
        auto exported = exporter.export_stream(*stream.value(), metadata, cancellation);
        if (!exported.has_value())
            return exported;
        if (telemetry)
            telemetry->record({ .metric_name = "fastchunk_processing_duration_seconds",
                .value = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()) / 1000.0,
                .unit = "s",
                .document_path = path.string(),
                .duration_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()) });
        if (telemetry)
            telemetry->record({ .metric_name = "fastchunk_bytes_processed_total",
                .value = static_cast<double>(input.value().data.size()),
                .unit = "By",
                .document_path = path.string() });
    }
    return Result<void>();
}

} // namespace fastchunk
