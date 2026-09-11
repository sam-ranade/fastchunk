> **Implementation note:** The repository includes a configuration-driven CLI, shared YAML configuration library, directory pipeline, NDJSON exporter, C ABI, nanobind Python module, and optional native Rust tokenizer backends. External telemetry delivery is optional and requires libcurl.

fastchunk/
├── include/fastchunk/      # Public C++ Headers & C ABI
│   ├── core.h              # Core interfaces, Result/Error, and options
│   ├── fastchunk_c.h       # Pure C ABI header
│   ├── export.h            # Symbol visibility macros (DLL/SO)
│   ├── cancellation.h      # Cooperative thread cancellation token
│   └── stream_chunker.h    # Stateful streaming chunker engine
├── src/
│   ├── abi/                # C ABI Bridge Implementation
│   ├── bindings/           # Python nanobind wrapper
│   ├── core/               # Chunker, ParallelChunker, NDJSON, Metadata logic
│   ├── io/                 # MmapReader and SIMD UTF-8 validation
│   └── tokenizers/         # Tiktoken & Hugging Face wrappers
├── backends/
│   ├── tiktoken/            # Optional Rust tiktoken-rs C ABI adapter
│   └── huggingface/         # Optional Rust tokenizers C ABI adapter
├── examples/               # Runnable C++ and C usage examples
├── benchmarks/             # Benchmarking suite
└── tests/                  # Unit and integration test suite