#include "fastchunk/config.h"
#include "fastchunk/constants.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <sstream>

namespace fastchunk
{
namespace
{

    Error config_error(ErrorCode code, std::string message)
    {
        return Error { .code = static_cast<std::uint32_t>(code),
            .name = std::string(constants::error_name::invalid_configuration),
            .description = "Configuration values conflict or violate a documented rule.",
            .message = std::move(message) };
    }

    std::string scalar(const YAML::Node& node, std::string_view key,
        std::string fallback = {})
    {
        const auto value = node[std::string(key)];
        return value ? value.as<std::string>() : fallback;
    }

    bool boolean(const YAML::Node& node, std::string_view key, bool fallback)
    {
        const auto value = node[std::string(key)];
        return value ? value.as<bool>() : fallback;
    }

    std::optional<std::size_t> size_value(const YAML::Node& node, std::string_view key)
    {
        const auto value = node[std::string(key)];
        return value && !value.IsNull() ? std::optional<std::size_t>(value.as<std::size_t>())
                                        : std::nullopt;
    }

    Result<void> reject_unknown_keys(const YAML::Node& node,
        std::initializer_list<std::string_view> allowed,
        std::string_view section)
    {
        for (const auto& entry : node)
        {
            const auto key = entry.first.as<std::string>();
            const auto found = std::find(allowed.begin(), allowed.end(), key);
            if (found == allowed.end())
                return Result<void>(config_error(ErrorCode::invalid_configuration,
                    "Unknown configuration key in " + std::string(section) + ": " + key));
        }
        return Result<void>();
    }

} // namespace

Configuration default_configuration()
{
    Configuration configuration;
    configuration.tokenizer.type = "whitespace";
    configuration.chunking.max_tokens = std::nullopt;
    configuration.chunking.overlap_tokens = std::nullopt;
    return configuration;
}

Result<void> Configuration::validate() const
{
    if (tokenizer.type.empty())
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "tokenizer.type is required."));
    if (tokenizer.type != constants::tokenizer::tiktoken && tokenizer.type != constants::tokenizer::cl100k_base && tokenizer.type != constants::tokenizer::o200k_base && tokenizer.type != constants::tokenizer::huggingface && tokenizer.type != constants::tokenizer::huggingface_alias && tokenizer.type != "whitespace")
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "Unknown tokenizer.type: " + tokenizer.type));
    if (!chunking.max_tokens || *chunking.max_tokens == 0)
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "chunking.max_tokens is required and must be greater than zero."));
    if (!chunking.overlap_tokens)
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "chunking.overlap_tokens is required."));
    if (*chunking.overlap_tokens >= *chunking.max_tokens)
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "chunking.overlap_tokens must be less than max_tokens."));
    if (input.path.empty())
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "input.path is required."));
    if (input.mode == InputMode::zero_copy && input.invalid_utf8 != InvalidUtf8Policy::error)
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "zero_copy mode requires invalid_utf8=error."));
    if (concurrency.result_order != "ordered" && concurrency.result_order != "unordered")
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "concurrency.result_order must be ordered or unordered."));
    if (concurrency.cpu_affinity.mode != "none"
        && concurrency.cpu_affinity.mode != "explicit"
        && concurrency.cpu_affinity.mode != "isolated")
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "concurrency.cpu_affinity.mode must be none, explicit, or isolated."));
    if (concurrency.cpu_affinity.mode == "explicit"
        && concurrency.cpu_affinity.cpus.empty())
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "Explicit CPU affinity requires at least one CPU."));
    if (concurrency.cpu_affinity.mode != "none")
        return Result<void>(config_error(ErrorCode::unsupported_option,
            "CPU affinity is not available in this build."));
    if (export_options.on_error != "fail_batch" && export_options.on_error != "continue")
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "export.on_error must be fail_batch or continue."));
    if (export_options.retries.max_attempts == 0 || export_options.retries.initial_delay_ms > export_options.retries.max_delay_ms)
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "export.retries values are invalid."));
    if (telemetry.exporter != "none" && telemetry.exporter != "otlp" && telemetry.exporter != "splunk")
        return Result<void>(config_error(ErrorCode::unsupported_option,
            "Unknown telemetry.exporter: " + telemetry.exporter));
    if (telemetry.on_failure != "warn" && telemetry.on_failure != "drop")
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "telemetry.on_failure must be warn or drop."));
    if (telemetry.enabled && telemetry.exporter != "none" && telemetry.endpoint.empty())
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "telemetry.endpoint is required when telemetry is enabled."));
    if (!input.restricted_root.empty())
        return Result<void>(config_error(ErrorCode::unsupported_option,
            "input.restricted_root requires race-resistant root-anchored I/O, which is unavailable in this build."));
    if (export_options.type != "none" && export_options.type != "ndjson")
        return Result<void>(config_error(ErrorCode::unsupported_option,
            "Unknown export.type: " + export_options.type));
    if (export_options.type == "ndjson" && export_options.path.empty())
        return Result<void>(config_error(ErrorCode::invalid_configuration,
            "export.path is required when export.type is ndjson."));
    return Result<void>();
}

