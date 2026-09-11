#pragma once

#include "fastchunk/constants.h"
#include "fastchunk/core.h"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace fastchunk
{

struct NdjsonRecord
{
    std::string_view line_text;
    std::string record_id;
    std::size_t start_byte { 0 };
    std::size_t end_byte { 0 };
};

class NdjsonParser
{
    class JsonScanner
    {
    public:
        explicit JsonScanner(std::string_view input)
            : input_(input)
        {
        }

        bool parse_object(std::string_view wanted_key, std::string& value,
            bool& found)
        {
            skip_space();
            if (!consume('{'))
                return false;
            skip_space();
            if (consume('}'))
                return true;
            while (position_ < input_.size())
            {
                std::string key;
                if (!parse_string(key))
                    return false;
                skip_space();
                if (!consume(':'))
                    return false;
                skip_space();
                if (key == wanted_key)
                {
                    found = true;
                    if (!parse_string(value) || value.empty())
                        return false;
                }
                else if (!skip_value())
                {
                    return false;
                }
                skip_space();
                if (consume('}'))
                    return true;
                if (!consume(','))
                    return false;
                skip_space();
            }
            return false;
        }

        bool at_end()
        {
            skip_space();
            return position_ == input_.size();
        }

    private:
        void skip_space()
        {
            while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_])))
            {
                ++position_;
            }
        }

        bool consume(char expected)
        {
            if (position_ < input_.size() && input_[position_] == expected)
            {
                ++position_;
                return true;
            }
            return false;
        }

        bool parse_string(std::string& output)
        {
            if (!consume('"'))
                return false;
            output.clear();
            while (position_ < input_.size())
            {
                const char current = input_[position_++];
                if (current == '"')
                    return true;
                if (static_cast<unsigned char>(current) < 0x20)
                    return false;
                if (current != '\\')
                {
                    output.push_back(current);
                    continue;
                }
                if (position_ >= input_.size())
                    return false;
                const char escaped = input_[position_++];
                switch (escaped)
                {
                case '"':
                case '\\':
                case '/':
                    output.push_back(escaped);
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u':
                    if (position_ + 4 > input_.size())
                        return false;
                    output.append("\\u");
                    output.append(input_.substr(position_, 4));
                    position_ += 4;
                    break;
                default:
                    return false;
                }
            }
            return false;
        }

        bool skip_value()
        {
            if (position_ >= input_.size())
                return false;
            if (input_[position_] == '"')
            {
                std::string ignored;
                return parse_string(ignored);
            }
            if (input_[position_] == '{')
            {
                ++position_;
                skip_space();
                if (consume('}'))
                    return true;
                while (true)
                {
                    std::string ignored;
                    if (!parse_string(ignored))
                        return false;
                    skip_space();
                    if (!consume(':'))
                        return false;
                    skip_space();
                    if (!skip_value())
                        return false;
                    skip_space();
                    if (consume('}'))
                        return true;
                    if (!consume(','))
                        return false;
                    skip_space();
                }
            }
            if (input_[position_] == '[')
            {
                ++position_;
                skip_space();
                if (consume(']'))
                    return true;
                while (true)
                {
                    if (!skip_value())
                        return false;
                    skip_space();
                    if (consume(']'))
                        return true;
                    if (!consume(','))
                        return false;
                    skip_space();
                }
            }
            const std::size_t start = position_;
            while (position_ < input_.size() && input_[position_] != ',' && input_[position_] != '}' && input_[position_] != ']')
            {
                ++position_;
            }
            const auto literal = input_.substr(start, position_ - start);
            return literal == "true" || literal == "false" || literal == "null" || (!literal.empty() && std::isdigit(static_cast<unsigned char>(literal.front())));
        }

        std::string_view input_;
        std::size_t position_ { 0 };
    };

public:
    static Result<std::vector<NdjsonRecord>>
    parse(std::span<const std::byte> buffer, std::string_view record_id_key,
        InputOptions::MalformedRecordPolicy policy)
    {
        std::vector<NdjsonRecord> records;
        std::vector<Diagnostic> diagnostics;
        const std::string_view data(reinterpret_cast<const char*>(buffer.data()),
            buffer.size());
        std::size_t line_start = 0;
        std::size_t sequence = 0;

        while (line_start < data.size())
        {
            const std::size_t line_end = data.find('\n', line_start);
            const std::size_t delimiter_end = line_end == std::string_view::npos ? data.size() : line_end + 1;
            const std::size_t content_end = line_end == std::string_view::npos
                ? data.size()
                : (line_end > line_start && data[line_end - 1] == '\r'
                          ? line_end - 1
                          : line_end);
            const std::string_view line = data.substr(line_start, content_end - line_start);

            bool valid = true;
            std::string record_id;
            if (!line.empty())
            {
                bool found = false;
                JsonScanner scanner(line);
                valid = scanner.parse_object(record_id_key, record_id, found) && scanner.at_end();
                if (valid && record_id_key.empty())
                {
                    record_id = std::string(constants::records::generated_id_prefix)
                        + std::to_string(sequence);
                }
                else if (valid && (!found || record_id.empty()))
                {
                    valid = false;
                }
            }

            if (!line.empty() && !valid)
            {
                if (policy == InputOptions::MalformedRecordPolicy::error)
                {
                    return Result<std::vector<NdjsonRecord>>(Error {
                        .code = static_cast<std::uint32_t>(ErrorCode::invalid_argument),
                        .name = std::string(constants::error_name::invalid_argument),
                        .description = "A public argument is missing, malformed, or out of range.",
                        .message = "NDJSON line is not a valid object with a non-empty "
                                   "record ID.",
                        .byte_offset = line_start });
                }
                diagnostics.push_back(Diagnostic {
                    .severity = DiagnosticSeverity::warning,
                    .code = static_cast<std::uint32_t>(
                        DiagnosticCode::malformed_record_skipped),
                    .name = std::string(constants::diagnostic_name::malformed_record_skipped),
                    .description = "A malformed NDJSON record was skipped per policy.",
                    .message = "Invalid JSON object or missing record ID; skipped.",
                    .source_byte_offset = line_start });
            }
            else if (!line.empty())
            {
                records.push_back(NdjsonRecord { .line_text = line,
                    .record_id = std::move(record_id),
                    .start_byte = line_start,
                    .end_byte = delimiter_end });
                ++sequence;
            }
            line_start = delimiter_end;
        }
        return Result<std::vector<NdjsonRecord>>(std::move(records),
            std::move(diagnostics));
    }
};

} // namespace fastchunk
