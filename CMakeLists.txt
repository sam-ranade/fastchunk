cmake_minimum_required(VERSION 3.20)
project(fastchunk VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FASTCHUNK_BUILD_TESTS "Build test suite" ON)
option(FASTCHUNK_BUILD_BENCHMARKS "Build benchmark suite" OFF)
option(FASTCHUNK_ENABLE_TIKTOKEN "Build native Tiktoken backend" OFF)
option(FASTCHUNK_ENABLE_HUGGINGFACE "Build native Hugging Face backend" OFF)
option(FASTCHUNK_ENABLE_PYTHON "Build nanobind Python bindings" OFF)
option(FASTCHUNK_ENABLE_AVX2 "Enable AVX2 vector optimizations" OFF)
option(FASTCHUNK_BUILD_CLI "Build configuration-driven CLI" ON)

find_program(CARGO_EXECUTABLE cargo)

if(FASTCHUNK_ENABLE_TIKTOKEN AND NOT CARGO_EXECUTABLE)
  message(FATAL_ERROR "FASTCHUNK_ENABLE_TIKTOKEN requires cargo")
endif()
if(FASTCHUNK_ENABLE_HUGGINGFACE AND NOT CARGO_EXECUTABLE)
  message(FATAL_ERROR "FASTCHUNK_ENABLE_HUGGINGFACE requires cargo")
endif()

