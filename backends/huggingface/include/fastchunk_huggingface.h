#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fastchunk_huggingface_backend fastchunk_huggingface_backend;
typedef struct {
    uint32_t id;
    size_t start_byte;
    size_t end_byte;
} fastchunk_huggingface_token;

int32_t fastchunk_huggingface_create(const char* path,
    fastchunk_huggingface_backend** handle, char** error);
int32_t fastchunk_huggingface_encode(fastchunk_huggingface_backend* handle,
    const uint8_t* input, size_t input_len, fastchunk_huggingface_token** output,
    size_t* output_len, char** error);
void fastchunk_huggingface_free_tokens(fastchunk_huggingface_token* tokens, size_t len);
void fastchunk_huggingface_destroy(fastchunk_huggingface_backend* handle);
void fastchunk_huggingface_free_error(char* error);

#ifdef __cplusplus
}
#endif
