#include "fastchunk/config.h"
#include "fastchunk/constants.h"
#include "fastchunk/core.h"
#include "fastchunk/factory.h"
#include "fastchunk/tokenizers.h"
#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <exception>
#include <optional>
#include <stdexcept>

namespace nb = nanobind;
using namespace nb::literals;

namespace
{
struct FastchunkPythonError : std::runtime_error
{
    explicit FastchunkPythonError(const fastchunk::Error& value)
        : std::runtime_error(value.message)
        , error(value)
    {
    }

    fastchunk::Error error;
};
struct ConfigurationError : FastchunkPythonError
{
    using FastchunkPythonError::FastchunkPythonError;
};
struct TokenizerError : FastchunkPythonError
{
    using FastchunkPythonError::FastchunkPythonError;
};
struct DependencyError : FastchunkPythonError
{
    using FastchunkPythonError::FastchunkPythonError;
};
struct CancellationError : FastchunkPythonError
{
    using FastchunkPythonError::FastchunkPythonError;
};
struct UnsupportedOptionError : FastchunkPythonError
{
    using FastchunkPythonError::FastchunkPythonError;
};
struct InputError : FastchunkPythonError
{
    using FastchunkPythonError::FastchunkPythonError;
};
struct InvalidUtf8Error : FastchunkPythonError
{
    using FastchunkPythonError::FastchunkPythonError;
};
struct FileError : FastchunkPythonError
{
    using FastchunkPythonError::FastchunkPythonError;
};
struct ExportError : FastchunkPythonError
{
    using FastchunkPythonError::FastchunkPythonError;
};
struct ResourceLimitError : FastchunkPythonError
{
    using FastchunkPythonError::FastchunkPythonError;
};

void set_python_error(PyObject* exception_type, const fastchunk::Error& error)
{
    PyObject* instance = PyObject_CallFunction(exception_type, "(s)", error.message.c_str());
    if (!instance)
        return;

    auto set_string = [instance](const char* name, const std::string& value)
    {
        PyObject* object = PyUnicode_FromStringAndSize(value.data(), value.size());
        if (object)
        {
            PyObject_SetAttrString(instance, name, object);
            Py_DECREF(object);
        }
    };
    set_string("name", error.name);
    set_string("description", error.description);
    set_string("message", error.message);
    set_string("path", error.path);
    set_string("dependency", error.dependency);
    set_string("details_json", error.details_json);

    PyObject* code = PyLong_FromUnsignedLong(error.code);
    if (code)
    {
        PyObject_SetAttrString(instance, "code", code);
        Py_DECREF(code);
    }
    PyObject* byte_offset = error.byte_offset.has_value()
        ? PyLong_FromUnsignedLongLong(*error.byte_offset)
        : Py_None;
    Py_INCREF(byte_offset);
    PyObject_SetAttrString(instance, "byte_offset", byte_offset);
    Py_DECREF(byte_offset);

    PyErr_SetObject(exception_type, instance);
    Py_DECREF(instance);
}

[[noreturn]] void raise_python_error(const fastchunk::Error& error)
{
    switch (static_cast<fastchunk::ErrorCode>(error.code))
    {
    case fastchunk::ErrorCode::invalid_configuration:
        throw ConfigurationError(error);
    case fastchunk::ErrorCode::invalid_argument:
        throw InputError(error);
    case fastchunk::ErrorCode::tokenizer_unavailable:
    case fastchunk::ErrorCode::tokenizer_model_not_found:
    case fastchunk::ErrorCode::tokenizer_model_invalid:
    case fastchunk::ErrorCode::tokenizer_failed:
        throw TokenizerError(error);
    case fastchunk::ErrorCode::tokenizer_dependency_missing:
        throw DependencyError(error);
    case fastchunk::ErrorCode::cancelled:
        throw CancellationError(error);
    case fastchunk::ErrorCode::unsupported_option:
        throw UnsupportedOptionError(error);
    case fastchunk::ErrorCode::invalid_utf8:
        throw InvalidUtf8Error(error);
    case fastchunk::ErrorCode::file_not_found:
    case fastchunk::ErrorCode::permission_denied:
    case fastchunk::ErrorCode::mapping_failed:
    case fastchunk::ErrorCode::io_error:
        throw FileError(error);
    case fastchunk::ErrorCode::export_failed:
    case fastchunk::ErrorCode::exporter_dependency_missing:
    case fastchunk::ErrorCode::exporter_configuration_invalid:
        throw ExportError(error);
    case fastchunk::ErrorCode::resource_limit_exceeded:
        throw ResourceLimitError(error);
    default:
        throw FastchunkPythonError(error);
    }
}

struct PythonExceptionTypes
{
    PyObject* base;
    PyObject* configuration;
    PyObject* tokenizer;
    PyObject* dependency;
    PyObject* cancellation;
    PyObject* unsupported;
    PyObject* input;
    PyObject* utf8;
    PyObject* file;
    PyObject* exporter;
    PyObject* resource;
};

void translate_python_error(const std::exception_ptr& exception, void* payload)
{
    const auto& types = *static_cast<PythonExceptionTypes*>(payload);
    try
    {
        std::rethrow_exception(exception);
    }
    catch (const ConfigurationError& error)
    {
        set_python_error(types.configuration, error.error);
    }
    catch (const TokenizerError& error)
    {
        set_python_error(types.tokenizer, error.error);
    }
    catch (const DependencyError& error)
    {
        set_python_error(types.dependency, error.error);
    }
    catch (const CancellationError& error)
    {
        set_python_error(types.cancellation, error.error);
    }
    catch (const UnsupportedOptionError& error)
    {
        set_python_error(types.unsupported, error.error);
    }
    catch (const InputError& error)
    {
        set_python_error(types.input, error.error);
    }
    catch (const InvalidUtf8Error& error)
    {
        set_python_error(types.utf8, error.error);
    }
    catch (const FileError& error)
    {
        set_python_error(types.file, error.error);
    }
    catch (const ExportError& error)
    {
        set_python_error(types.exporter, error.error);
    }
    catch (const ResourceLimitError& error)
    {
        set_python_error(types.resource, error.error);
    }
    catch (const FastchunkPythonError& error)
    {
        set_python_error(types.base, error.error);
    }
}

class PythonChunkStream
{
public:
    PythonChunkStream(const std::string& path, std::size_t max_tokens,
        std::size_t overlap_tokens, const std::string& tokenizer_name)
    {
        reader_ = std::shared_ptr<fastchunk::IReader>(fastchunk::create_mmap_reader().release());
        auto input = reader_->open(path, {});
        if (!input.has_value())
            raise_python_error(*input.error());
        auto tokenizer = fastchunk::create_tokenizer_from_name(tokenizer_name);
        if (!tokenizer.has_value())
            raise_python_error(*tokenizer.error());
        tokenizer_ = std::shared_ptr<fastchunk::ITokenizer>(std::move(tokenizer).value().release());
        fastchunk::ChunkOptions options { .max_tokens = max_tokens,
            .overlap_tokens = overlap_tokens };
        fastchunk::ChunkInputContext context { .doc_id = path,
            .tokenizer_name = tokenizer_name };
        auto result = fastchunk::create_chunker()->stream_buffer(input.value().data,
            options, *tokenizer_, context);
        if (!result.has_value())
            raise_python_error(*result.error());
        stream_ = std::move(result).value();
    }