if(FASTCHUNK_ENABLE_TIKTOKEN)
  set(FASTCHUNK_TIKTOKEN_LIB ${CMAKE_CURRENT_SOURCE_DIR}/backends/tiktoken/target/release/libfastchunk_tiktoken.a)
  add_custom_command(
    OUTPUT ${FASTCHUNK_TIKTOKEN_LIB}
    COMMAND ${CARGO_EXECUTABLE} build --release --locked --manifest-path
      ${CMAKE_CURRENT_SOURCE_DIR}/backends/tiktoken/Cargo.toml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/backends/tiktoken
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/backends/tiktoken/Cargo.toml
      ${CMAKE_CURRENT_SOURCE_DIR}/backends/tiktoken/Cargo.lock
      ${CMAKE_CURRENT_SOURCE_DIR}/backends/tiktoken/src/lib.rs
    COMMENT "Building native Tiktoken backend")
  add_custom_target(fastchunk-tiktoken-build DEPENDS ${FASTCHUNK_TIKTOKEN_LIB})
  add_library(fastchunk-tiktoken STATIC IMPORTED GLOBAL)
  set_target_properties(fastchunk-tiktoken PROPERTIES
    IMPORTED_LOCATION ${FASTCHUNK_TIKTOKEN_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/backends/tiktoken/include)
  add_dependencies(fastchunk-tiktoken fastchunk-tiktoken-build)
endif()

if(FASTCHUNK_ENABLE_HUGGINGFACE)
  set(FASTCHUNK_HUGGINGFACE_LIB ${CMAKE_CURRENT_SOURCE_DIR}/backends/huggingface/target/release/libfastchunk_huggingface.a)
  add_custom_command(
    OUTPUT ${FASTCHUNK_HUGGINGFACE_LIB}
    COMMAND ${CARGO_EXECUTABLE} build --release --locked --manifest-path
      ${CMAKE_CURRENT_SOURCE_DIR}/backends/huggingface/Cargo.toml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/backends/huggingface
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/backends/huggingface/Cargo.toml
      ${CMAKE_CURRENT_SOURCE_DIR}/backends/huggingface/Cargo.lock
      ${CMAKE_CURRENT_SOURCE_DIR}/backends/huggingface/src/lib.rs
    COMMENT "Building native Hugging Face backend")
  add_custom_target(fastchunk-huggingface-build DEPENDS ${FASTCHUNK_HUGGINGFACE_LIB})
  add_library(fastchunk-huggingface STATIC IMPORTED GLOBAL)
  set_target_properties(fastchunk-huggingface PROPERTIES
    IMPORTED_LOCATION ${FASTCHUNK_HUGGINGFACE_LIB}
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/backends/huggingface/include)
  add_dependencies(fastchunk-huggingface fastchunk-huggingface-build)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  if(FASTCHUNK_ENABLE_AVX2)
    add_compile_options(/arch:AVX2)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(FASTCHUNK_ENABLE_AVX2)
    add_compile_options(-mavx2)
  endif()
endif()

include_directories(include)

add_library(fastchunk-core STATIC
  src/io/mmap_reader.cpp
  src/core/chunker.cpp
  src/tokenizers/tokenizers.cpp
)
set_target_properties(fastchunk-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(fastchunk-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(FASTCHUNK_ENABLE_TIKTOKEN)
  target_compile_definitions(fastchunk-core PRIVATE FASTCHUNK_HAS_TIKTOKEN=1)
  target_link_libraries(fastchunk-core PRIVATE fastchunk-tiktoken dl pthread m)
endif()
if(FASTCHUNK_ENABLE_HUGGINGFACE)
  target_compile_definitions(fastchunk-core PRIVATE FASTCHUNK_HAS_HUGGINGFACE=1)
  target_link_libraries(fastchunk-core PRIVATE fastchunk-huggingface dl pthread m)
endif()

include(FetchContent)
FetchContent_Declare(
  yaml_cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
  GIT_SHALLOW TRUE
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml_cpp)

add_library(fastchunk-config STATIC
  src/config.cpp
  src/directory_pipeline.cpp
  src/telemetry.cpp
)
set_target_properties(fastchunk-config PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(fastchunk-config PUBLIC fastchunk-core yaml-cpp::yaml-cpp)
target_include_directories(fastchunk-config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
find_package(CURL QUIET)
if(CURL_FOUND)
  target_link_libraries(fastchunk-config PRIVATE CURL::libcurl)
  target_compile_definitions(fastchunk-config PRIVATE FASTCHUNK_HAS_CURL=1)
endif()

if(FASTCHUNK_BUILD_CLI)
  add_executable(fastchunk src/cli/main.cpp)
  target_link_libraries(fastchunk PRIVATE fastchunk-config)
endif()

add_library(fastchunk-c SHARED src/abi/spec_abi.cpp)
target_link_libraries(fastchunk-c PRIVATE fastchunk-core)

if(FASTCHUNK_ENABLE_PYTHON)
  find_package(Python 3.10 COMPONENTS Interpreter Development.Module REQUIRED)
  FetchContent_Declare(
    nanobind
    GIT_REPOSITORY https://github.com/wjakob/nanobind.git
    GIT_TAG v2.0.0
  )
  FetchContent_MakeAvailable(nanobind)
  nanobind_add_module(_fastchunk_cpp src/bindings/python/fastchunk_py.cpp)
  target_link_libraries(_fastchunk_cpp PRIVATE fastchunk-config)
endif()

option(FASTCHUNK_BUILD_EXAMPLES "Build example executables" ON)
if(FASTCHUNK_BUILD_EXAMPLES)
  add_executable(simple_chunking examples/simple_chunking.cpp)
  target_link_libraries(simple_chunking PRIVATE fastchunk-core)
  add_executable(c_api_example examples/c_api_example.c)
  target_link_libraries(c_api_example PRIVATE fastchunk-c)
endif()

if(FASTCHUNK_BUILD_BENCHMARKS)
  add_executable(fastchunk_bench benchmarks/bench_main.cpp)
  target_link_libraries(fastchunk_bench PRIVATE fastchunk-core)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_custom_target(fastchunk_perf_data
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_perf_datasets.py
        --output-dir ${CMAKE_CURRENT_BINARY_DIR}/perf_data --size-mb 50 --formats all
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Generating FastChunk performance datasets")
  endif()
endif()

if(FASTCHUNK_BUILD_TESTS)
  enable_testing()
  find_package(Catch2 3 QUIET)
  if(NOT Catch2_FOUND)
    FetchContent_Declare(
      Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.8.1
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
  endif()
  add_executable(fastchunk-tests
    tests/unit/test_reader.cpp
    tests/unit/functional.cpp
    tests/unit/abi_ndjson.cpp
    tests/unit/exporter.cpp
    tests/unit/config.cpp
    tests/unit/directory_pipeline.cpp
    tests/unit/telemetry.cpp
    tests/unit/tokenizer_backend.cpp
  )
  target_link_libraries(fastchunk-tests PRIVATE fastchunk-config fastchunk-c Catch2::Catch2WithMain)
  include(CTest)
  add_test(NAME fastchunk-tests COMMAND fastchunk-tests)
endif()
