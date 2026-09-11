#!/usr/bin/env python3
"""
Performance Dataset Generator for fastchunk.

Generates structured and unstructured benchmark files (.txt, .md, .json, .xml)
containing realistic natural language, code blocks, tables, and multibyte UTF-8 strings
for testing IReader, chunking throughput, mmap page faults, and tokenizers.
"""

import argparse
import json
import os
import random
import sys
import time
from pathlib import Path

# Sample vocabularies and multilingual phrases for multibyte UTF-8 testing
WORD_POOL = [
    "hyperplane", "transformer", "latency", "vector", "embedding", "tokenizer",
    "throughput", "zero-copy", "buffer", "concurrency", "pipeline", "cache",
    "retrieval", "generation", "streaming", "memory-map", "allocator", "index",
    "distributed", "asynchronous", "SIMD", "AVX2", "multibyte", "boundary",
    "invariant", "benchmark", "serialization", "deserialization", "subword", "corpus"
]

MULTIBYTE_SAMPLES = [
    "こんにちは世界 (Japanese greeting)",
    "你好，世界！(Chinese greeting)",
    "안녕하세요 세계 (Korean greeting)",
    "Привет, мир! (Russian greeting)",
    "مرحبا بالعالم (Arabic greeting)",
    "שלום עולם (Hebrew greeting)",
    "Héllo Wörld with áccènts",
    "🚀 High-performance zero-copy text processing ⚡️",
    "Math symbols: ∀x ∈ ℝ, ∃y: x + y = 0, ∑_{i=1}^n i = n(n+1)/2, ∫_0^∞ e^{-x^2} dx = √π/2",
    "Emoji sequences: 👩🏽‍💻 👨‍👩‍👧‍👦 🏳️‍🌈 🛰️ 📦 🔍 🧠",
]

CODE_SNIPPETS = [
    '```cpp\nauto reader = fastchunk::create_mmap_reader();\nauto buffer = reader->open(filepath, options);\n```',
    '```python\nimport fastchunk\nchunks = fastchunk.chunk_file("corpus.txt", max_tokens=512)\n```',
    '```json\n{"status": "ok", "latency_ms": 1.42, "processed_bytes": 1048576}\n```',
    '```rust\nfn process_chunk(data: &[u8]) -> Result<Vec<Chunk>, Error> {\n    // zero-copy chunking\n}\n```',
]


def random_sentence(rng: random.Random) -> str:
    length = rng.randint(8, 20)
    words = [rng.choice(WORD_POOL) for _ in range(length)]
    words[0] = words[0].capitalize()
    sentence = " ".join(words) + "."
    if rng.random() < 0.2:
        sentence += " " + rng.choice(MULTIBYTE_SAMPLES)
    return sentence


def random_paragraph(rng: random.Random, num_sentences: int = 5) -> str:
    return " ".join(random_sentence(rng) for _ in range(num_sentences))


def generate_txt(target_bytes: int, output_path: Path, rng: random.Random) -> int:
    written = 0
    with open(output_path, "w", encoding="utf-8", buffering=1024 * 1024) as f:
        while written < target_bytes:
            p = random_paragraph(rng, num_sentences=rng.randint(3, 8)) + "\n\n"
            f.write(p)
            written += len(p.encode("utf-8"))
    return written


def generate_md(target_bytes: int, output_path: Path, rng: random.Random) -> int:
    written = 0
    section_counter = 1
    with open(output_path, "w", encoding="utf-8", buffering=1024 * 1024) as f:
        header = f"# FastChunk Benchmark Corpus - Markdown Payload\n\n"
        f.write(header)
        written += len(header.encode("utf-8"))

        while written < target_bytes:
            block_type = rng.choice(["paragraph", "code", "table", "list", "header"])
            if block_type == "header":
                text = f"\n## Section {section_counter}: Benchmark Invariant {rng.choice(WORD_POOL).capitalize()}\n\n"
                section_counter += 1
            elif block_type == "code":
                text = rng.choice(CODE_SNIPPETS) + "\n\n"
            elif block_type == "table":
                text = (
                    "| Parameter | Value | Description |\n"
                    "|---|---|---|\n"
                    f"| Block ID | {rng.randint(1000, 9999)} | {random_sentence(rng)} |\n"
                    f"| Throughput Target | >1.0 GB/s | {rng.choice(MULTIBYTE_SAMPLES)} |\n\n"
                )
            elif block_type == "list":
                items = [f"- Item {i}: {random_sentence(rng)}" for i in range(rng.randint(3, 6))]
                text = "\n".join(items) + "\n\n"
            else:
                text = random_paragraph(rng, num_sentences=rng.randint(4, 7)) + "\n\n"

            f.write(text)
            written += len(text.encode("utf-8"))
    return written


