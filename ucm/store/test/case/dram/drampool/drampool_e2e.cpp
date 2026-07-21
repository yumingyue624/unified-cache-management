/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 * */
#include <acl/acl.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "core/transport_manager.h"
#include "drampool_config.h"
#include "drampool_server.h"
#include "kv_protocol.h"
#include "two_sided/tcp/tcp_message_channel.h"

namespace UC::DramPool {
namespace {

constexpr std::size_t kDataSize = 4096;
constexpr std::size_t kResponseSize = 64;
constexpr auto kWaitInterval = std::chrono::milliseconds(10);
constexpr auto kWaitTimeout = std::chrono::seconds(30);
std::atomic_bool g_stop{false};

struct E2eConfig {
    transport::Endpoint serverControl{"127.0.0.1", 19000};
    transport::Endpoint clientControl{"127.0.0.1", 19001};
    transport::ManagerID serverOneSided{"127.0.0.1:19100"};
    transport::ManagerID clientOneSided{"127.0.0.1:19101"};
    std::int32_t deviceId{0};
};

void HandleSignal(int) { g_stop.store(true, std::memory_order_release); }

bool ParseArgs(int argc, char** argv, E2eConfig& config)
{
    if (argc != 7) {
        std::cerr << "usage: drampool_e2e <server|client> <server-control> <client-control> "
                     "<server-one-sided> <client-one-sided> <device-id>\n";
        return false;
    }
    if (ParseDramPoolEndpoint("server-control", argv[2], config.serverControl).Failure() ||
        ParseDramPoolEndpoint("client-control", argv[3], config.clientControl).Failure()) {
        return false;
    }
    config.serverOneSided = argv[4];
    config.clientOneSided = argv[5];
    try {
        config.deviceId = std::stoi(argv[6]);
    } catch (const std::exception&) {
        return false;
    }
    return config.deviceId >= 0;
}

bool InitAcl(std::int32_t deviceId, bool& ownsRuntime)
{
    const auto initStatus = aclInit(nullptr);
    ownsRuntime = initStatus == ACL_SUCCESS;
    if (!ownsRuntime && initStatus != ACL_ERROR_REPEAT_INITIALIZE) {
        std::cerr << "aclInit failed: " << initStatus << '\n';
        return false;
    }
    const auto status = aclrtSetDevice(deviceId);
    if (status != ACL_SUCCESS) { std::cerr << "aclrtSetDevice failed: " << status << '\n'; }
    return status == ACL_SUCCESS;
}

void FinalizeAcl(std::int32_t deviceId, bool ownsRuntime)
{
    (void)aclrtResetDevice(deviceId);
    if (ownsRuntime) { (void)aclFinalize(); }
}

int RunServer(const E2eConfig& config)
{
    g_config = DramPoolConfig{};
    g_config.addr = config.serverControl;
    g_config.nics = {"mlx5_0"};
    g_config.poolBlockSizes = {kDataSize};
    g_config.poolBlockProportions = {1};
    // A successful DUMP occupies the only data slot. Responses must still use the
    // dedicated response pool, otherwise the client would wait forever here.
    g_config.poolSlotCounts = {1};
    g_config.transportDeviceId = config.deviceId;
    g_config.twoSidedToOneSided = {
        {config.serverControl.ToString(), config.serverOneSided},
        {config.clientControl.ToString(), config.clientOneSided},
    };
    g_config.requestQueueDepth = 16;
    g_config.completionQueueDepth = 16;
    g_config.pollerPendingDepth = 4;
    g_config.flagBufferCapacityMb = 1;
    g_config.flagBufferSlotSizeBytes = kResponseSize;
    g_config.flagBufferSlotCount = g_config.pollerPendingDepth;
    g_config.gcEnabled = false;
    g_config.opTimeoutMs = 10'000;

    DramPoolServer server;
    auto status = server.Init();
    if (status.Success()) { status = server.Start(); }
    if (status.Failure()) {
        std::cerr << "DramPool E2E server start failed: " << status.ToString() << '\n';
        return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);
    std::cout << "DRAMPOOL_E2E_SERVER_READY" << std::endl;
    while (!g_stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.Stop();
    return 0;
}

class DramStoreSimulator {
public:
    explicit DramStoreSimulator(E2eConfig config)
        : config_(std::move(config)), manager_(config_.clientOneSided)
    {
    }

    ~DramStoreSimulator() { Reset(); }

    bool Init()
    {
        if (!InitAcl(config_.deviceId, ownsAclRuntime_)) { return false; }
        aclInitialized_ = true;

        transport::HixlInitAttrs attrs;
        transport::Endpoint localEndpoint;
        if (ParseDramPoolEndpoint("client-one-sided", config_.clientOneSided, localEndpoint)
                .Failure()) {
            return false;
        }
        attrs.ip = localEndpoint.host;
        transport::HixlInitAttrs::Instance instance;
        instance.port = -1;
        instance.device_id = config_.deviceId;
        attrs.instances.push_back(instance);
        attrs.connect_timeout_ms = 10'000;
        attrs.transfer_timeout_ms = 10'000;
        if (manager_.InstallTransport(transport::TransportProtocol::Hixl, attrs) !=
            transport::Status::Ok) {
            return false;
        }

        if (aclrtMalloc(&deviceData_, kDataSize, ACL_MEM_MALLOC_HUGE_ONLY) != ACL_SUCCESS ||
            aclrtMallocHost(&response_, kResponseSize) != ACL_SUCCESS) {
            return false;
        }
        transport::MemoryRegion deviceRegion;
        deviceRegion.addr = deviceData_;
        deviceRegion.length = kDataSize;
        deviceRegion.type = transport::MemoryType::Device;
        deviceRegion.device_id = config_.deviceId;
        transport::MemoryRegion responseRegion;
        responseRegion.addr = response_;
        responseRegion.length = kResponseSize;
        responseRegion.type = transport::MemoryType::Host;
        if (manager_.RegisterMemory(deviceRegion, deviceHandle_) != transport::Status::Ok ||
            manager_.RegisterMemory(responseRegion, responseHandle_) != transport::Status::Ok ||
            manager_.Init() != transport::Status::Ok ||
            control_.Init(config_.clientControl) != transport::Status::Ok) {
            return false;
        }
        return ExchangeMetadata();
    }

    bool Run()
    {
        std::vector<std::uint8_t> expected(kDataSize);
        for (std::size_t index = 0; index < expected.size(); ++index) {
            expected[index] = static_cast<std::uint8_t>((index * 17U + 3U) & 0xFFU);
        }
        if (aclrtMemcpy(deviceData_, kDataSize, expected.data(), expected.size(),
                        ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
            return false;
        }

        BlockId key{};
        key.back() = std::byte{0x5A};
        KvDumpRequest dump;
        dump.opcode = KvOpcode::Dump;
        dump.resp_addr = reinterpret_cast<std::uint64_t>(response_);
        dump.ttl = 60'000;
        dump.batch_size = 1;
        dump.entries.push_back(KvDumpEntry{
            key, reinterpret_cast<std::uint64_t>(deviceData_), kDataSize, 7
        });
        if (!SendAndWait(dump, {0})) { return false; }

        BlockId missing{};
        missing.back() = std::byte{0x6B};
        KvLookupRequest lookup;
        lookup.opcode = KvOpcode::Lookup;
        lookup.resp_addr = reinterpret_cast<std::uint64_t>(response_);
        lookup.batch_size = 2;
        lookup.entries.push_back(KvLookupEntry{key});
        lookup.entries.push_back(KvLookupEntry{missing});
        if (!SendAndWait(lookup, {1, 0})) { return false; }

        if (aclrtMemset(deviceData_, kDataSize, 0, kDataSize) != ACL_SUCCESS) { return false; }
        KvLoadRequest load;
        load.opcode = KvOpcode::Load;
        load.resp_addr = reinterpret_cast<std::uint64_t>(response_);
        load.batch_size = 1;
        load.entries.push_back(KvLoadEntry{
            key, reinterpret_cast<std::uint64_t>(deviceData_), kDataSize, 7
        });
        if (!SendAndWait(load, {0})) { return false; }

        std::vector<std::uint8_t> actual(kDataSize);
        if (aclrtMemcpy(actual.data(), actual.size(), deviceData_, kDataSize,
                        ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS ||
            actual != expected) {
            std::cerr << "LOAD data does not match the preceding DUMP" << std::endl;
            return false;
        }
        std::cout << "DRAMPOOL_E2E_PASS" << std::endl;
        return true;
    }

private:
    bool ExchangeMetadata()
    {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (manager_.ExchangeMetadata(config_.serverOneSided) == transport::Status::Ok) {
                return true;
            }
            std::this_thread::sleep_for(kWaitInterval);
        }
        std::cerr << "metadata exchange timed out" << std::endl;
        return false;
    }

    bool SendAndWait(KvRequest& request, const std::vector<std::uint8_t>& expected)
    {
        const auto packedSize = protocols_.GetPackedRequestSize(request.opcode, request);
        std::vector<std::uint8_t> packed(packedSize);
        if (packedSize == 0 || protocols_.PackRequest(packed.data(), request.opcode, request)
                                   .Failure()) {
            return false;
        }
        std::memset(response_, 0, kResponseSize);
        if (control_.Send(config_.serverControl, packed.data(), packed.size()) !=
            transport::Status::Ok) {
            return false;
        }

        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            KvResponse response;
            const auto status = protocols_.UnpackResponse(
                response_, request.opcode, static_cast<std::uint16_t>(expected.size()), response);
            if (status.Success()) {
                if (response.results != expected) {
                    std::cerr << "unexpected response for opcode "
                              << static_cast<int>(request.opcode) << std::endl;
                    return false;
                }
                return true;
            }
            if (status != Status::Retry()) {
                std::cerr << "response unpack failed: " << status.ToString() << std::endl;
                return false;
            }
            std::this_thread::sleep_for(kWaitInterval);
        }
        std::cerr << "response timed out for opcode " << static_cast<int>(request.opcode)
                  << std::endl;
        return false;
    }

    void Reset()
    {
        (void)control_.Shutdown();
        if (responseHandle_ != transport::kInvalidMemoryHandle) {
            (void)manager_.UnregisterMemory(responseHandle_);
            responseHandle_ = transport::kInvalidMemoryHandle;
        }
        if (deviceHandle_ != transport::kInvalidMemoryHandle) {
            (void)manager_.UnregisterMemory(deviceHandle_);
            deviceHandle_ = transport::kInvalidMemoryHandle;
        }
        (void)manager_.Shutdown();
        if (response_ != nullptr) {
            (void)aclrtFreeHost(response_);
            response_ = nullptr;
        }
        if (deviceData_ != nullptr) {
            (void)aclrtFree(deviceData_);
            deviceData_ = nullptr;
        }
        if (aclInitialized_) { FinalizeAcl(config_.deviceId, ownsAclRuntime_); }
        aclInitialized_ = false;
        ownsAclRuntime_ = false;
    }

    E2eConfig config_;
    transport::TransportManager manager_;
    transport::TcpMessageChannel control_;
    ProtocolManager protocols_;
    void* deviceData_{nullptr};
    void* response_{nullptr};
    transport::MemoryHandle deviceHandle_{transport::kInvalidMemoryHandle};
    transport::MemoryHandle responseHandle_{transport::kInvalidMemoryHandle};
    bool aclInitialized_{false};
    bool ownsAclRuntime_{false};
};

}  // namespace
}  // namespace UC::DramPool

int main(int argc, char** argv)
{
    UC::DramPool::E2eConfig config;
    if (!UC::DramPool::ParseArgs(argc, argv, config)) { return 2; }
    if (std::string{argv[1]} == "server") { return UC::DramPool::RunServer(config); }
    if (std::string{argv[1]} != "client") { return 2; }
    UC::DramPool::DramStoreSimulator client{config};
    return client.Init() && client.Run() ? 0 : 1;
}
