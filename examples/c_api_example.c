#include "fastchunk_abi.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    fastchunk_reader_t* reader = NULL;
    fastchunk_error_t* error = NULL;
    if (fastchunk_reader_create(&reader, &error) != 0) {
        fprintf(stderr, "Failed to create reader\n");
        fastchunk_error_release(error);
        return 1;
    }

    const char* path_text = "sample.txt";
    fastchunk_bytes_t path = {
        (const unsigned char*)path_text,
        strlen(path_text)
    };
    fastchunk_reader_options_t in_opts = {
        .input_mode = FASTCHUNK_INPUT_ZERO_COPY,
        .invalid_utf8_policy = FASTCHUNK_UTF8_ERROR,
        .record_mode = FASTCHUNK_RECORD_NONE,
        .malformed_record_policy = FASTCHUNK_RECORD_ERROR,
        .record_id_field = { NULL, 0 }
    };

    if (fastchunk_reader_open_path(reader, &path, &in_opts) != 0) {
        fastchunk_error_view_t view;
        fastchunk_reader_get_error(reader, &view);
        fprintf(stderr, "Error [%u]: %.*s\n", view.code,
                (int)view.message.size, (const char*)view.message.data);
        fastchunk_reader_destroy(reader);
        return 1;
    }

    fastchunk_chunker_options_t chunk_options = {
        .max_tokens = 256,
        .overlap_tokens = 32,
        .metadata_collision_policy = FASTCHUNK_METADATA_ERROR
    };
    fastchunk_bytes_t tokenizer_name = {
        (const unsigned char*)"tiktoken", 8
    };
    fastchunk_tokenizer_t* tokenizer = NULL;
    if (fastchunk_tokenizer_create(&tokenizer_name, NULL, &tokenizer, &error) != 0) {
        fastchunk_error_release(error);
        fastchunk_reader_close(reader);
        fastchunk_reader_destroy(reader);
        return 1;
    }

    fastchunk_chunker_t* chunker = NULL;
    if (fastchunk_chunker_create(&chunk_options, tokenizer, &chunker, &error) != 0) {
        fastchunk_error_release(error);
        fastchunk_tokenizer_destroy(tokenizer);
        fastchunk_reader_close(reader);
        fastchunk_reader_destroy(reader);
        return 1;
    }

    fastchunk_result_t* result = NULL;
    if (fastchunk_chunker_run(chunker, reader, NULL, &result) != 0) {
        fastchunk_chunker_destroy(chunker);
        fastchunk_tokenizer_destroy(tokenizer);
        fastchunk_reader_close(reader);
        fastchunk_reader_destroy(reader);
        return 1;
    }

    size_t count = 0;
    fastchunk_result_chunk_count(result, &count);
    printf("Successfully generated %zu chunks (ABI v%u)\n", count,
           fastchunk_abi_version());

    fastchunk_result_release(result);
    fastchunk_chunker_destroy(chunker);
    fastchunk_tokenizer_destroy(tokenizer);
    fastchunk_reader_close(reader);
    fastchunk_reader_destroy(reader);
    return 0;
}