# FastChunk Architectural Overview & Technical Design Specification

> **Implementation status:** The current repository implements the core reader, whitespace tokenizer, chunker, C ABI, Python bindings, YAML configuration, directory pipeline, CLI, NDJSON exporter, local bounded telemetry collector, and optional native Rust tokenizer backends. Tiktoken supports `cl100k_base` and `o200k_base`; Hugging Face loads a local `tokenizer.json`. External OTLP/Splunk delivery remains optional, and some advanced parallel-engine details remain planned.

## 1. System Context & Design Principles

FastChunk is a C++20 ingestion and token-bounded chunking engine built specifically to address I/O, string parsing, and tokenization bottlenecks in Retrieval-Augmented Generation (RAG) pipelines and LLM preprocessing workflows.

                    +----------------------------------------------+
                    |               External Applications          |
                    |     (C++ / C ABI / Python via nanobind)      |
                    +-----------------------+----------------------+
                                            |
                                            v
+-----------------------------------------------------------------------------------+
|                                FastChunk Engine                                   |
|                                                                                   |
|  +-------------------------+  +------------------------+  +--------------------+  |
|  |     Memory & I/O        |  |    Validation Layer    |  | Parallel Engine    |  |
|  |  (POSIX mmap / Win32)   |  |   (SIMD AVX2 / NEON)   |  | (Newline Aligning) |  |
|  +------------+------------+  +-----------+------------+  +---------+----------+  |
|               |                           |                         |             |
|               +---------------------------+-------------------------+             |
|                                           |                                       |
|                                           v                                       |
|  +-----------------------------------------------------------------------------+  |
|  |                           Tokenization & Windowing                          |  |
|  |               (OpenAI tiktoken BPE / Hugging Face WordPiece)                |  |
|  +----------------------------------------+------------------------------------+  |
|                                           |                                       |
+-------------------------------------------|---------------------------------------+
                                            v
                    +----------------------------------------------+
                    |         Zero-Copy Chunks / Vector DB         |
                    +----------------------------------------------+

### Core Design Principles

1. **Zero-Copy Ingestion:** Data is read directly from memory-mapped storage (`mmap` on POSIX, `MapViewOfFile` on Windows). String spans (`std::string_view` / `std::span<const std::byte>`) point into mapped pages to minimize dynamic allocations.
2. **SIMD Acceleration:** Fast-path byte verification uses SIMD instructions (32-byte AVX2 vector routines or 16-byte ARM NEON intrinsics) to scan input text for UTF-8 validity and structural delimiters before tokenization.
3. **Deterministic Parallelism:** Multi-threaded chunking splits buffers along structural line boundaries (`\n` or `\r\n`), ensuring chunk indexing and token coverage remain deterministic regardless of thread count.
4. **Resilient Memory Layouts:** Data representations favor contiguous allocations and lightweight stack views over object hierarchies.

---

## 2. Core Subsystems
                              +-----------------------+
                              |     File / Buffer     |
                              +-----------+-----------+
                                          |
                                          v
                              +-----------------------+
                              |    MmapReader Engine  |
                              +-----------+-----------+
                                          |
                                          v
                              +-----------------------+
                              |   SIMD UTF-8 Scanner  |
                              +-----------+-----------+
                                          |
                                          v
                    +-------------------------------------------+
                    |           Parallel Task Dispatcher        |
                    +-------------+-----------------+-----------+
                                  |                 |
                                  v                 v
                           +--------------+  +--------------+
                           | Worker Thread|  | Worker Thread|
                           | (Tokenizer)  |  | (Tokenizer)  |
                           +--------------+  +--------------+

### 2.1 Storage & Memory Ingestion (`io/mmap_reader.cpp`)
* Uses POSIX `mmap` or Windows `CreateFileMappingW` to project storage pages directly into user space virtual memory.
* Supports two main operation modes:
  * `InputMode::zero_copy`: Points references directly into mapped physical pages.
  * `InputMode::copy`: Allocates an internal string buffer when caller context requires mutating memory or non-mapped lifetime guarantees.

### 2.2 SIMD Validation Layer (`include/simd_utf8.h`)
* Validates UTF-8 stream integrity in 32-byte blocks using AVX2 (or 16-byte blocks using ARM NEON).
* Converts surrogate pairs, invalid byte sequences, and malformed codepoints according to configurable policies:
  * `error`: Halts execution and populates error context.
  * `replace`: Overwrites non-canonical bytes with Unicode replacement character `U+FFFD`.
  * `skip`: Omits non-canonical bytes while re-aligning byte offsets.

### 2.3 Tokenizer Abstraction & Engines (`tokenizers/`)
* **`ITokenizer` Interface:** Defines methods for byte-to-token encoding, token-to-byte decoding, and boundaries discovery.
* **`WhitespaceTokenizer`:** Mandatory deterministic reference tokenizer using ASCII whitespace and zero-based per-call IDs.
* **`TiktokenTokenizer`:** Optional C++ adapter over `backends/tiktoken`, a Rust `tiktoken-rs` static library supporting `cl100k_base` and `o200k_base`. It returns token IDs and reconstructed UTF-8 byte offsets.
* **`HuggingFaceTokenizer`:** Optional C++ adapter over `backends/huggingface`, a Rust `tokenizers` static library loading a caller-provided local `tokenizer.json` and exposing native offsets.
* **Factory Pattern:** Instantiates the selected backend from runtime configuration. Disabled optional backends return `tokenizer_unavailable`; they never silently fall back to whitespace.