std::string Configuration::effective_json() const
{
    return std::format(
        "{{\"input_path\":\"{}\",\"input_mode\":\"{}\",\"tokenizer\":\"{}\",\"max_tokens\":{},\"overlap_tokens\":{},\"export_type\":\"{}\",\"retry_attempts\":{},\"telemetry_enabled\":{},\"telemetry_exporter\":\"{}\"}}",
        input.path,
        input.mode == InputMode::copy ? "copy" : "zero_copy",
        tokenizer.type,
        *chunking.max_tokens,
        *chunking.overlap_tokens,
        export_options.type,
        export_options.retries.max_attempts,
        telemetry.enabled,
        telemetry.exporter);
}

Result<Configuration> load_configuration(std::string_view path)
{
    Configuration configuration = default_configuration();
    try
    {
        const YAML::Node root = YAML::LoadFile(std::string(path));
        for (const auto& entry : root)
        {
            const auto key = entry.first.as<std::string>();
            if (key != "schema_version" && key != "input" && key != "tokenizer" && key != "chunking" && key != "concurrency" && key != "limits" && key != "export" && key != "telemetry")
                return Result<Configuration>(config_error(ErrorCode::invalid_configuration,
                    "Unknown configuration key: " + key));
        }
        if (root["schema_version"])
            configuration.schema_version = root["schema_version"].as<int>();
        if (configuration.schema_version != 1)
            return Result<Configuration>(config_error(ErrorCode::unsupported_option,
                "Unsupported configuration schema_version."));

        const auto input = root["input"];
        if (input)
        {
            auto keys = reject_unknown_keys(input,
                { "path", "restricted_root", "mode", "invalid_utf8", "recursive",
                    "include_hidden", "record_mode", "record_id_field", "on_malformed_record" },
                "input");
            if (!keys.has_value())
                return Result<Configuration>(*keys.error());
            configuration.input.path = scalar(input, "path");
            configuration.input.restricted_root = scalar(input, "restricted_root");
            const auto mode = scalar(input, "mode", "zero_copy");
            configuration.input.mode = mode == "copy" ? InputMode::copy : InputMode::zero_copy;
            const auto utf8 = scalar(input, "invalid_utf8", "error");
            if (utf8 == "replace")
                configuration.input.invalid_utf8 = InvalidUtf8Policy::replace;
            else if (utf8 == "skip")
                configuration.input.invalid_utf8 = InvalidUtf8Policy::skip;
            configuration.input.recursive = boolean(input, "recursive", false);
            configuration.input.include_hidden = boolean(input, "include_hidden", false);
            configuration.input.record_id_field = scalar(input, "record_id_field");
            configuration.input.record_mode = scalar(input, "record_mode", "none") == "ndjson"
                ? InputOptions::RecordMode::ndjson
                : InputOptions::RecordMode::none;
            configuration.input.on_malformed_record = scalar(input, "on_malformed_record", "error") == "skip"
                ? InputOptions::MalformedRecordPolicy::skip
                : InputOptions::MalformedRecordPolicy::error;
        }
        const auto tokenizer = root["tokenizer"];
        if (tokenizer)
        {
            auto keys = reject_unknown_keys(tokenizer,
                { "type", "encoding", "tokenizer_path" }, "tokenizer");
            if (!keys.has_value())
                return Result<Configuration>(*keys.error());
            configuration.tokenizer.type = scalar(tokenizer, "type", configuration.tokenizer.type);
            configuration.tokenizer.encoding = scalar(tokenizer, "encoding");
            configuration.tokenizer.tokenizer_path = scalar(tokenizer, "tokenizer_path");
        }
        const auto chunking = root["chunking"];
        if (chunking)
        {
            auto keys = reject_unknown_keys(chunking,
                { "max_tokens", "overlap_tokens", "metadata_collision" }, "chunking");
            if (!keys.has_value())
                return Result<Configuration>(*keys.error());
            configuration.chunking.max_tokens = size_value(chunking, "max_tokens");
            configuration.chunking.overlap_tokens = size_value(chunking, "overlap_tokens");
            if (scalar(chunking, "metadata_collision", "error") == "ignore")
                configuration.chunking.metadata_collision = ChunkOptions::MetadataCollisionPolicy::ignore;
        }
        const auto export_node = root["export"];
        if (export_node)
        {
            auto keys = reject_unknown_keys(export_node,
                { "type", "path", "on_error", "retries" }, "export");
            if (!keys.has_value())
                return Result<Configuration>(*keys.error());
            configuration.export_options.type = scalar(export_node, "type", "ndjson");
            configuration.export_options.path = scalar(export_node, "path");
            configuration.export_options.on_error = scalar(export_node, "on_error", "fail_batch");
            const auto retries = export_node["retries"];
            if (retries)
            {
                auto retry_keys = reject_unknown_keys(retries,
                    { "max_attempts", "initial_delay_ms", "max_delay_ms" },
                    "export.retries");
                if (!retry_keys.has_value())
                    return Result<Configuration>(*retry_keys.error());
                if (auto value = size_value(retries, "max_attempts"))
                    configuration.export_options.retries.max_attempts = *value;
                if (auto value = size_value(retries, "initial_delay_ms"))
                    configuration.export_options.retries.initial_delay_ms = *value;
                if (auto value = size_value(retries, "max_delay_ms"))
                    configuration.export_options.retries.max_delay_ms = *value;
            }
        }
        const auto concurrency = root["concurrency"];
        if (concurrency)
        {
            auto keys = reject_unknown_keys(concurrency,
                { "workers", "result_order", "cpu_affinity" }, "concurrency");
            if (!keys.has_value())
                return Result<Configuration>(*keys.error());
            if (auto workers = size_value(concurrency, "workers"))
                configuration.concurrency.workers = *workers;
            configuration.concurrency.result_order = scalar(concurrency, "result_order", "ordered");
            const auto affinity = concurrency["cpu_affinity"];
            if (affinity)
            {
                auto affinity_keys = reject_unknown_keys(affinity,
                    { "mode", "cpus", "required" }, "concurrency.cpu_affinity");
                if (!affinity_keys.has_value())
                    return Result<Configuration>(*affinity_keys.error());
                configuration.concurrency.cpu_affinity.mode = scalar(affinity, "mode", "none");
                configuration.concurrency.cpu_affinity.required = boolean(affinity, "required", false);
                const auto cpus = affinity["cpus"];
                if (cpus)
                    for (const auto& cpu : cpus)
                        configuration.concurrency.cpu_affinity.cpus.push_back(cpu.as<std::size_t>());
            }
        }
        const auto limits = root["limits"];
        if (limits)
        {
            auto keys = reject_unknown_keys(limits,
                { "max_file_size_mb", "max_record_size_mb", "max_metadata_size_mb",
                    "max_chunks_per_document", "max_files_per_directory", "max_memory_mb",
                    "max_tokenizer_model_size_mb" }, "limits");
            if (!keys.has_value())
                return Result<Configuration>(*keys.error());
            configuration.limits.max_file_size_mb = size_value(limits, "max_file_size_mb");
            configuration.limits.max_record_size_mb = size_value(limits, "max_record_size_mb");
            configuration.limits.max_metadata_size_mb = size_value(limits, "max_metadata_size_mb");
            configuration.limits.max_chunks_per_document = size_value(limits, "max_chunks_per_document");
            configuration.limits.max_files_per_directory = size_value(limits, "max_files_per_directory");
            configuration.limits.max_memory_mb = size_value(limits, "max_memory_mb");
            configuration.limits.max_tokenizer_model_size_mb = size_value(limits, "max_tokenizer_model_size_mb");
        }
        const auto telemetry = root["telemetry"];
        if (telemetry)
        {
            auto keys = reject_unknown_keys(telemetry,
                { "enabled", "exporter", "queue_size", "on_failure",
                    "include_document_attributes", "endpoint", "token_env",
                    "shutdown_timeout_ms" }, "telemetry");
            if (!keys.has_value())
                return Result<Configuration>(*keys.error());
            configuration.telemetry.enabled = boolean(telemetry, "enabled", false);
            configuration.telemetry.exporter = scalar(telemetry, "exporter", "none");
            configuration.telemetry.on_failure = scalar(telemetry, "on_failure", "warn");
            configuration.telemetry.endpoint = scalar(telemetry, "endpoint");
            configuration.telemetry.token_env = scalar(telemetry, "token_env");
            configuration.telemetry.include_document_attributes = boolean(telemetry, "include_document_attributes", false);
            if (auto queue_size = size_value(telemetry, "queue_size"))
                configuration.telemetry.queue_size = *queue_size;
            if (auto timeout = size_value(telemetry, "shutdown_timeout_ms"))
                configuration.telemetry.shutdown_timeout_ms = *timeout;
        }
    }
    catch (const YAML::Exception& error)
    {
        return Result<Configuration>(Error {
            .code = static_cast<std::uint32_t>(ErrorCode::configuration_parse_failed),
            .name = "configuration_parse_failed",
            .description = "The configuration file cannot be parsed.",
            .message = error.what(),
            .path = std::string(path) });
    }
    auto validation = configuration.validate();
    if (!validation.has_value())
        return Result<Configuration>(*validation.error());
    return Result<Configuration>(std::move(configuration));
}

} // namespace fastchunk
