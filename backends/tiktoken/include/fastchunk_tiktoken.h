#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fastchunk_tiktoken_backend fastchunk_tiktoken_backend;
typedef struct {
    uint32_t id;
    size_t start_byte;
    size_t end_byte;
} fastchunk_tiktoken_token;

int32_t fastchunk_tiktoken_create(const char* encoding,
    fastchunk_tiktoken_backend** handle, char** error);
int32_t fastchunk_tiktoken_encode(fastchunk_tiktoken_backend* handle,
    const uint8_t* input, size_t input_len, fastchunk_tiktoken_token** output,
    size_t* output_len, char** error);
void fastchunk_tiktoken_free_tokens(fastchunk_tiktoken_token* tokens, size_t len);
void fastchunk_tiktoken_destroy(fastchunk_tiktoken_backend* handle);
void fastchunk_tiktoken_free_error(char* error);

#ifdef __cplusplus
}
#endif
