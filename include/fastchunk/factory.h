#pragma once

#include "fastchunk/core.h"
#include <memory>

namespace fastchunk
{

[[nodiscard]] std::unique_ptr<IReader> create_mmap_reader();
[[nodiscard]] std::unique_ptr<IChunker> create_chunker();

} // namespace fastchunk
