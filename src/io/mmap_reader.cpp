#include "fastchunk/constants.h"
#include "fastchunk/core.h"
#include "simd_utf8.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fastchunk
{

namespace
{

    // Helper: Scalar fall-back repair implementation for UTF-8 replacing or
    // skipping
    std::vector<std::byte> repair_utf8_bytes(const std::byte* data,
        std::size_t size,
        InvalidUtf8Policy policy,
        std::vector<Diagnostic>& diagnostics)
    {

        std::vector<std::byte> repaired;
        repaired.reserve(size);

        const auto* bytes = reinterpret_cast<const unsigned char*>(data);
        std::size_t i = 0;

        const char* replacement = "\xEF\xBF\xBD"; // U+FFFD in UTF-8

        while (i < size)
        {
            // ASCII fast path
            if (bytes[i] <= 0x7F)
            {
                repaired.push_back(static_cast<std::byte>(bytes[i]));
                i++;
                continue;
            }

            std::size_t seq_len = 0;
            bool valid = false;

            if ((bytes[i] & 0xE0) == 0xC0)
            {
                seq_len = 2;
                if (i + 1 < size && (bytes[i + 1] & 0xC0) == 0x80 && bytes[i] >= 0xC2)
                {
                    valid = true;
                }
            }
            else if ((bytes[i] & 0xF0) == 0xE0)
            {
                seq_len = 3;
                if (i + 2 < size && (bytes[i + 1] & 0xC0) == 0x80 && (bytes[i + 2] & 0xC0) == 0x80)
                {
                    if (!(bytes[i] == 0xED && bytes[i + 1] >= 0xA0))
                    {
                        valid = true;
                    }
                }
            }
            else if ((bytes[i] & 0xF8) == 0xF0)
            {
                seq_len = 4;
                if (i + 3 < size && (bytes[i + 1] & 0xC0) == 0x80 && (bytes[i + 2] & 0xC0) == 0x80 && (bytes[i + 3] & 0xC0) == 0x80)
                {
                    if (bytes[i] <= 0xF4 && !(bytes[i] == 0xF4 && bytes[i + 1] > 0x8F))
                    {
                        valid = true;
                    }
                }
            }

            if (valid && (i + seq_len <= size))
            {
                for (std::size_t k = 0; k < seq_len; ++k)
                {
                    repaired.push_back(static_cast<std::byte>(bytes[i + k]));
                }
                i += seq_len;
            }
            else
            {
                // Record diagnostic warning
                diagnostics.push_back(Diagnostic {
                    .severity = DiagnosticSeverity::warning,
                    .code = static_cast<std::uint32_t>(
                        policy == InvalidUtf8Policy::replace
                            ? DiagnosticCode::invalid_utf8_replaced
                            : DiagnosticCode::invalid_utf8_skipped),
                    .name = policy == InvalidUtf8Policy::replace ? "invalid_utf8_replaced"
                                                                 : "invalid_utf8_skipped",
                    .description = "Malformed UTF-8 sequence encountered and processed "
                                   "per input policy.",
                    .message = "Invalid UTF-8 byte encountered at offset " + std::to_string(i) + ". Applied policy.",
                    .source_byte_offset = i });

                if (policy == InvalidUtf8Policy::replace)
                {
                    repaired.push_back(static_cast<std::byte>(replacement[0]));
                    repaired.push_back(static_cast<std::byte>(replacement[1]));
                    repaired.push_back(static_cast<std::byte>(replacement[2]));
                }
                // Increment past the bad byte
                i++;
            }
        }

        return repaired;
    }

} // namespace

class MmapReader final : public IReader
{
public:
    MmapReader() = default;

    ~MmapReader() override { (void)close(); }

    // Prevent copying
    MmapReader(const MmapReader&) = delete;
    MmapReader& operator=(const MmapReader&) = delete;

    // Allow moving
    MmapReader(MmapReader&& other) noexcept
        : mapped_data_(other.mapped_data_)
        , mapped_size_(other.mapped_size_)
        , copied_buffer_(std::move(other.copied_buffer_))
#if defined(_WIN32)
        , file_handle_(other.file_handle_)
        , mapping_handle_(other.mapping_handle_)
#else
        , fd_(other.fd_)
#endif
    {
        other.mapped_data_ = nullptr;
        other.mapped_size_ = 0;
#if defined(_WIN32)
        other.file_handle_ = INVALID_HANDLE_VALUE;
        other.mapping_handle_ = nullptr;
#else
        other.fd_ = -1;
#endif
    }

    MmapReader& operator=(MmapReader&& other) noexcept
    {
        if (this != &other)
        {
            (void)close();
            mapped_data_ = other.mapped_data_;
            mapped_size_ = other.mapped_size_;
            copied_buffer_ = std::move(other.copied_buffer_);
#if defined(_WIN32)
            file_handle_ = other.file_handle_;
            mapping_handle_ = other.mapping_handle_;
            other.file_handle_ = INVALID_HANDLE_VALUE;
            other.mapping_handle_ = nullptr;
#else
            fd_ = other.fd_;
            other.fd_ = -1;
#endif
            other.mapped_data_ = nullptr;
            other.mapped_size_ = 0;
        }
        return *this;
    }

    Result<FileBuffer> open(const std::filesystem::path& filepath,
        const InputOptions& options) override
    {

        (void)close();

        if (options.mode == InputMode::zero_copy && options.invalid_utf8 != InvalidUtf8Policy::error)
        {
            return Result<FileBuffer>(Error {
                .code = static_cast<std::uint32_t>(ErrorCode::invalid_configuration),
                .name = std::string(constants::error_name::invalid_configuration),
                .description = "Configuration values conflict or violate a documented rule.",
                .message = "zero_copy mode only supports invalid_utf8=error; use "
                           "copy mode for lossy repair.",
                .path = filepath.string() });
        }

        if (!std::filesystem::exists(filepath))
        {
            return Result<FileBuffer>(Error {
                .code = static_cast<std::uint32_t>(ErrorCode::file_not_found),
                .name = "file_not_found",
                .description = "The requested input or model file does not exist.",
                .message = "Failed to locate target file for mapping.",
                .path = filepath.string() });
        }

        std::uint64_t file_size = 0;
        try
        {
            file_size = std::filesystem::file_size(filepath);
        }
        catch (...)
        {
            return Result<FileBuffer>(
                Error { .code = static_cast<std::uint32_t>(ErrorCode::io_error),
                    .name = "io_error",
                    .description = "Low-level system I/O failure during file "
                                   "access or mapping operations.",
                    .message = "Failed to retrieve target file size.",
                    .path = filepath.string() });
        }

        if (file_size == 0)
        {
            return FileBuffer { .data = std::span<const std::byte> {},
                .file_size = 0,
                .diagnostics = {} };
        }

#if defined(_WIN32)
        file_handle_ = CreateFileW(filepath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (file_handle_ == INVALID_HANDLE_VALUE)
        {
            return Result<FileBuffer>(
                Error { .code = static_cast<std::uint32_t>(ErrorCode::io_error),
                    .name = "io_error",
                    .description = "Low-level system I/O failure during file "
                                   "access or mapping operations.",
                    .message = "Failed to open OS file handle.",
                    .path = filepath.string() });
        }

        mapping_handle_ = CreateFileMappingW(file_handle_, NULL, PAGE_READONLY, 0, 0, NULL);

        if (!mapping_handle_)
        {
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
            return Result<FileBuffer>(Error {
                .code = static_cast<std::uint32_t>(ErrorCode::mapping_failed),
                .name = "mapping_failed",
                .description = "The operating system rejected or failed the memory mapping.",
                .message = "CreateFileMapping failed.",
                .path = filepath.string() });
        }

        void* ptr = MapViewOfFile(mapping_handle_, FILE_MAP_READ, 0, 0, 0);

        if (!ptr)
        {
            CloseHandle(mapping_handle_);
            CloseHandle(file_handle_);
            mapping_handle_ = nullptr;
            file_handle_ = INVALID_HANDLE_VALUE;
            return Result<FileBuffer>(Error {
                .code = static_cast<std::uint32_t>(ErrorCode::mapping_failed),
                .name = "mapping_failed",
                .description = "The operating system rejected or failed the memory mapping.",
                .message = "MapViewOfFile failed.",
                .path = filepath.string() });
        }

        mapped_data_ = static_cast<const std::byte*>(ptr);
        mapped_size_ = static_cast<std::size_t>(file_size);
#else
        fd_ = ::open(filepath.c_str(), O_RDONLY);
        if (fd_ < 0)
        {
            return Result<FileBuffer>(
                Error { .code = static_cast<std::uint32_t>(ErrorCode::io_error),
                    .name = "io_error",
                    .description = "Low-level system I/O failure during file "
                                   "access or mapping operations.",
                    .message = "Failed to open POSIX file descriptor.",
                    .path = filepath.string() });
        }

        void* ptr = ::mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd_, 0);
        if (ptr == MAP_FAILED)
        {
            ::close(fd_);
            fd_ = -1;
            return Result<FileBuffer>(Error {
                .code = static_cast<std::uint32_t>(ErrorCode::mapping_failed),
                .name = "mapping_failed",
                .description = "The operating system rejected or failed the memory mapping.",
                .message = "POSIX mmap syscall failed.",
                .path = filepath.string() });
        }

        mapped_data_ = static_cast<const std::byte*>(ptr);
        mapped_size_ = static_cast<std::size_t>(file_size);
#endif

        std::vector<Diagnostic> diagnostics;

        // Zero-Copy Mode Processing with SIMD Validation
        if (options.mode == InputMode::zero_copy)
        {
            std::size_t error_offset = 0;
            if (!SimdUtf8Validator::validate(mapped_data_, mapped_size_,
                    error_offset))
            {
                if (options.invalid_utf8 == InvalidUtf8Policy::error)
                {
                    (void)close();
                    return Result<FileBuffer>(
                        Error { .code = static_cast<std::uint32_t>(ErrorCode::invalid_utf8),
                            .name = "invalid_utf8",
                            .description = "Input contains malformed UTF-8; byte "
                                           "offset identifies the first error.",
                            .message = "Malformed UTF-8 sequence discovered during "
                                       "zero-copy ingestion scan.",
                            .byte_offset = error_offset,
                            .path = filepath.string() });
                }
                else
                {
                    // If policy is replace/skip, convert buffer and downgrade mode to
                    // copy
                    copied_buffer_ = repair_utf8_bytes(mapped_data_, mapped_size_,
                        options.invalid_utf8, diagnostics);
                    return FileBuffer { .data = std::span<const std::byte>(
                                            copied_buffer_.data(), copied_buffer_.size()),
                        .file_size = mapped_size_,
                        .diagnostics = std::move(diagnostics) };
                }
            }

            return FileBuffer {
                .data = std::span<const std::byte>(mapped_data_, mapped_size_),
                .file_size = mapped_size_,
                .diagnostics = std::move(diagnostics)
            };
        }
        // Copy Mode Processing
        else
        {
            if (options.invalid_utf8 == InvalidUtf8Policy::error)
            {
                std::size_t error_offset = 0;
                if (!SimdUtf8Validator::validate(mapped_data_, mapped_size_,
                        error_offset))
                {
                    (void)close();
                    return Result<FileBuffer>(
                        Error { .code = static_cast<std::uint32_t>(ErrorCode::invalid_utf8),
                            .name = "invalid_utf8",
                            .description = "Input contains malformed UTF-8; byte "
                                           "offset identifies the first error.",
                            .message = "Malformed UTF-8 sequence discovered during "
                                       "copy ingestion scan.",
                            .byte_offset = error_offset,
                            .path = filepath.string() });
                }
                copied_buffer_.assign(mapped_data_, mapped_data_ + mapped_size_);
            }
            else
            {
                copied_buffer_ = repair_utf8_bytes(mapped_data_, mapped_size_,
                    options.invalid_utf8, diagnostics);
            }

            return FileBuffer { .data = std::span<const std::byte>(
                                    copied_buffer_.data(), copied_buffer_.size()),
                .file_size = mapped_size_,
                .diagnostics = std::move(diagnostics) };
        }
    }

    Result<void> close() noexcept override
    {
        if (mapped_data_ != nullptr)
        {
#if defined(_WIN32)
            if (mapped_data_)
            {
                UnmapViewOfFile(mapped_data_);
                mapped_data_ = nullptr;
            }
            if (mapping_handle_)
            {
                CloseHandle(mapping_handle_);
                mapping_handle_ = nullptr;
            }
            if (file_handle_ != INVALID_HANDLE_VALUE)
            {
                CloseHandle(file_handle_);
                file_handle_ = INVALID_HANDLE_VALUE;
            }
#else
            if (mapped_data_ && mapped_size_ > 0)
            {
                ::munmap(const_cast<void*>(static_cast<const void*>(mapped_data_)),
                    mapped_size_);
                mapped_data_ = nullptr;
            }
            if (fd_ >= 0)
            {
                ::close(fd_);
                fd_ = -1;
            }
#endif
            mapped_size_ = 0;
        }
        copied_buffer_.clear();
        copied_buffer_.shrink_to_fit();

        return Result<void>();
    }

private:
    const std::byte* mapped_data_ { nullptr };
    std::size_t mapped_size_ { 0 };
    std::vector<std::byte> copied_buffer_;

#if defined(_WIN32)
    HANDLE file_handle_ { INVALID_HANDLE_VALUE };
    HANDLE mapping_handle_ { nullptr };
#else
    int fd_ { -1 };
#endif
};

// Factory export function
std::unique_ptr<IReader> create_mmap_reader()
{
    return std::make_unique<MmapReader>();
}

} // namespace fastchunk