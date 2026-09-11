#include "fastchunk/directory_pipeline.h"
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("Directory pipeline processes sorted files", "[pipeline]")
{
    const auto root = std::filesystem::temp_directory_path() / "fastchunk_pipeline_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "b.txt") << "second document";
    std::ofstream(root / "a.txt") << "first document";
    std::ofstream(root / ".hidden.txt") << "hidden document";

    auto configuration = fastchunk::default_configuration();
    configuration.input.path = root.string();
    configuration.input.recursive = false;
    configuration.chunking.max_tokens = 8;
    configuration.chunking.overlap_tokens = 0;
    configuration.tokenizer.type = "whitespace";
    configuration.export_options.type = "none";

    fastchunk::DirectoryPipeline pipeline;
    auto result = pipeline.run(configuration);
    REQUIRE(result.has_value());
    REQUIRE(result.value().size() == 2);
    CHECK(result.value()[0].path.filename() == "a.txt");
    CHECK(result.value()[1].path.filename() == "b.txt");

    std::filesystem::remove_all(root);
}
