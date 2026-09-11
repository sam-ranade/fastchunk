#include "fastchunk/config.h"
#include "fastchunk/directory_pipeline.h"
#include "fastchunk/ndjson_exporter.h"
#include "fastchunk/telemetry.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

void usage()
{
    std::cerr << "Usage: fastchunk chunk [input] --config FILE [options]\n"
                 "Options: --tokenizer NAME --max-tokens N --overlap-tokens N\n"
                 "         --input-mode zero_copy|copy --invalid-utf8 error|replace|skip\n"
                 "         --export FILE\n";
}

void version()
{
    std::cout << "fastchunk 0.1.0\n";
}

bool take_value(int& index, int argc, char** argv, std::string& value)
{
    if (index + 1 >= argc)
        return false;
    value = argv[++index];
    return true;
}

std::string json_escape(std::string_view value)
{
    std::string escaped = "\"";
    for (const char character : value)
    {
        if (character == '\\' || character == '"')
            escaped += '\\';
        if (character == '\n')
            escaped += "\\n";
        else if (character == '\r')
            escaped += "\\r";
        else
            escaped += character;
    }
    escaped += '"';
    return escaped;
}

void print_error(const fastchunk::Error& error, bool json)
{
    if (!json)
    {
        std::cerr << error.message << "\n";
        return;
    }
    std::cerr << "{\"code\":" << error.code
              << ",\"name\":" << json_escape(error.name)
              << ",\"description\":" << json_escape(error.description)
              << ",\"message\":" << json_escape(error.message)
              << ",\"path\":" << json_escape(error.path)
              << ",\"details_json\":" << json_escape(error.details_json) << "}\n";
}

fastchunk::Result<fastchunk::Configuration> parse(int argc, char** argv)
{
    if (argc >= 2 && std::string_view(argv[1]) == "--help")
    {
        usage();
        return fastchunk::Result<fastchunk::Configuration>(fastchunk::Error {
            .code = 0, .message = "help" });
    }
    if (argc >= 2 && std::string_view(argv[1]) == "--version")
    {
        version();
        return fastchunk::Result<fastchunk::Configuration>(fastchunk::Error {
            .code = 0, .message = "version" });
    }
    if (argc < 2 || std::string_view(argv[1]) != "chunk")
    {
        usage();
        return fastchunk::Result<fastchunk::Configuration>(fastchunk::Error {
            .code = static_cast<std::uint32_t>(fastchunk::ErrorCode::invalid_argument),
            .name = "invalid_argument",
            .description = "A public argument is missing, malformed, or out of range.",
            .message = "Expected the 'chunk' command." });
    }

    std::string config_path;
    std::string positional_path;
    std::string tokenizer;
    std::string input_mode;
    std::string invalid_utf8;
    std::string export_path;
    std::optional<std::size_t> max_tokens;
    std::optional<std::size_t> overlap_tokens;
    std::optional<std::size_t> workers;
    std::string record_mode;
    std::string record_id_field;
    std::string malformed_record;
    std::string result_order;
    bool recursive = false;
    bool include_hidden = false;
    bool json_errors = false;

    for (int index = 2; index < argc; ++index)
    {
        const std::string_view argument = argv[index];
        std::string value;
        if (argument == "--config" && take_value(index, argc, argv, value))
            config_path = value;
        else if (argument == "--tokenizer" && take_value(index, argc, argv, value))
            tokenizer = value;
        else if (argument == "--max-tokens" && take_value(index, argc, argv, value))
            max_tokens = std::stoull(value);
        else if (argument == "--overlap-tokens" && take_value(index, argc, argv, value))
            overlap_tokens = std::stoull(value);
        else if (argument == "--input-mode" && take_value(index, argc, argv, value))
            input_mode = value;
        else if (argument == "--invalid-utf8" && take_value(index, argc, argv, value))
            invalid_utf8 = value;
        else if (argument == "--export" && take_value(index, argc, argv, value))
            export_path = value;
        else if (argument == "--workers" && take_value(index, argc, argv, value))
            workers = std::stoull(value);
        else if (argument == "--result-order" && take_value(index, argc, argv, value))
            result_order = value;
        else if (argument == "--record-mode" && take_value(index, argc, argv, value))
            record_mode = value;
        else if (argument == "--record-id-field" && take_value(index, argc, argv, value))
            record_id_field = value;
        else if (argument == "--on-malformed-record" && take_value(index, argc, argv, value))
            malformed_record = value;
        else if (argument == "--recursive")
            recursive = true;
        else if (argument == "--include-hidden")
            include_hidden = true;
        else if (argument == "--json-errors")
            json_errors = true;
        else if (!argument.starts_with("--") && positional_path.empty())
            positional_path = argument;
        else
        {
            usage();
            return fastchunk::Result<fastchunk::Configuration>(fastchunk::Error {
                .code = static_cast<std::uint32_t>(fastchunk::ErrorCode::invalid_argument),
                .name = "invalid_argument",
                .description = "A public argument is missing, malformed, or out of range.",
                .message = "Unknown or incomplete command-line option: " + std::string(argument) });
        }
    }

    fastchunk::Configuration configuration = fastchunk::default_configuration();
    if (!config_path.empty())
    {
        auto loaded = fastchunk::load_configuration(config_path);
        if (!loaded.has_value())
            return loaded;
        configuration = std::move(loaded).value();
    }
    if (!positional_path.empty())
        configuration.input.path = positional_path;
    if (!tokenizer.empty())
        configuration.tokenizer.type = tokenizer;
    if (max_tokens)
        configuration.chunking.max_tokens = max_tokens;
    if (overlap_tokens)
        configuration.chunking.overlap_tokens = overlap_tokens;
    if (!input_mode.empty())
        configuration.input.mode = input_mode == "copy"
            ? fastchunk::InputMode::copy
            : fastchunk::InputMode::zero_copy;
    if (!invalid_utf8.empty())
    {
        configuration.input.invalid_utf8 = invalid_utf8 == "replace"
            ? fastchunk::InvalidUtf8Policy::replace
            : invalid_utf8 == "skip" ? fastchunk::InvalidUtf8Policy::skip
                                     : fastchunk::InvalidUtf8Policy::error;
    }
    if (!export_path.empty())
        configuration.export_options.path = export_path;
    if (workers)
        configuration.concurrency.workers = *workers;
    if (!result_order.empty())
        configuration.concurrency.result_order = result_order;
    if (recursive)
        configuration.input.recursive = true;
    if (include_hidden)
        configuration.input.include_hidden = true;
    if (!record_mode.empty())
        configuration.input.record_mode = record_mode == "ndjson"
            ? fastchunk::InputOptions::RecordMode::ndjson
            : fastchunk::InputOptions::RecordMode::none;
    if (!record_id_field.empty())
        configuration.input.record_id_field = record_id_field;
    if (!malformed_record.empty())
        configuration.input.on_malformed_record = malformed_record == "skip"
            ? fastchunk::InputOptions::MalformedRecordPolicy::skip
            : fastchunk::InputOptions::MalformedRecordPolicy::error;
    configuration.json_errors = json_errors;
    auto validation = configuration.validate();
    if (!validation.has_value())
        return fastchunk::Result<fastchunk::Configuration>(*validation.error());
    return fastchunk::Result<fastchunk::Configuration>(std::move(configuration));
}

} // namespace

