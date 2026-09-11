#pragma once

#include "fastchunk/core.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace fastchunk
{

struct TelemetryEvent
{
    std::string metric_name;
    double value { 0.0 };
    std::string unit;
    std::string document_path;
    std::size_t chunk_count { 0 };
    std::uint64_t duration_ms { 0 };
};

class ITelemetryExporter
{
public:
    virtual ~ITelemetryExporter() = default;
    [[nodiscard]] virtual Result<void> export_batch(std::span<const TelemetryEvent> events) = 0;
};

class ITelemetryCollector
{
public:
    virtual ~ITelemetryCollector() = default;
    virtual void record(TelemetryEvent event) = 0;
    [[nodiscard]] virtual std::vector<TelemetryEvent> snapshot() const = 0;
    [[nodiscard]] virtual std::vector<Diagnostic> diagnostics() const = 0;
};

class BoundedTelemetryCollector final : public ITelemetryCollector
{
public:
    explicit BoundedTelemetryCollector(std::size_t capacity)
        : capacity_(capacity)
    {
    }

    void record(TelemetryEvent event) override;
    [[nodiscard]] std::vector<TelemetryEvent> snapshot() const override;
    [[nodiscard]] std::vector<Diagnostic> diagnostics() const override;
    [[nodiscard]] std::size_t dropped_events() const;

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<TelemetryEvent> events_;
    std::vector<Diagnostic> diagnostics_;
    std::size_t dropped_events_ { 0 };
};

class AsyncTelemetryCollector final : public ITelemetryCollector
{
public:
    AsyncTelemetryCollector(std::size_t capacity, std::unique_ptr<ITelemetryExporter> exporter,
        std::string failure_policy = "warn", std::size_t shutdown_timeout_ms = 5000);
    ~AsyncTelemetryCollector() override;

    void record(TelemetryEvent event) override;
    [[nodiscard]] std::vector<TelemetryEvent> snapshot() const override;
    [[nodiscard]] std::vector<Diagnostic> diagnostics() const override;
    void shutdown();

private:
    void worker_loop();
    void add_diagnostic(Diagnostic diagnostic);

    const std::size_t capacity_;
    const std::string failure_policy_;
    const std::size_t shutdown_timeout_ms_;
    std::unique_ptr<ITelemetryExporter> exporter_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<TelemetryEvent> queue_;
    std::vector<TelemetryEvent> delivered_events_;
    std::vector<Diagnostic> diagnostics_;
    bool stopping_ { false };
    std::thread worker_;
};

[[nodiscard]] std::unique_ptr<ITelemetryExporter> create_telemetry_exporter(
    std::string_view type, std::string_view endpoint, std::string_view token);

} // namespace fastchunk
