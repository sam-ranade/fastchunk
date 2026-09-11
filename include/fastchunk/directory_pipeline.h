#pragma once

#include "fastchunk/config.h"
#include "fastchunk/telemetry.h"

#include <filesystem>
#include <vector>

namespace fastchunk
{

struct DocumentResult
{
    std::filesystem::path path;
    std::vector<ChunkResult> chunks;
    std::vector<Diagnostic> diagnostics;
    std::optional<Error> error;
};

class DirectoryPipeline
{
public:
    [[nodiscard]] Result<std::vector<DocumentResult>> run(
        const Configuration& configuration,
        const CancellationToken* cancellation = nullptr,
        ITelemetryCollector* telemetry = nullptr) const;

    [[nodiscard]] Result<void> export_streaming(
        const Configuration& configuration,
        IStreamingExporter& exporter,
        const CancellationToken* cancellation = nullptr,
        ITelemetryCollector* telemetry = nullptr) const;

private:
    [[nodiscard]] static Result<DocumentResult> process_file(
        const std::filesystem::path& path,
        const Configuration& configuration,
        const CancellationToken* cancellation);
};

} // namespace fastchunk