    nb::dict next()
    {
        fastchunk::ChunkView view;
        auto result = stream_->next(view);
        if (!result.has_value())
            raise_python_error(*result.error());
        if (!result.value())
            throw nb::stop_iteration();
        nb::dict chunk;
        chunk["text"] = std::string(view.text);
        chunk["tokens"] = std::vector<std::uint32_t>(view.tokens.begin(), view.tokens.end());
        chunk["chunk_id"] = std::string(view.chunk_id);
        chunk["doc_id"] = std::string(view.doc_id);
        chunk["record_id"] = std::string(view.record_id);
        chunk["start_byte"] = view.start_byte;
        chunk["end_byte"] = view.end_byte;
        chunk["chunk_index"] = view.chunk_index;
        return chunk;
    }

    PythonChunkStream& iter() { return *this; }
    void close()
    {
        if (stream_)
            stream_->cancel();
    }

private:
    std::shared_ptr<fastchunk::IReader> reader_;
    std::shared_ptr<fastchunk::ITokenizer> tokenizer_;
    std::unique_ptr<fastchunk::IChunkStream> stream_;
};
} // namespace

NB_MODULE(_fastchunk_cpp, m)
{
    m.doc() = "High-performance zero-copy text chunking engine Python bindings";

    nb::exception<FastchunkPythonError> base_error(m, "FastchunkError");
    nb::exception<ConfigurationError> configuration_error(m, "ConfigurationError",
        base_error);
    nb::exception<TokenizerError> tokenizer_error(m, "TokenizerError",
        base_error);
    nb::exception<DependencyError> dependency_error(m, "DependencyError",
        base_error);
    nb::exception<CancellationError> cancellation_error(m, "CancellationError",
        base_error);
    nb::exception<UnsupportedOptionError> unsupported_option_error(
        m, "UnsupportedOptionError", base_error);
    nb::exception<InputError> input_error(m, "InputError", base_error);
    nb::exception<InvalidUtf8Error> invalid_utf8_error(m, "InvalidUtf8Error",
        base_error);
    nb::exception<FileError> file_error(m, "FileError", base_error);
    nb::exception<ExportError> export_error(m, "ExportError", base_error);
    nb::exception<ResourceLimitError> resource_limit_error(m,
        "ResourceLimitError", base_error);

    auto* exception_types = new PythonExceptionTypes {
        base_error.ptr(), configuration_error.ptr(), tokenizer_error.ptr(),
        dependency_error.ptr(), cancellation_error.ptr(), unsupported_option_error.ptr(),
        input_error.ptr(), invalid_utf8_error.ptr(), file_error.ptr(), export_error.ptr(),
        resource_limit_error.ptr()
    };
    nb::register_exception_translator(translate_python_error, exception_types);

    // Enums
    nb::enum_<fastchunk::InputMode>(m, "InputMode")
        .value("ZeroCopy", fastchunk::InputMode::zero_copy)
        .value("Copy", fastchunk::InputMode::copy);

    nb::enum_<fastchunk::InvalidUtf8Policy>(m, "InvalidUtf8Policy")
        .value("Error", fastchunk::InvalidUtf8Policy::error)
        .value("Replace", fastchunk::InvalidUtf8Policy::replace)
        .value("Skip", fastchunk::InvalidUtf8Policy::skip);

    nb::enum_<fastchunk::ChunkResult::SourceOffsetKind>(m, "SourceOffsetKind")
        .value("Unavailable",
            fastchunk::ChunkResult::SourceOffsetKind::unavailable)
        .value("Exact", fastchunk::ChunkResult::SourceOffsetKind::exact)
        .value("Record", fastchunk::ChunkResult::SourceOffsetKind::record);

    nb::enum_<fastchunk::InputOptions::RecordMode>(m, "RecordMode")
        .value("None", fastchunk::InputOptions::RecordMode::none)
        .value("Ndjson", fastchunk::InputOptions::RecordMode::ndjson);

    nb::enum_<fastchunk::InputOptions::MalformedRecordPolicy>(
        m, "MalformedRecordPolicy")
        .value("Error", fastchunk::InputOptions::MalformedRecordPolicy::error)
        .value("Skip", fastchunk::InputOptions::MalformedRecordPolicy::skip);

    // Structs
    nb::class_<fastchunk::InputOptions>(m, "InputOptions")
        .def(nb::init<>())
        .def_rw("mode", &fastchunk::InputOptions::mode)
        .def_rw("invalid_utf8", &fastchunk::InputOptions::invalid_utf8)
        .def_rw("record_mode", &fastchunk::InputOptions::record_mode)
        .def_rw("malformed_record", &fastchunk::InputOptions::malformed_record)
        .def_rw("record_id_field", &fastchunk::InputOptions::record_id_field);

    nb::class_<fastchunk::ChunkOptions>(m, "ChunkOptions")
        .def(nb::init<>())
        .def_rw("max_tokens", &fastchunk::ChunkOptions::max_tokens)
        .def_rw("overlap_tokens", &fastchunk::ChunkOptions::overlap_tokens);

    nb::class_<fastchunk::ChunkView>(m, "ChunkView")
        .def_ro("text", &fastchunk::ChunkView::text)
        .def_ro("tokens", &fastchunk::ChunkView::tokens)
        .def_ro("start_byte", &fastchunk::ChunkView::start_byte)
        .def_ro("end_byte", &fastchunk::ChunkView::end_byte)
        .def_ro("chunk_index", &fastchunk::ChunkView::chunk_index)
        .def_ro("doc_id", &fastchunk::ChunkView::doc_id)
        .def_ro("record_id", &fastchunk::ChunkView::record_id)
        .def_ro("metadata", &fastchunk::ChunkView::metadata)
        .def_ro("tokenizer_name", &fastchunk::ChunkView::tokenizer_name)
        .def_ro("tokenizer_configuration",
            &fastchunk::ChunkView::tokenizer_configuration)
        .def_ro("source_start_byte", &fastchunk::ChunkView::source_start_byte)
        .def_ro("source_end_byte", &fastchunk::ChunkView::source_end_byte)
        .def_ro("has_source_offsets", &fastchunk::ChunkView::has_source_offsets)
        .def_ro("source_offset_kind", &fastchunk::ChunkView::source_offset_kind);

    // Tokenizer Interface & Implementations
    nb::class_<fastchunk::ITokenizer>(m, "ITokenizer");

    m.def(
        "create_tokenizer",
        [](const std::string& name, const std::string& config)
        {
            auto res = fastchunk::create_tokenizer_from_name(name, config);
            if (!res.has_value())
            {
                raise_python_error(*res.error());
            }
            return std::move(res).value();
        },
        "name"_a, "config"_a = "");

    // Reader API
    nb::class_<fastchunk::IReader>(m, "IReader");

    m.def("create_mmap_reader", []()
        { return fastchunk::create_mmap_reader(); });

    nb::class_<PythonChunkStream>(m, "ChunkStream")
        .def("__iter__", &PythonChunkStream::iter, nb::rv_policy::reference_internal)
        .def("__next__", &PythonChunkStream::next)
        .def("close", &PythonChunkStream::close)
        .def("__enter__", &PythonChunkStream::iter, nb::rv_policy::reference_internal)
        .def("__exit__", [](PythonChunkStream& stream, nb::args)
            { stream.close(); });

    m.def("stream_file", [](const std::string& path, std::size_t max_tokens, std::size_t overlap_tokens, const std::string& tokenizer_name)
        { return PythonChunkStream(path, max_tokens, overlap_tokens, tokenizer_name); }, "path"_a, "max_tokens"_a = 512, "overlap_tokens"_a = 64, "tokenizer_name"_a = std::string(fastchunk::constants::tokenizer::default_name));

    // Main Chunker Wrapper
    m.def(
        "chunk_file",
        [](const std::string& path, uint32_t max_tokens, uint32_t overlap_tokens,
            const std::string& tokenizer_name)
        {
            auto reader = fastchunk::create_mmap_reader();
            fastchunk::InputOptions in_opts {};

            auto open_res = reader->open(path, in_opts);
            if (!open_res.has_value())
            {
                raise_python_error(*open_res.error());
            }

            auto tok_res = fastchunk::create_tokenizer_from_name(tokenizer_name);
            if (!tok_res.has_value())
            {
                raise_python_error(*tok_res.error());
            }

            auto chunker = fastchunk::create_chunker();
            fastchunk::ChunkOptions chk_opts { .max_tokens = max_tokens,
                .overlap_tokens = overlap_tokens };
            fastchunk::ChunkInputContext ctx { .tokenizer_name = tokenizer_name };

            // Run chunker while releasing Python GIL
            nb::gil_scoped_release release;
            auto res = chunker->chunk_buffer(open_res.value().data, chk_opts,
                *tok_res.value(), ctx);

            if (!res.has_value())
            {
                raise_python_error(*res.error());
            }

            return res.value();
        },
        "path"_a, "max_tokens"_a = 512, "overlap_tokens"_a = 64,
        "tokenizer_name"_a = std::string(fastchunk::constants::tokenizer::default_name));

    m.def("load_configuration_json", [](const std::string& path)
        {
            auto result = fastchunk::load_configuration(path);
            if (!result.has_value()) raise_python_error(*result.error());
            return std::move(result).value().effective_json(); }, "path"_a);
}