#pragma once

#include "fastchunk/constants.h"

#include "fastchunk/core.h"
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fastchunk
{

class MetadataProcessor
{
public:
    // Parses raw JSON string into key-value map (zero-dependency flat parser)
    static std::unordered_map<std::string, std::string>
    parse_json_object(std::string_view json_str)
    {
        std::unordered_map<std::string, std::string> kv_map;
        if (json_str.empty())
            return kv_map;

        std::size_t pos = 0;
        while (pos < json_str.size())
        {
            auto key_start = json_str.find('"', pos);
            if (key_start == std::string_view::npos)
                break;
            auto key_end = json_str.find('"', key_start + 1);
            if (key_end == std::string_view::npos)
                break;

            std::string_view key = json_str.substr(key_start + 1, key_end - key_start - 1);

            auto colon_pos = json_str.find(':', key_end + 1);
            if (colon_pos == std::string_view::npos)
                break;

            auto val_start = json_str.find_first_not_of(" \t\n\r", colon_pos + 1);
            if (val_start == std::string_view::npos)
                break;

            std::size_t val_end = std::string_view::npos;
            if (json_str[val_start] == '"')
            {
                val_start++;
                val_end = json_str.find('"', val_start);
                if (val_end != std::string_view::npos)
                {
                    kv_map[std::string(key)] = std::string(json_str.substr(val_start, val_end - val_start));
                    pos = val_end + 1;
                }
                else
                {
                    break;
                }
            }
            else
            {
                val_end = json_str.find_first_of(",}", val_start);
                if (val_end == std::string_view::npos)
                    val_end = json_str.size();
                kv_map[std::string(key)] = std::string(json_str.substr(val_start, val_end - val_start));
                pos = val_end;
            }
        }
        return kv_map;
    }

    // Merges doc-level and record-level metadata objects according to collision
    // policy
    static Result<std::string>
    merge_metadata(std::string_view doc_metadata,
        std::string_view record_metadata,
        ChunkOptions::MetadataCollisionPolicy policy)
    {

        if (doc_metadata.empty())
            return Result<std::string>(std::string(record_metadata));
        if (record_metadata.empty())
            return Result<std::string>(std::string(doc_metadata));

        auto doc_map = parse_json_object(doc_metadata);
        auto record_map = parse_json_object(record_metadata);

        std::vector<Diagnostic> diagnostics;

        for (const auto& [key, val] : record_map)
        {
            auto it = doc_map.find(key);
            if (it != doc_map.end() && it->second != val)
            {
                if (policy == ChunkOptions::MetadataCollisionPolicy::error)
                {
                    return Result<std::string>(Error {
                        .code = static_cast<std::uint32_t>(ErrorCode::invalid_configuration),
                        .name = std::string(constants::error_name::invalid_configuration),
                        .description = "Document-level and record-level metadata keys "
                                       "collided under error policy.",
                        .message = "Key collision detected on metadata key '" + key + "'." });
                }
                else if (policy == ChunkOptions::MetadataCollisionPolicy::ignore)
                {
                    diagnostics.push_back(Diagnostic {
                        .severity = DiagnosticSeverity::warning,
                        .code = static_cast<std::uint32_t>(DiagnosticCode::metadata_ignored),
                        .name = "metadata_ignored",
                        .description = "Conflicting record metadata was ignored.",
                        .message = "Record key '" + key + "' overwritten by document metadata." });
                    // Retain primary doc value (ignore record collision key)
                    continue;
                }
            }
            doc_map[key] = val;
        }

        // Reconstruct merged JSON string
        std::string serialized = "{";
        bool first = true;
        for (const auto& [k, v] : doc_map)
        {
            if (!first)
                serialized += ",";
            serialized += "\"" + k + "\":\"" + v + "\"";
            first = false;
        }
        serialized += "}";

        return Result<std::string>(std::move(serialized), std::move(diagnostics));
    }
};

} // namespace fastchunk
