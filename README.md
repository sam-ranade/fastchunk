# FastChunk

FastChunk is a C++20 text-ingestion and token-bounded chunking library for RAG and LLM preprocessing pipelines. It provides memory-mapped or copied input, UTF-8 validation, sliding token windows, NDJSON record handling, a C ABI, Python bindings, and a configuration-driven CLI.

## Status

The core C++ API, C ABI, NDJSON handling, YAML configuration, directory pipeline, NDJSON exporter, CLI, Python streaming API, asynchronous telemetry queue, and required telemetry metrics are implemented. OTLP and Splunk delivery are optional: builds without libcurl report an explicit exporter dependency warning while chunk processing continues.

## Requirements

- CMake 3.20+
- A C++20 compiler
- Git
- Rust and Cargo (only when building the native Tiktoken or Hugging Face backend)
- Python 3.10+ only when building the nanobind module
- Network access on the first configure so CMake can fetch Catch2, yaml-cpp, optionally nanobind, and the Rust backend crates
- libcurl is optional; when available it enables HTTP telemetry delivery for OTLP and Splunk HEC

## Build

```bash
cmake -S . -B build \
  -DFASTCHUNK_BUILD_CLI=ON \
  -DFASTCHUNK_BUILD_TESTS=ON \
  -DFASTCHUNK_BUILD_EXAMPLES=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Build the Python extension separately:

```bash
cmake -S . -B build-python \
  -DFASTCHUNK_BUILD_CLI=OFF \
  -DFASTCHUNK_ENABLE_PYTHON=ON \
  -DFASTCHUNK_BUILD_TESTS=OFF
cmake --build build-python --target _fastchunk_cpp --parallel
```

Native tokenizer backends are optional. The built-in `whitespace` tokenizer is
always available. Tiktoken uses native `tiktoken-rs` and supports
`cl100k_base` and `o200k_base`:

```bash
cmake -S . -B build-tiktoken \
  -DFASTCHUNK_ENABLE_TIKTOKEN=ON \
  -DFASTCHUNK_BUILD_TESTS=ON
cmake --build build-tiktoken --parallel
ctest --test-dir build-tiktoken --output-on-failure
```

Hugging Face uses the native Rust `tokenizers` library and loads a local
`tokenizer.json` file. It does not download models automatically:

```bash
cmake -S . -B build-huggingface \
  -DFASTCHUNK_ENABLE_HUGGINGFACE=ON \
  -DFASTCHUNK_BUILD_TESTS=ON
cmake --build build-huggingface --parallel
ctest --test-dir build-huggingface --output-on-failure
```

Use the Hugging Face backend with a local tokenizer file:

```yaml
tokenizer:
  type: huggingface
  tokenizer_path: ./models/tokenizer.json
```

Or use Tiktoken explicitly from the CLI:

```bash
./build-tiktoken/fastchunk chunk input.txt \
  --tokenizer cl100k_base \
  --max-tokens 256 \
  --overlap-tokens 32 \
  --input-mode zero_copy \
  --invalid-utf8 error \
  --export output.ndjson
```

Build options include `FASTCHUNK_BUILD_CLI`, `FASTCHUNK_BUILD_TESTS`,
`FASTCHUNK_BUILD_EXAMPLES`, `FASTCHUNK_BUILD_BENCHMARKS`,
`FASTCHUNK_ENABLE_PYTHON`, `FASTCHUNK_ENABLE_TIKTOKEN`,
`FASTCHUNK_ENABLE_HUGGINGFACE`, and `FASTCHUNK_ENABLE_AVX2`.

When an optional backend is disabled, selecting it returns an explicit
`tokenizer_unavailable` error. It never silently falls back to whitespace
tokenization.

### Input Path Lifetime

Input paths and files must remain present, readable, and unchanged for the full
duration of processing. Do not replace, rename, truncate, or modify an input
file while FastChunk is reading or chunking it. Results are undefined if the
input path is removed or replaced during processing. The `restricted_root`
configuration is reserved for a future race-resistant, root-anchored I/O
implementation and currently returns `unsupported_option` when configured.

## CLI

The CLI applies defaults, loads YAML, applies command-line overrides, validates the effective configuration, and processes a file or directory.

Build the CLI first:

```bash
cmake -S . -B build \
  -DFASTCHUNK_BUILD_CLI=ON \
  -DFASTCHUNK_BUILD_TESTS=ON
