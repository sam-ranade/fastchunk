#include "fastchunk/telemetry.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#ifdef FASTCHUNK_HAS_CURL
#include <curl/curl.h>
#endif

namespace fastchunk
{
namespace
{

    Diagnostic telemetry_diagnostic(DiagnosticCode code, std::string name, std::string message)
    {
        return Diagnostic { .severity = DiagnosticSeverity::warning,
            .code = static_cast<std::uint32_t>(code),
            .name = std::move(name),
            .description = "Telemetry delivery did not complete successfully.",
            .message = std::move(message) };
    }

    std::string json_escape(std::string_view value)
    {
        std::string output = "\"";
        for (const char character : value)
        {
            if (character == '\\' || character == '"')
                output += '\\';
            if (character == '\n')
                output += "\\n";
            else if (character == '\r')
                output += "\\r";
            else
                output += character;
        }
        output += '"';
        return output;
    }

    class UnavailableTelemetryExporter final : public ITelemetryExporter
    {
    public:
        explicit UnavailableTelemetryExporter(std::string type)
            : type_(std::move(type))
        {
        }

        Result<void> export_batch(std::span<const TelemetryEvent>) override
        {
            return Result<void>(Error {
                .code = static_cast<std::uint32_t>(ErrorCode::exporter_dependency_missing),
                .name = "exporter_dependency_missing",
                .description = "A required exporter library or runtime dependency is unavailable.",
                .message = "Telemetry exporter '" + type_ + "' requires libcurl at build time." });
        }

    private:
        std::string type_;
    };

#ifdef FASTCHUNK_HAS_CURL
    class CurlTelemetryExporter final : public ITelemetryExporter
    {
    public:
        CurlTelemetryExporter(std::string type, std::string endpoint, std::string token)
            : type_(std::move(type))
            , endpoint_(std::move(endpoint))
            , token_(std::move(token))
        {
        }