### 2.4 Parallel Chunking Engine (`include/parallel_chunker.h`)
* Splits mapped memory buffers into sub-regions aligned with newline characters (`\n` or `\r\n`).
* Distributes distinct regions across worker threads to perform sliding-window tokenization in parallel.
* Retains sliding window history across partition boundaries to maintain token overlap constraints.

### 2.5 Stateful Stream Chunker (`include/fastchunk/stream_chunker.h`)
* Handles streaming ingestion from pipes or network interfaces where data arrives in variable fragments.
* Maintains a residual buffer to store trailing unchunked bytes and overlapping token contexts between successive calls to `push_bytes()`.

---

## 3. Data Structures & Layout

### Memory Structures (C++ Interface)

```cpp
// Core view into mapped memory buffers
struct FileBuffer {
  std::span<const std::byte> data;
  std::size_t file_size{0};
  std::vector<Diagnostic> diagnostics;
};

// Represents a generated text chunk
struct ChunkResult {
  std::uint64_t chunk_index{0};
  std::uint64_t start_byte{0};
  std::uint64_t end_byte{0};
  std::string_view text;
  std::vector<std::uint32_t> tokens;
  std::string doc_id;
  std::string record_id;
};

C ABI View Layout (include/fastchunk/fastchunk_c.h)
To ensure stable C ABI interop without relying on non-standard C++ memory layouts, C views use explicitly aligned POD structs:

C
typedef struct fastchunk_chunk_view {
  uint64_t chunk_index;
  uint64_t start_byte;
  uint64_t end_byte;
  const char* text_ptr;
  size_t text_len;
  const uint32_t* tokens_ptr;
  size_t tokens_len;
  const char* doc_id;
  const char* record_id;
} fastchunk_chunk_view_t;

4. Foreign Language Integration & Threading Model

+-----------------------------------------------------------------------------+
|                               Python Runtime                                |
|                                                                             |
|   fastchunk.Chunker().chunk_buffer(...)                                     |
+-------------------------------------+---------------------------------------+
                                      |
                     nanobind GIL Release (nb::gil_scoped_release)
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                                C++ Core Engine                              |
|                                                                             |
|  +--------------------+   +--------------------+   +---------------------+  |
|  | Thread Pool Task 1 |   | Thread Pool Task 2 |   | Thread Pool Task N  |  |
|  +--------------------+   +--------------------+   +---------------------+  |
+-------------------------------------+---------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                              Native C Chunks                                |
+-----------------------------------------------------------------------------+

4.1 Python Binding via nanobind (src/bindings/fastchunk_py.cpp)
Binds lightweight C++ structures using nanobind headers for high performance.

Releases Python's Global Interpreter Lock (nb::gil_scoped_release) during long-running chunking operations, allowing true OS-level thread concurrency across CPU cores.

4.2 Cooperative Thread Cancellation (include/fastchunk/cancellation.h)
Long-running parallel loops periodically inspect an atomic state flag wrapped by CancellationToken.

If a caller sets the cancellation flag, worker threads stop further chunk allocations, release partial contexts, and return early with ErrorCode::cancelled.

5. Error Handling & Diagnostics Framework
Errors and diagnostic events bypass standard C++ exceptions to maintain predictability across C ABI and foreign language runtimes.

Result Paradigm: Functions return a Result<T> monadic wrapper containing either a valid payload T or an Error object.

Diagnostic Propagation: Non-fatal parsing issues (such as invalid UTF-8 replacement actions) emit a Diagnostic object containing:

    DiagnosticSeverity (info, warning, error).

    byte_offset marking location within the source stream.

    Context description and diagnostic error code.

C++
struct Error {
  std::uint32_t code{0};
  std::size_t byte_offset{0};
  std::string name;
  std::string description;
  std::string message;
  std::string path;
};

6. Build System & Toolchain Specifications
The build system relies on CMake 3.20+ configured for modern C++20 standard requirements:

CMake
# Compilation targets configured in CMakeLists.txt
add_library(fastchunk-core STATIC)
target_compile_features(fastchunk-core PUBLIC cxx_std_20)

# Optional Python extension module build target
if(FASTCHUNK_ENABLE_PYTHON)
  nanobind_add_module(fastchunk_py src/bindings/fastchunk_py.cpp)
endif()

# Optional native tokenizer backends; each invokes Cargo for its Rust staticlib
cmake -S . -B build-tiktoken -DFASTCHUNK_ENABLE_TIKTOKEN=ON
cmake -S . -B build-huggingface -DFASTCHUNK_ENABLE_HUGGINGFACE=ON

Operating Systems & Target Architectures
Linux: GCC 11+, Clang 13+ (x86_64 with AVX2, ARM64 with NEON)

macOS: Apple Clang 13+ (Apple Silicon M-series NEON native, x86_64 Rosettas)

Windows: MSVC 2019+ / Visual Studio 2022 (x64)