cmake --build build --target fastchunk --parallel
```

Process a file and write chunk records as NDJSON:

```bash
./build/fastchunk chunk input.txt \
  --tokenizer whitespace \
  --max-tokens 256 \
  --overlap-tokens 32 \
  --export output.ndjson
```

Configuration-driven execution:

```yaml
schema_version: 1
input:
  path: ./documents
  recursive: true
tokenizer:
  type: whitespace
chunking:
  max_tokens: 256
  overlap_tokens: 32
export:
  type: ndjson
  path: output.ndjson
```

```bash
./build/fastchunk chunk --config fastchunk.yaml
```

Useful overrides include `--tokenizer`, `--max-tokens`, `--overlap-tokens`, `--input-mode`, `--invalid-utf8`, `--recursive`, `--include-hidden`, `--workers`, `--result-order`, `--record-mode`, `--record-id-field`, `--on-malformed-record`, `--export`, and `--json-errors`.

Use `--json-errors` when a machine-readable error response is required:

```bash
./build/fastchunk chunk missing.txt \
  --tokenizer whitespace \
  --max-tokens 256 \
  --overlap-tokens 32 \
  --export output.ndjson \
  --json-errors
```

## Synthetic Benchmark Data

The generator creates deterministic, UTF-8 benchmark inputs for disk and
`mmap` testing. It writes files under `build/perf_data` by default and accepts
`txt`, `md`, `json`, and `xml` formats:

```bash
python3 scripts/generate_perf_datasets.py \
  --size-mb 50 \
  --formats json \
  --output-dir build/perf_data \
  --seed 42
```

Generate the complete format set:

```bash
python3 scripts/generate_perf_datasets.py \
  --size-mb 50 \
  --formats all \
  --output-dir build/perf_data
```

Generate the project benchmark sample sizes used for end-to-end checks:

```bash
python3 scripts/generate_perf_datasets.py --size-mb 50 --formats json \
  --output-dir benchmarks
python3 scripts/generate_perf_datasets.py --size-mb 10 --formats txt \
  --output-dir benchmarks
python3 scripts/generate_perf_datasets.py --size-mb 25 --formats xml \
  --output-dir benchmarks
```

Run the CLI against a generated file:

```bash
./build/fastchunk chunk build/perf_data/corpus_50mb.json \
  --tokenizer whitespace \
  --max-tokens 256 \
  --overlap-tokens 32 \
  --input-mode zero_copy \
  --invalid-utf8 error \
  --export build/perf_data/corpus_50mb.ndjson
```

The same generator is available as a CMake target when benchmarks are enabled:

```bash
cmake -S . -B build -DFASTCHUNK_BUILD_BENCHMARKS=ON
cmake --build build --target fastchunk_perf_data
```

## Python

The nanobind module exposes direct chunking and an owned streaming iterator:

```python
import _fastchunk_cpp as fastchunk

for chunk in fastchunk.stream_file("input.txt", 256, 32, "whitespace"):
    print(chunk["text"])

print(fastchunk.load_configuration_json("fastchunk.yaml"))
```

The Pythonic reader/chunker API is also available:

```python
import _fastchunk_cpp as fastchunk

chunker = fastchunk.Chunker(256, 32, "whitespace")
with fastchunk.Reader("input.txt") as reader:
  chunks = chunker.chunk(reader)
  print(chunks[0]["text"])
```

`Reader.bytes_view()` returns an owned Python `bytes` snapshot, so it remains
valid after the reader is closed. Diagnostics are emitted as Python warnings by
default. Use `fastchunk.set_warning_mode("ignore")` or
`fastchunk.set_warning_mode("error")` to control warning handling.

Benchmark results can be compared with the versioned baseline:

```bash
./build/fastchunk_bench --generated-mb 1 --iterations 10 \
  --json-output build/benchmark.json
python3 scripts/check_benchmark_baseline.py \
  benchmarks/baseline_benchmarks.json build/benchmark.json
```

## C++ and C APIs

Public C++ headers are under `include/fastchunk/`. The C ABI is declared in `include/fastchunk_abi.h`; the example programs in `examples/` demonstrate basic usage.

## Documentation

- [Architecture](architecture.md)
- [Project architecture](project_architecture.md)

## Public-readiness notes

The repository intentionally excludes build output and fetched dependencies through `.gitignore`. Do not commit tokenizer credentials, model files, private certificates, API tokens, or generated corpora. Telemetry is disabled by default; enable it explicitly and provide an endpoint. Credential values should be supplied through an environment variable configured by `telemetry.token_env`.

## License

See [LICENSE](LICENSE).
