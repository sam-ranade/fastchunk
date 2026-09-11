#include "fastchunk/telemetry.h"
#include "fastchunk/config.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Telemetry collector is bounded", "[telemetry]")
{
    fastchunk::BoundedTelemetryCollector collector(2);
    collector.record({ .metric_name = "first" });
    collector.record({ .metric_name = "second" });
    collector.record({ .metric_name = "third" });

    const auto events = collector.snapshot();
    REQUIRE(events.size() == 2);
    CHECK(events[0].metric_name == "second");
    CHECK(events[1].metric_name == "third");
    CHECK(collector.dropped_events() == 1);
}

TEST_CASE("Export retry configuration validates bounds", "[config]")
{
    auto configuration = fastchunk::default_configuration();
    configuration.input.path = "input.txt";
    configuration.chunking.max_tokens = 8;
    configuration.chunking.overlap_tokens = 1;
    configuration.export_options.path = "output.ndjson";
    configuration.export_options.retries.max_attempts = 0;
    CHECK_FALSE(configuration.validate().has_value());
}