int main(int argc, char** argv)
{
    const bool json_errors = std::find_if(argv, argv + argc,
                                 [](const char* value)
                                 { return std::string_view(value) == "--json-errors"; })
        != argv + argc;
    auto parsed = parse(argc, argv);
    if (!parsed.has_value())
    {
        if (parsed.error()->code == 0)
            return 0;
        print_error(*parsed.error(), json_errors);
        return 2;
    }
    auto configuration = std::move(parsed).value();
    if (configuration.export_options.type == "none")
        return 0;
    std::ofstream output(configuration.export_options.path);
    if (!output)
    {
        std::cerr << "Unable to open export path: " << configuration.export_options.path << "\n";
        return 1;
    }
    fastchunk::NdjsonExporter exporter(output,
        configuration.export_options.retries.max_attempts,
        configuration.export_options.retries.initial_delay_ms,
        configuration.export_options.retries.max_delay_ms);
    fastchunk::DirectoryPipeline pipeline;
    const char* telemetry_token = configuration.telemetry.token_env.empty()
        ? nullptr
        : std::getenv(configuration.telemetry.token_env.c_str());
    auto telemetry_exporter = fastchunk::create_telemetry_exporter(
        configuration.telemetry.exporter, configuration.telemetry.endpoint,
        telemetry_token ? telemetry_token : "");
    fastchunk::AsyncTelemetryCollector telemetry(configuration.telemetry.queue_size,
        std::move(telemetry_exporter), configuration.telemetry.on_failure,
        configuration.telemetry.shutdown_timeout_ms);
    fastchunk::ITelemetryCollector* telemetry_ptr = configuration.telemetry.enabled ? &telemetry : nullptr;
    if (telemetry_ptr)
        telemetry_ptr->record({ .metric_name = "fastchunk_bytes_processed_total",
            .value = 0.0,
            .unit = "By",
            .document_path = configuration.input.path });
    auto exported = pipeline.export_streaming(configuration, exporter, nullptr, telemetry_ptr);
    if (!exported.has_value())
    {
        print_error(*exported.error(), configuration.json_errors);
        return 1;
    }
    telemetry.shutdown();
    if (configuration.telemetry.enabled)
    {
        for (const auto& diagnostic : telemetry.diagnostics())
            std::cerr << "telemetry warning [" << diagnostic.code << "]: "
                      << diagnostic.message << "\n";
    }
    return 0;
}
