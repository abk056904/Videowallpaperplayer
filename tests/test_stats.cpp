#include "doctest.h"

#include <atomic>
#include <thread>
#include <vector>

#include "performance/StatsCollector.h"

using vw::performance::StatsCollector;
using vw::ui::TelemetrySnapshot;

TEST_CASE("stats: default snapshot is all-zero / empty") {
    StatsCollector sc;
    const auto s = sc.snapshot();
    CHECK(s.cpuUsage == 0.0);
    CHECK(s.gpuUsage == 0.0);
    CHECK(s.gpuMemoryUsed == 0);
    CHECK(s.gpuMemoryBudget == 0);
    CHECK(s.systemMemoryUsed == 0);
    CHECK(s.decodedFps == 0.0);
    CHECK(s.presentedFps == 0.0);
    CHECK(s.droppedFrames == 0);
    CHECK(s.decodeLatencyMs == 0.0);
    CHECK(s.renderTimeMs == 0.0);
    CHECK_FALSE(s.hardwareDecode);
    CHECK(s.perMonitor.empty());
}

TEST_CASE("stats: updatePlayback fills the playback fields") {
    StatsCollector sc;
    sc.updatePlayback(36.4, 36.4, 3, 12.5, 0.4, true);
    const auto s = sc.snapshot();
    CHECK(s.decodedFps == doctest::Approx(36.4));
    CHECK(s.presentedFps == doctest::Approx(36.4));
    CHECK(s.droppedFrames == 3);
    CHECK(s.decodeLatencyMs == doctest::Approx(12.5));
    CHECK(s.renderTimeMs == doctest::Approx(0.4));
    CHECK(s.hardwareDecode);
    // Workload fields untouched by the playback path (M9 fills them).
    CHECK(s.cpuUsage == 0.0);
    CHECK(s.gpuMemoryUsed == 0);
}

TEST_CASE("stats: later updates overwrite earlier ones") {
    StatsCollector sc;
    sc.updatePlayback(10.0, 9.0, 1, 5.0, 0.1, false);
    sc.updatePlayback(20.0, 19.5, 2, 6.0, 0.2, true);
    const auto s = sc.snapshot();
    CHECK(s.decodedFps == doctest::Approx(20.0));
    CHECK(s.presentedFps == doctest::Approx(19.5));
    CHECK(s.droppedFrames == 2);
    CHECK(s.decodeLatencyMs == doctest::Approx(6.0));
    CHECK(s.renderTimeMs == doctest::Approx(0.2));
    CHECK(s.hardwareDecode);
}

TEST_CASE("stats: updatePerMonitor appends then replaces by id") {
    StatsCollector sc;
    sc.updatePerMonitor(L"mon-1", 30.0, 0);
    sc.updatePerMonitor(L"mon-2", 25.0, 4);
    auto s = sc.snapshot();
    REQUIRE(s.perMonitor.size() == 2);
    CHECK(s.perMonitor[0].monitorId == L"mon-1");
    CHECK(s.perMonitor[0].presentedFps == doctest::Approx(30.0));
    CHECK(s.perMonitor[1].monitorId == L"mon-2");
    CHECK(s.perMonitor[1].droppedFrames == 4);

    // Same id replaces in place (no duplicate entries).
    sc.updatePerMonitor(L"mon-1", 33.3, 1);
    s = sc.snapshot();
    REQUIRE(s.perMonitor.size() == 2);
    CHECK(s.perMonitor[0].monitorId == L"mon-1");
    CHECK(s.perMonitor[0].presentedFps == doctest::Approx(33.3));
    CHECK(s.perMonitor[0].droppedFrames == 1);
    CHECK(s.perMonitor[1].monitorId == L"mon-2"); // untouched
}

TEST_CASE("stats: snapshot returns an independent copy") {
    StatsCollector sc;
    sc.updatePlayback(10.0, 9.0, 0, 1.0, 0.1, false);
    auto s = sc.snapshot();
    // Mutating the collector after the snapshot must not affect the copy.
    sc.updatePlayback(99.0, 98.0, 9, 9.0, 9.9, true);
    CHECK(s.decodedFps == doctest::Approx(10.0));
    CHECK(s.presentedFps == doctest::Approx(9.0));
    CHECK_FALSE(s.hardwareDecode);
    CHECK(s.perMonitor.empty());
}

TEST_CASE("stats: reset clears everything") {
    StatsCollector sc;
    sc.updatePlayback(30.0, 29.0, 2, 3.0, 0.3, true);
    sc.updatePerMonitor(L"mon-1", 29.0, 2);
    sc.reset();
    const auto s = sc.snapshot();
    CHECK(s.decodedFps == 0.0);
    CHECK(s.droppedFrames == 0);
    CHECK_FALSE(s.hardwareDecode);
    CHECK(s.perMonitor.empty());
}

TEST_CASE("stats: concurrent updates and reads are safe") {
    StatsCollector sc;
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) {
        writers.emplace_back([&, i] {
            while (!stop.load()) {
                sc.updatePlayback(static_cast<double>(i), 10.0 + i, static_cast<uint64_t>(i),
                                  static_cast<double>(i), 0.1, i % 2 == 0);
                sc.updatePerMonitor(L"mon-" + std::to_wstring(i), 20.0 + i,
                                    static_cast<uint64_t>(i));
            }
        });
    }
    std::thread reader([&] {
        while (!stop.load()) {
            const auto s = sc.snapshot();
            // The copy must be internally consistent (never torn).
            CHECK(s.perMonitor.size() <= 4);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    for (auto& t : writers) t.join();
    reader.join();
    const auto s = sc.snapshot();
    CHECK(s.perMonitor.size() >= 1);
    CHECK(s.perMonitor.size() <= 4);
}