def generate_json(target_bytes: int, output_path: Path, rng: random.Random) -> int:
    written = 0
    doc_id = 1
    with open(output_path, "w", encoding="utf-8", buffering=1024 * 1024) as f:
        f.write("[\n")
        written += 2
        first = True

        while written < target_bytes:
            record = {
                "doc_id": f"doc_{doc_id:08d}",
                "timestamp": 1700000000 + doc_id,
                "title": f"Document {doc_id}: {rng.choice(WORD_POOL).capitalize()}",
                "body": random_paragraph(rng, num_sentences=rng.randint(3, 6)),
                "multibyte_sample": rng.choice(MULTIBYTE_SAMPLES),
                "tags": [rng.choice(WORD_POOL) for _ in range(rng.randint(2, 5))],
                "metrics": {
                    "word_count": rng.randint(50, 500),
                    "importance_score": round(rng.uniform(0.1, 1.0), 4),
                }
            }
            doc_id += 1
            encoded = json.dumps(record, ensure_ascii=False, indent=2)
            # Indent each line
            indented = "  " + encoded.replace("\n", "\n  ")
            item_str = (",\n" if not first else "") + indented
            first = False

            f.write(item_str)
            written += len(item_str.encode("utf-8"))

        f.write("\n]\n")
        written += 2
    return written


def generate_xml(target_bytes: int, output_path: Path, rng: random.Random) -> int:
    written = 0
    doc_id = 1
    with open(output_path, "w", encoding="utf-8", buffering=1024 * 1024) as f:
        header = '<?xml version="1.0" encoding="UTF-8"?>\n<corpus>\n'
        f.write(header)
        written += len(header.encode("utf-8"))

        while written < target_bytes:
            title = f"Doc {doc_id} - {rng.choice(WORD_POOL)}"
            sample = rng.choice(MULTIBYTE_SAMPLES).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            body = random_paragraph(rng, num_sentences=rng.randint(3, 5)).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

            entry = (
                f'  <document id="{doc_id:08d}">\n'
                f'    <title>{title}</title>\n'
                f'    <multibyte>{sample}</multibyte>\n'
                f'    <content>\n'
                f'      <![CDATA[{body}]]>\n'
                f'    </content>\n'
                f'  </document>\n'
            )
            doc_id += 1
            f.write(entry)
            written += len(entry.encode("utf-8"))

        footer = '</corpus>\n'
        f.write(footer)
        written += len(footer.encode("utf-8"))
    return written


GENERATORS = {
    "txt": generate_txt,
    "md": generate_md,
    "json": generate_json,
    "xml": generate_xml,
}


def main():
    parser = argparse.ArgumentParser(
        description="Generate synthetic performance test datasets for fastchunk."
    )
    parser.add_argument(
        "--size-mb",
        type=float,
        default=50.0,
        help="Target size in megabytes (MB) per generated dataset file (default: 50.0).",
    )
    parser.add_argument(
        "--output-dir",
        type=str,
        default="build/perf_data",
        help="Target output directory (default: build/perf_data).",
    )
    parser.add_argument(
        "--formats",
        nargs="+",
        choices=["txt", "md", "json", "xml", "all"],
        default=["all"],
        help="File format(s) to generate (default: all).",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="Random seed for reproducible dataset generation (default: 42).",
    )

    args = parser.parse_args()

    formats = args.formats
    if "all" in formats:
        formats = ["txt", "md", "json", "xml"]

    target_bytes = int(args.size_mb * 1024 * 1024)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    rng = random.Random(args.seed)

    print(f"=== FastChunk Synthetic Data Generator ===")
    print(f"Target size per file: {args.size_mb:.2f} MB ({target_bytes:,} bytes)")
    print(f"Output directory:     {out_dir.resolve()}")
    print(f"Formats:              {', '.join(formats)}")
    print(f"Seed:                 {args.seed}\n")

    total_start = time.time()
    generated_files = []

    for fmt in formats:
        gen_fn = GENERATORS.get(fmt)
        if not gen_fn:
            continue
        file_name = f"corpus_{int(args.size_mb)}mb.{fmt}"
        file_path = out_dir / file_name
        print(f"Generating [{fmt.upper()}] -> {file_path.name} ...", end="", flush=True)

        start_time = time.time()
        actual_bytes = gen_fn(target_bytes, file_path, rng)
        elapsed = time.time() - start_time
        mb_written = actual_bytes / (1024 * 1024)
        throughput = mb_written / elapsed if elapsed > 0 else 0

        print(f" Done! ({mb_written:.2f} MB written in {elapsed:.2f}s, {throughput:.1f} MB/s)")
        generated_files.append((file_path, actual_bytes))

    total_elapsed = time.time() - total_start
    total_mb = sum(b for _, b in generated_files) / (1024 * 1024)
    print(f"\nSuccessfully generated {len(generated_files)} files ({total_mb:.2f} MB total) in {total_elapsed:.2f}s.")


if __name__ == "__main__":
    main()
