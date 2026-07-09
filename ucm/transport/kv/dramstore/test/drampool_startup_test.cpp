#include "drampool_config.h"
#include "drampool_daemon.h"
#include "drampool_server.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>
#include <gtest/gtest.h>

namespace UC::DRAMPOOL {
namespace {

DramPoolConfig MakeValidConfig()
{
    DramPoolConfig config;
    config.serverId = "drampool-test";
    config.listenAddr = "127.0.0.1:19000";
    config.transportMode = "tcp";
    config.poolSizeGb = 1;
    config.poolBlockSizes = {4096, 8192};
    config.poolBlockProportions = {70, 30};
    config.metadataShards = 4;
    config.requestQueueDepth = 128;
    config.handleQueueDepth = 128;
    config.pollerDrainBudget = 64;
    config.pollerScanBudget = 64;
    config.pollerMaxPending = 1024;
    config.gcEnabled = true;
    config.gcIntervalMs = 10;
    config.opTimeoutMs = 5000;
    config.shutdownTimeoutMs = 30000;
    config.logLevel = "info";
    config.logDir = "./drampool-test-logs";
    return config;
}

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

std::string WriteConfigFile(const std::string& name, const std::string& content)
{
    std::ofstream out{name};
    out << content;
    return name;
}

std::string ValidConfigText()
{
    return "server.id = drampool-0\n"
           "listen.addr = 127.0.0.1:9000\n"
           "transport.mode = tcp\n"
           "pool.size_gb = 1\n"
           "pool.block_sizes = 4096,8192\n"
           "pool.block_proportions = 70,30\n"
           "metadata.shards = 8\n"
           "queue.request_depth = 256\n"
           "queue.handle_depth = 256\n"
           "poller.drain_budget = 64\n"
           "poller.scan_budget = 64\n"
           "poller.max_pending = 1024\n"
           "gc.enabled = true\n"
           "gc.interval_ms = 1000\n"
           "op.timeout_ms = 5000\n"
           "shutdown.timeout_ms = 30000\n"
           "log.level = INFO\n"
           "log.dir = ./logs\n";
}

}  // namespace

TEST(DramPoolConfigTest, ParsesLongShortAndEqualsConfigFlags)
{
    {
        const char* argv[] = {"drampool", "--config", "long.conf"};
        CommandLineOptions options;
        auto status = ParseCommandLine(3, const_cast<char**>(argv), options);
        ASSERT_TRUE(status.Success()) << status.ToString();
        EXPECT_EQ(options.configPath, "long.conf");
        EXPECT_FALSE(options.showHelp);
    }
    {
        const char* argv[] = {"drampool", "-c", "short.conf"};
        CommandLineOptions options;
        auto status = ParseCommandLine(3, const_cast<char**>(argv), options);
        ASSERT_TRUE(status.Success()) << status.ToString();
        EXPECT_EQ(options.configPath, "short.conf");
    }
    {
        const char* argv[] = {"drampool", "--config=equals.conf"};
        CommandLineOptions options;
        auto status = ParseCommandLine(2, const_cast<char**>(argv), options);
        ASSERT_TRUE(status.Success()) << status.ToString();
        EXPECT_EQ(options.configPath, "equals.conf");
    }
}

TEST(DramPoolConfigTest, HandlesHelpAndInvalidCommandLines)
{
    {
        const char* argv[] = {"drampool", "--help"};
        CommandLineOptions options;
        auto status = ParseCommandLine(2, const_cast<char**>(argv), options);
        ASSERT_TRUE(status.Success()) << status.ToString();
        EXPECT_TRUE(options.showHelp);
    }
    {
        const char* argv[] = {"drampool"};
        CommandLineOptions options;
        EXPECT_TRUE(ParseCommandLine(1, const_cast<char**>(argv), options).Failure());
    }
    {
        const char* argv[] = {"drampool", "--config"};
        CommandLineOptions options;
        EXPECT_TRUE(ParseCommandLine(2, const_cast<char**>(argv), options).Failure());
    }
    {
        const char* argv[] = {"drampool", "--unknown"};
        CommandLineOptions options;
        EXPECT_TRUE(ParseCommandLine(2, const_cast<char**>(argv), options).Failure());
    }
}

TEST(DramPoolConfigTest, LoadsKeyValueConfigAndPreservesExtraKeys)
{
    const std::string path = WriteConfigFile("drampool_startup_valid.conf",
                                            ValidConfigText() + "future.option = value\n");

    auto loaded = LoadDramPoolConfig(path);
    std::remove(path.c_str());

    ASSERT_TRUE(loaded.HasValue()) << loaded.Error().ToString();
    const auto& config = loaded.Value();
    EXPECT_EQ(config.serverId, "drampool-0");
    EXPECT_EQ(config.poolBlockSizes.size(), 2U);
    EXPECT_EQ(config.poolBlockProportions[1], 30U);
    EXPECT_EQ(config.metadataShards, 8U);
    EXPECT_EQ(config.logLevel, "info");
    ASSERT_EQ(config.extra.count("future.option"), 1U);
    EXPECT_EQ(config.extra.at("future.option"), "value");
}

TEST(DramPoolConfigTest, LoadsDefaultsAndBoolVariants)
{
    const std::string path = WriteConfigFile("drampool_startup_defaults.conf",
                                            "pool.size_gb = 1\n"
                                            "gc.enabled = off\n"
                                            "log.level = WARN\n");
    auto loaded = LoadDramPoolConfig(path);
    std::remove(path.c_str());

    ASSERT_TRUE(loaded.HasValue()) << loaded.Error().ToString();
    EXPECT_EQ(loaded.Value().serverId, "drampool-0");
    EXPECT_FALSE(loaded.Value().gcEnabled);
    EXPECT_EQ(loaded.Value().gcIntervalMs, 1000U);
    EXPECT_EQ(loaded.Value().logLevel, "warn");
}

TEST(DramPoolConfigTest, RejectsMalformedConfigFiles)
{
    {
        const std::string path = WriteConfigFile("drampool_startup_missing_equal.conf", "bad line\n");
        auto loaded = LoadDramPoolConfig(path);
        std::remove(path.c_str());
        EXPECT_FALSE(loaded.HasValue());
    }
    {
        const std::string path = WriteConfigFile("drampool_startup_empty_key.conf", " = value\n");
        auto loaded = LoadDramPoolConfig(path);
        std::remove(path.c_str());
        EXPECT_FALSE(loaded.HasValue());
    }
    {
        const std::string path =
            WriteConfigFile("drampool_startup_bad_number.conf", "pool.size_gb = nope\n");
        auto loaded = LoadDramPoolConfig(path);
        std::remove(path.c_str());
        EXPECT_FALSE(loaded.HasValue());
    }
    {
        const std::string path =
            WriteConfigFile("drampool_startup_bad_bool.conf", "pool.size_gb = 1\ngc.enabled = maybe\n");
        auto loaded = LoadDramPoolConfig(path);
        std::remove(path.c_str());
        EXPECT_FALSE(loaded.HasValue());
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
    expectInvalid([](DramPoolConfig& config) { config.listenAddr.clear(); });
    expectInvalid([](DramPoolConfig& config) { config.transportMode = "udp"; });
    expectInvalid([](DramPoolConfig& config) { config.poolSizeGb = 0; });
    expectInvalid([](DramPoolConfig& config) { config.poolBlockSizes.clear(); });
    expectInvalid([](DramPoolConfig& config) { config.poolBlockSizes[0] = 0; });
    expectInvalid([](DramPoolConfig& config) { config.poolBlockProportions.clear(); });
    expectInvalid([](DramPoolConfig& config) { config.poolBlockProportions[0] = 0; });
    expectInvalid([](DramPoolConfig& config) { config.poolBlockProportions = {100}; });
    expectInvalid([](DramPoolConfig& config) { config.metadataShards = 0; });
    expectInvalid([](DramPoolConfig& config) { config.requestQueueDepth = 1; });
    expectInvalid([](DramPoolConfig& config) { config.handleQueueDepth = 1; });
    expectInvalid([](DramPoolConfig& config) { config.pollerDrainBudget = 0; });
    expectInvalid([](DramPoolConfig& config) { config.pollerScanBudget = 0; });
    expectInvalid([](DramPoolConfig& config) { config.pollerMaxPending = 0; });
    expectInvalid([](DramPoolConfig& config) { config.pollerMaxPending = 32; });
    expectInvalid([](DramPoolConfig& config) { config.gcIntervalMs = 0; });
    expectInvalid([](DramPoolConfig& config) { config.opTimeoutMs = 0; });
    expectInvalid([](DramPoolConfig& config) { config.shutdownTimeoutMs = 0; });
    expectInvalid([](DramPoolConfig& config) { config.logLevel = "verbose"; });
    expectInvalid([](DramPoolConfig& config) { config.logDir.clear(); });
}

TEST(DramPoolConfigTest, AcceptsGcDisabledWithZeroIntervalAndSupportedTransports)
{
    for (const auto& mode : {"tcp", "rdma", "hixl"}) {
        auto config = MakeValidConfig();
        config.transportMode = mode;
        config.gcEnabled = false;
        config.gcIntervalMs = 0;
        auto status = ValidateDramPoolConfig(config);
        EXPECT_TRUE(status.Success()) << status.ToString();
    }
}

TEST(DramPoolServerTest, RejectsInvalidLifecycleCalls)
{
    DramPoolServer server;
    EXPECT_TRUE(server.Start().Failure());

    auto invalidConfig = MakeValidConfig();
    invalidConfig.poolSizeGb = 0;
    EXPECT_TRUE(server.Init(invalidConfig).Failure());

    auto status = server.Init(MakeValidConfig());
    ASSERT_TRUE(status.Success()) << status.ToString();
    EXPECT_TRUE(server.Init(MakeValidConfig()).Failure());
    EXPECT_TRUE(server.Start().Success());
    EXPECT_TRUE(server.Start().Failure());
    server.Stop();
    EXPECT_TRUE(server.Start().Failure());
}

TEST(DramPoolServerTest, StartsReceiverLastAndStopsReceiverFirst)
{
    DramPoolServer server;
    auto status = server.Init(MakeValidConfig());
    ASSERT_TRUE(status.Success()) << status.ToString();

    status = server.Start();
    ASSERT_TRUE(status.Success()) << status.ToString();
    EXPECT_TRUE(server.IsServiceReady());

    server.Stop();
    EXPECT_FALSE(server.IsServiceReady());

    const auto events = server.LifecycleEvents();
    const std::vector<std::string> startupOrder = {
        "InitDataTransportManager",
        "InstallDataTransport",
        "InitBufferMgr",
        "RegisterBufferMemory",
        "InitMetadataIndex",
        "InitProtocol",
        "InitQueues",
        "StartCompletionPoller",
        "StartTaskWorker",
        "StartGCThread",
        "StartRequestChannelAndReceiver",
        "SetServiceReady(true)",
    };
    EXPECT_TRUE(ContainsInOrder(events, startupOrder));

    const std::vector<std::string> shutdownOrder = {
        "SetServiceReady(false)",
        "StopReceiver",
        "StopTaskWorker",
        "CancelInflightTransports",
        "StopCompletionPoller",
        "StopGCThread",
        "UnregisterBufferMemory",
        "DestroyMetadataIndex",
    };
    EXPECT_TRUE(ContainsInOrder(events, shutdownOrder));
}

TEST(DramPoolServerTest, GcDisabledSkipsGcThreadLifecycleEvents)
{
    auto config = MakeValidConfig();
    config.gcEnabled = false;
    config.gcIntervalMs = 0;

    DramPoolServer server;
    auto status = server.Init(config);
    ASSERT_TRUE(status.Success()) << status.ToString();
    ASSERT_TRUE(server.Start().Success());
    server.Stop();

    const auto events = server.LifecycleEvents();
    EXPECT_EQ(std::find(events.begin(), events.end(), "StartGCThread"), events.end());
    EXPECT_EQ(std::find(events.begin(), events.end(), "StopGCThread"), events.end());
    EXPECT_TRUE(ContainsInOrder(events, {"StartCompletionPoller", "StartTaskWorker",
                                         "StartRequestChannelAndReceiver",
                                         "SetServiceReady(true)"}));
}

TEST(DramPoolDaemonTest, ReturnsForHelpAndInvalidConfig)
{
    {
        const char* argv[] = {"drampool", "--help"};
        DramPoolDaemon daemon;
        EXPECT_EQ(daemon.Run(2, const_cast<char**>(argv)), 0);
    }
    {
        const char* argv[] = {"drampool"};
        DramPoolDaemon daemon;
        EXPECT_EQ(daemon.Run(1, const_cast<char**>(argv)), 1);
    }
    {
        const char* argv[] = {"drampool", "--config", "missing-drampool.conf"};
        DramPoolDaemon daemon;
        EXPECT_EQ(daemon.Run(3, const_cast<char**>(argv)), 1);
    }
}

TEST(DramPoolDaemonTest, RunsUntilSigint)
{
    const std::string path = WriteConfigFile("drampool_daemon_valid.conf",
                                            ValidConfigText() + "gc.interval_ms = 10\n");
    const char* argv[] = {"drampool", "--config", "drampool_daemon_valid.conf"};

    auto result = std::async(std::launch::async, [&argv]() {
        DramPoolDaemon daemon;
        return daemon.Run(3, const_cast<char**>(argv));
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::raise(SIGINT);

    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_EQ(result.get(), 0);
    std::remove(path.c_str());
}

}  // namespace UC::DRAMPOOL
