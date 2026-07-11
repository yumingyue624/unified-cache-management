#include <algorithm>
#include <chrono>
#include <csignal>
#include <future>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "drampool_config.h"
#include "drampool_daemon.h"
#include "drampool_server.h"

namespace UC::DRAMPOOL {
namespace {

DramPoolConfig MakeValidConfig()
{
    DramPoolConfig config;
    config.serverId = "drampool-test";
    config.addr = "127.0.0.1:19000";
    config.nics = {"mlx5_0", "mlx5_1"};
    config.poolSizeGb = 1;
    config.poolBlockSizes = {4096, 8192};
    config.poolBlockProportions = {70, 30};
    config.transportMode = "hixl";
    config.transportLocalEngine = "drampool-test";
    config.transportDeviceId = 0;
    config.metadataShards = 4;
    config.requestQueueDepth = 128;
    config.handleQueueDepth = 128;
    config.pollerDrainBudget = 64;
    config.pollerScanBudget = 64;
    config.pollerMaxPending = 1024;
    config.pollerIdleWaitUs = 100;
    config.gcEnabled = true;
    config.gcIntervalMs = 10;
    config.opTimeoutMs = 5000;
    config.shutdownTimeoutMs = 30000;
    if (const auto status = ResolvePoolSlotCounts(config); status.Failure()) {
        throw std::runtime_error(status.ToString());
    }
    return config;
}

#if defined(UCM_DRAMPOOL_RUNTIME_INTEGRATION_TESTS)
class ScopedDramPoolConfig {
public:
    explicit ScopedDramPoolConfig(DramPoolConfig config)
        : previous_(g_config)
    {
        g_config = std::move(config);
    }

    ~ScopedDramPoolConfig() { g_config = std::move(previous_); }

