import _fastchunk_cpp as fastchunk

# The current binding exposes file chunking as a single operation.
chunks = fastchunk.chunk_file(
    "large_corpus.txt",
    max_tokens=512,
    overlap_tokens=64,
    tokenizer_name="cl100k_base",
)

for chunk in chunks:
    print(f"Index: {chunk.chunk_index} | Bytes: {chunk.start_byte}..{chunk.end_byte}")
    print(f"Content preview: {chunk.text[:50]}...\n")