        Result<void> export_batch(std::span<const TelemetryEvent> events) override
        {
            std::string body = type_ == "splunk" ? splunk_payload(events) : otlp_payload(events);
            CURL* curl = curl_easy_init();
            if (!curl)
                return failure("Unable to initialize libcurl.");
            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            std::string authorization;
            if (type_ == "splunk" && !token_.empty())
            {
                authorization = "Authorization: Splunk " + token_;
                headers = curl_slist_append(headers, authorization.c_str());
            }
            curl_easy_setopt(curl, CURLOPT_URL, endpoint_.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            const CURLcode result = curl_easy_perform(curl);
            long response = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            if (result != CURLE_OK || response < 200 || response >= 300)
                return failure(result == CURLE_OK ? "Telemetry endpoint returned HTTP " + std::to_string(response)
                                                  : curl_easy_strerror(result));
            return Result<void>();
        }

    private:
        Result<void> failure(std::string message) const
        {
            return Result<void>(Error { .code = static_cast<std::uint32_t>(ErrorCode::export_failed),
                .name = "export_failed",
                .description = "Telemetry exporter delivery failed.",
                .message = std::move(message),
                .path = endpoint_ });
        }

        static std::string otlp_payload(std::span<const TelemetryEvent> events)
        {
            std::string body = "{\"resourceMetrics\":[{\"scopeMetrics\":[{\"metrics\":[";
            for (std::size_t index = 0; index < events.size(); ++index)
            {
                if (index)
                    body += ',';
                body += "{\"name\":" + json_escape(events[index].metric_name)
                    + ",\"unit\":" + json_escape(events[index].unit)
                    + ",\"gauge\":{\"dataPoints\":[{\"asDouble\":"
                    + std::to_string(events[index].value) + "}]}}";
            }
            return body + "]}]}]}";
        }

        static std::string splunk_payload(std::span<const TelemetryEvent> events)
        {
            std::string body;
            for (const auto& event : events)
                body += "{\"event\":{\"metric\":" + json_escape(event.metric_name)
                    + ",\"value\":" + std::to_string(event.value)
                    + ",\"unit\":" + json_escape(event.unit) + "}}\n";
            return body;
        }

        std::string type_;
        std::string endpoint_;
        std::string token_;
    };
#endif

} // namespace

void BoundedTelemetryCollector::record(TelemetryEvent event)
{
    std::lock_guard lock(mutex_);
    if (events_.size() == capacity_ && capacity_ != 0)
    {
        events_.erase(events_.begin());
        ++dropped_events_;
        diagnostics_.push_back(telemetry_diagnostic(DiagnosticCode::telemetry_queue_overflow,
            "telemetry_queue_overflow", "Telemetry queue capacity was exceeded."));
    }
    if (capacity_ != 0)
        events_.push_back(std::move(event));
}

std::vector<TelemetryEvent> BoundedTelemetryCollector::snapshot() const
{
    std::lock_guard lock(mutex_);
    return events_;
}

std::vector<Diagnostic> BoundedTelemetryCollector::diagnostics() const
{
    std::lock_guard lock(mutex_);
    return diagnostics_;
}

std::size_t BoundedTelemetryCollector::dropped_events() const
{
    std::lock_guard lock(mutex_);
    return dropped_events_;
}

AsyncTelemetryCollector::AsyncTelemetryCollector(std::size_t capacity,
    std::unique_ptr<ITelemetryExporter> exporter, std::string failure_policy,
    std::size_t shutdown_timeout_ms)
    : capacity_(capacity)
    , failure_policy_(std::move(failure_policy))
    , shutdown_timeout_ms_(shutdown_timeout_ms)
    , exporter_(std::move(exporter))
    , worker_(&AsyncTelemetryCollector::worker_loop, this)
{
    (void)shutdown_timeout_ms_;
}

AsyncTelemetryCollector::~AsyncTelemetryCollector() { shutdown(); }

void AsyncTelemetryCollector::record(TelemetryEvent event)
{
    std::lock_guard lock(mutex_);
    if (stopping_)
        return;
    if (queue_.size() >= capacity_ && capacity_ != 0)
    {
        queue_.pop_front();
        diagnostics_.push_back(telemetry_diagnostic(DiagnosticCode::telemetry_queue_overflow,
            "telemetry_queue_overflow", "Telemetry queue capacity was exceeded."));
    }
    if (capacity_ != 0)
        queue_.push_back(std::move(event));
    condition_.notify_one();
}

void AsyncTelemetryCollector::worker_loop()
{
    while (true)
    {
        std::vector<TelemetryEvent> batch;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this]
                { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty())
                return;
            while (!queue_.empty() && batch.size() < 64)
            {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }
        if (exporter_)
        {
            auto result = exporter_->export_batch(batch);
            if (!result.has_value())
                add_diagnostic(telemetry_diagnostic(DiagnosticCode::telemetry_delivery_failed,
                    "telemetry_delivery_failed", result.error()->message));
        }
        std::lock_guard lock(mutex_);
        delivered_events_.insert(delivered_events_.end(), batch.begin(), batch.end());
    }
}

void AsyncTelemetryCollector::shutdown()
{
    {
        std::lock_guard lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
    }
    condition_.notify_one();
    if (worker_.joinable())
        worker_.join();
}

std::vector<TelemetryEvent> AsyncTelemetryCollector::snapshot() const
{
    std::lock_guard lock(mutex_);
    return delivered_events_;
}

std::vector<Diagnostic> AsyncTelemetryCollector::diagnostics() const
{
    std::lock_guard lock(mutex_);
    return diagnostics_;
}

void AsyncTelemetryCollector::add_diagnostic(Diagnostic diagnostic)
{
    std::lock_guard lock(mutex_);
    diagnostics_.push_back(std::move(diagnostic));
}

std::unique_ptr<ITelemetryExporter> create_telemetry_exporter(
    std::string_view type, std::string_view endpoint, std::string_view token)
{
    if (type.empty() || type == "none")
        return nullptr;
#ifdef FASTCHUNK_HAS_CURL
    return std::make_unique<CurlTelemetryExporter>(std::string(type),
        std::string(endpoint), std::string(token));
#else
    return std::make_unique<UnavailableTelemetryExporter>(std::string(type));
#endif
}

} // namespace fastchunk