    ScopedDramPoolConfig(const ScopedDramPoolConfig&) = delete;
    ScopedDramPoolConfig& operator=(const ScopedDramPoolConfig&) = delete;

private:
    DramPoolConfig previous_;
};
#endif

#if defined(UCM_DRAMPOOL_RUNTIME_INTEGRATION_TESTS)
bool ContainsInOrder(const std::vector<std::string>& events,
                     const std::vector<std::string>& expected)
{
    auto iter = events.begin();
    for (const auto& item : expected) {
        iter = std::find(iter, events.end(), item);
        if (iter == events.end()) { return false; }
        ++iter;
    }
    return true;
}
#endif

}  // namespace

TEST(DramPoolConfigTest, ParsesLaunchOptions)
{
    const char* argv[] = {
        "drampool",
        "--addr",
        "127.0.0.1:9000",
        "--nics",
        "mlx5_0",
        "mlx5_1",
        "--pool-size-gb",
        "128",
        "--kvcache-block-sizes",
        "4096",
        "8192",
        "--kvcache-block-proportions",
        "1",
        "3",
        "--ttl-minutes=30",
    };
    DramPoolConfig config;

    const auto status = ParseCommandLine(15, const_cast<char**>(argv), config);

    ASSERT_TRUE(status.Success()) << status.ToString();
    EXPECT_EQ(config.addr, "127.0.0.1:9000");
    EXPECT_EQ(config.nics, (std::vector<std::string>{"mlx5_0", "mlx5_1"}));
    EXPECT_EQ(config.poolSizeGb, 128U);
    EXPECT_EQ(config.poolBlockSizes, (std::vector<std::uint64_t>{4096, 8192}));
    EXPECT_EQ(config.poolBlockProportions, (std::vector<std::uint32_t>{1, 3}));
    EXPECT_EQ(config.poolSlotCounts, (std::vector<std::uint32_t>{8'388'608, 12'582'912}));
    EXPECT_EQ(config.defaultDumpTtlMs, 30U * kMillisecondsPerMinute);
}

TEST(DramPoolConfigTest, DefaultsBlockProportionsAndTtl)
{
    const char* argv[] = {
        "drampool",
        "--addr=127.0.0.1:9000",
        "--nics=mlx5_0",
        "--pool-size-gb=64",
        "--kvcache-block-sizes=4096",
        "8192",
    };
    DramPoolConfig config;

    const auto status = ParseCommandLine(6, const_cast<char**>(argv), config);

    ASSERT_TRUE(status.Success()) << status.ToString();
    EXPECT_EQ(config.poolBlockProportions, (std::vector<std::uint32_t>{1, 1}));
    EXPECT_EQ(config.defaultDumpTtlMs, kDefaultDumpTtlMs);
}

TEST(DramPoolConfigTest, ResolvesGiBSlotCountsWithAlignedStride)
{
    const char* argv[] = {
        "drampool",
        "--addr=127.0.0.1:9000",
        "--nics=mlx5_0",
        "--pool-size-gb=1",
        "--kvcache-block-sizes=4097",
    };
    DramPoolConfig config;

    const auto status = ParseCommandLine(5, const_cast<char**>(argv), config);

    ASSERT_TRUE(status.Success()) << status.ToString();
    EXPECT_EQ(config.poolSlotCounts, (std::vector<std::uint32_t>{258'111}));
}

TEST(DramPoolConfigTest, RejectsInvalidCommandLines)
{
    {
        const char* argv[] = {"drampool", "--help"};
        DramPoolConfig config;
        EXPECT_TRUE(ParseCommandLine(2, const_cast<char**>(argv), config).Failure());
    }
    {
        const char* argv[] = {"drampool"};
        DramPoolConfig config;
        EXPECT_TRUE(ParseCommandLine(1, const_cast<char**>(argv), config).Failure());
    }
    {
        const char* argv[] = {"drampool", "--addr", "127.0.0.1:9000", "--pool-size-gb", "1",
                              "--kvcache-block-sizes", "4096"};
        DramPoolConfig config;
        EXPECT_TRUE(ParseCommandLine(7, const_cast<char**>(argv), config).Failure());
    }
    {
        const char* argv[] = {"drampool", "--config", "legacy.conf"};
        DramPoolConfig config;
        EXPECT_TRUE(ParseCommandLine(3, const_cast<char**>(argv), config).Failure());
    }
    {
        const char* argv[] = {
            "drampool", "--addr", "127.0.0.1:9000", "--nics", "mlx5_0", "--pool-size-gb",
            "1",        "--kvcache-block-sizes", "4096", "8192",
            "--kvcache-block-proportions", "1", "--ttl-minutes", "0",
        };
        DramPoolConfig config;
        EXPECT_TRUE(ParseCommandLine(14, const_cast<char**>(argv), config).Failure());
    }
    {
        const char* argv[] = {
            "drampool", "--addr", "127.0.0.1:9000", "--nics", "mlx5_0", "--pool-size-gb=-1",
            "--kvcache-block-sizes", "4096",
        };
        DramPoolConfig config;
        EXPECT_TRUE(ParseCommandLine(8, const_cast<char**>(argv), config).Failure());
    }
}

TEST(DramPoolConfigTest, RejectsInvalidCoreConfig)
{
    auto expectInvalid = [](auto mutate) {
        auto config = MakeValidConfig();
        mutate(config);
        EXPECT_TRUE(ValidateDramPoolConfig(config).Failure());
    };

    expectInvalid([](DramPoolConfig& config) { config.serverId.clear(); });
    expectInvalid([](DramPoolConfig& config) { config.addr.clear(); });
    expectInvalid([](DramPoolConfig& config) { config.nics.clear(); });
    expectInvalid([](DramPoolConfig& config) { config.transportMode = "udp"; });
    expectInvalid([](DramPoolConfig& config) { config.transportLocalEngine.clear(); });
    expectInvalid([](DramPoolConfig& config) { config.transportDeviceId = -1; });
    expectInvalid([](DramPoolConfig& config) { config.poolSizeGb = 0; });
    expectInvalid([](DramPoolConfig& config) { config.poolBlockSizes.clear(); });
    expectInvalid([](DramPoolConfig& config) { config.poolBlockSizes[0] = 0; });
    expectInvalid([](DramPoolConfig& config) { config.poolBlockProportions.clear(); });
    expectInvalid([](DramPoolConfig& config) { config.poolBlockProportions[0] = 0; });
    expectInvalid([](DramPoolConfig& config) { config.poolBlockProportions = {100}; });
    expectInvalid([](DramPoolConfig& config) { config.defaultDumpTtlMs = 0; });
    expectInvalid([](DramPoolConfig& config) { config.metadataShards = 0; });
    expectInvalid([](DramPoolConfig& config) { config.requestQueueDepth = 1; });
    expectInvalid([](DramPoolConfig& config) { config.handleQueueDepth = 1; });
    expectInvalid([](DramPoolConfig& config) { config.pollerDrainBudget = 0; });
    expectInvalid([](DramPoolConfig& config) { config.pollerScanBudget = 0; });
    expectInvalid([](DramPoolConfig& config) { config.pollerMaxPending = 0; });
    expectInvalid([](DramPoolConfig& config) { config.pollerMaxPending = 32; });
    expectInvalid([](DramPoolConfig& config) { config.pollerIdleWaitUs = 0; });
    expectInvalid([](DramPoolConfig& config) { config.gcIntervalMs = 0; });
    expectInvalid([](DramPoolConfig& config) { config.opTimeoutMs = 0; });
    expectInvalid([](DramPoolConfig& config) { config.shutdownTimeoutMs = 0; });
}

TEST(DramPoolConfigTest, AcceptsGcDisabledWithHixlTransport)
{
    auto config = MakeValidConfig();
    config.gcEnabled = false;
    config.gcIntervalMs = 0;
    const auto status = ValidateDramPoolConfig(config);
    EXPECT_TRUE(status.Success()) << status.ToString();
}

TEST(DramPoolServerTest, RejectsCallsOutsideValidState)
{
    DramPoolServer server;
    EXPECT_TRUE(server.Start().Failure());
}

#if defined(UCM_DRAMPOOL_RUNTIME_INTEGRATION_TESTS)
TEST(DramPoolServerTest, StartsReceiverLastAndStopsReceiverFirst)
{
    ScopedDramPoolConfig configScope(MakeValidConfig());
    DramPoolServer server;
    auto status = server.Init();
    ASSERT_TRUE(status.Success()) << status.ToString();

    status = server.Start();
    ASSERT_TRUE(status.Success()) << status.ToString();
    EXPECT_TRUE(server.IsServiceReady());

    server.Stop();
    EXPECT_FALSE(server.IsServiceReady());

    const auto events = server.LifecycleEvents();
    const std::vector<std::string> startupOrder = {
        "InitMemoryPool",
        "InitMetadata",
        "InitProtocol",
        "InitQueues",
        "StartTransportService",
        "CreateRuntimeContext",
        "StartCompletionPoller",
        "StartTaskWorker",
        "StartGCThread",
        "StartListeningService",
        "SetServiceReady(true)",
    };
    EXPECT_TRUE(ContainsInOrder(events, startupOrder));

    const std::vector<std::string> shutdownOrder = {
        "SetServiceReady(false)",       "StopReceiver",         "StopTaskWorker",
        "MarkInflightTransportsFailed", "StopCompletionPoller", "StopGCThread",
        "UnregisterBufferMemory",       "DestroyMetadataIndex",
    };
    EXPECT_TRUE(ContainsInOrder(events, shutdownOrder));
}

TEST(DramPoolServerTest, GcDisabledSkipsGcThreadLifecycleEvents)
{
    auto config = MakeValidConfig();
    config.gcEnabled = false;
    config.gcIntervalMs = 0;
    ScopedDramPoolConfig configScope(std::move(config));

    DramPoolServer server;
    auto status = server.Init();
    ASSERT_TRUE(status.Success()) << status.ToString();
    ASSERT_TRUE(server.Start().Success());
    server.Stop();

    const auto events = server.LifecycleEvents();
    EXPECT_EQ(std::find(events.begin(), events.end(), "StartGCThread"), events.end());
    EXPECT_EQ(std::find(events.begin(), events.end(), "StopGCThread"), events.end());
    EXPECT_TRUE(
        ContainsInOrder(events, {"StartCompletionPoller", "StartTaskWorker",
                                 "StartListeningService", "SetServiceReady(true)"}));
}
#endif

TEST(DramPoolDaemonTest, ReturnsForInvalidArguments)
{
    {
        const char* argv[] = {"drampool"};
        DramPoolDaemon daemon;
        EXPECT_EQ(daemon.Run(1, const_cast<char**>(argv)), 1);
    }
    {
        const char* argv[] = {"drampool", "--help"};
        DramPoolDaemon daemon;
        EXPECT_EQ(daemon.Run(2, const_cast<char**>(argv)), 1);
    }
    {
        const char* argv[] = {"drampool", "--config", "legacy.conf"};
        DramPoolDaemon daemon;
        EXPECT_EQ(daemon.Run(3, const_cast<char**>(argv)), 1);
    }
}

#if defined(UCM_DRAMPOOL_RUNTIME_INTEGRATION_TESTS)
TEST(DramPoolDaemonTest, RunsUntilSigint)
{
    const char* argv[] = {
        "drampool", "--addr", "127.0.0.1:9000", "--nics", "mlx5_0", "--pool-size-gb",
        "1",        "--kvcache-block-sizes", "4096", "8192", "--ttl-minutes", "120",
    };

    auto result = std::async(std::launch::async, [&argv]() {
        DramPoolDaemon daemon;
        return daemon.Run(12, const_cast<char**>(argv));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::raise(SIGINT);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(result.get(), 0);
}
#endif

}  // namespace UC::DRAMPOOL
