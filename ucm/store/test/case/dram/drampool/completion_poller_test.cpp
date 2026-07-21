#include <acl/acl.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include "core/transport_init_attrs.h"
#include "core/transport_manager.h"

namespace {

constexpr std::size_t kValueSize = 64;
constexpr std::size_t kRegionSize = 64 * 1024;
constexpr std::size_t kResponseStride = 64;

transport::HixlInitAttrs MakeHixlAttrs(const std::string& host, int deviceId)
{
    transport::HixlInitAttrs attrs;
    attrs.ip = host;
    transport::HixlInitAttrs::Instance instance;
    instance.port = -1;
    instance.device_id = deviceId;
    attrs.instances.push_back(instance);
    attrs.connect_timeout_ms = 30'000;
    attrs.transfer_timeout_ms = 30'000;
    return attrs;
}

std::string EncodeHex(const std::uint8_t* data, std::size_t length)
{
    constexpr char kDigits[] = "0123456789abcdef";
    std::string encoded(length * 2, '0');
    for (std::size_t index = 0; index < length; ++index) {
        encoded[index * 2] = kDigits[data[index] >> 4U];
        encoded[index * 2 + 1] = kDigits[data[index] & 0x0FU];
    }
    return encoded;
}

int HexValue(char value)
{
    if (value >= '0' && value <= '9') { return value - '0'; }
    if (value >= 'a' && value <= 'f') { return value - 'a' + 10; }
    if (value >= 'A' && value <= 'F') { return value - 'A' + 10; }
    return -1;
}

bool DecodeHex(const std::string& encoded, std::vector<std::uint8_t>& data)
{
    if ((encoded.size() & 1U) != 0) { return false; }
    data.resize(encoded.size() / 2);
    for (std::size_t index = 0; index < data.size(); ++index) {
        const auto high = HexValue(encoded[index * 2]);
        const auto low = HexValue(encoded[index * 2 + 1]);
        if (high < 0 || low < 0) { return false; }
        data[index] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return true;
}

}  // namespace

#ifdef DRAMPOOL_POLLER_PEER_MAIN

namespace {

int RunPeer(const std::string& managerId, const std::string& host, int deviceId)
{
    if (aclInit(nullptr) != ACL_SUCCESS || aclrtSetDevice(deviceId) != ACL_SUCCESS) { return 2; }

    void* deviceMemory = nullptr;
    void* responseMemory = nullptr;
    transport::TransportManager manager(managerId);
    transport::MemoryHandle handle = transport::kInvalidMemoryHandle;
    bool initialized = false;
    int result = 3;

    if (aclrtMalloc(&deviceMemory, kRegionSize, ACL_MEM_MALLOC_HUGE_ONLY) != ACL_SUCCESS ||
        aclrtMallocHost(&responseMemory, kRegionSize) != ACL_SUCCESS ||
        aclrtMemset(deviceMemory, kRegionSize, 0, kRegionSize) != ACL_SUCCESS) {
        goto cleanup;
    }
    std::memset(responseMemory, 0, kRegionSize);

    if (manager.InstallTransport(transport::TransportProtocol::Hixl,
                                 MakeHixlAttrs(host, deviceId)) != transport::Status::Ok) {
        goto cleanup;
    }
    {
        transport::MemoryRegion region;
        region.addr = deviceMemory;
        region.length = kRegionSize;
        region.type = transport::MemoryType::Device;
        region.device_id = deviceId;
        if (manager.RegisterMemory(region, handle) != transport::Status::Ok) { goto cleanup; }
    }
    {
        const transport::MemoryRegion region{responseMemory, kRegionSize,
                                             transport::MemoryType::Host};
        if (manager.RegisterMemory(region, handle) != transport::Status::Ok) { goto cleanup; }
    }
    if (manager.Init() != transport::Status::Ok) { goto cleanup; }
    initialized = true;

    std::cout << "READY " << reinterpret_cast<std::uintptr_t>(deviceMemory) << ' '
              << reinterpret_cast<std::uintptr_t>(responseMemory) << std::endl;

    for (std::string line; std::getline(std::cin, line);) {
        std::istringstream command(line);
        std::string opcode;
        std::size_t offset = 0;
        std::size_t length = 0;
        command >> opcode;
        if (opcode == "STOP") {
            result = 0;
            std::cout << "BYE" << std::endl;
            break;
        }
        if (opcode == "CLEAR_RESP") {
            std::memset(responseMemory, 0, kRegionSize);
            std::cout << "OK" << std::endl;
            continue;
        }
        if (opcode == "WRITE_DEVICE") {
            std::string encoded;
            command >> offset >> encoded;
            std::vector<std::uint8_t> bytes;
            const bool valid = DecodeHex(encoded, bytes) && offset <= kRegionSize &&
                               bytes.size() <= kRegionSize - offset;
            const bool copied = valid &&
                                aclrtMemcpy(static_cast<std::uint8_t*>(deviceMemory) + offset,
                                            kRegionSize - offset, bytes.data(), bytes.size(),
                                            ACL_MEMCPY_HOST_TO_DEVICE) == ACL_SUCCESS;
            std::cout << (copied ? "OK" : "ERROR") << std::endl;
            continue;
        }
        if (opcode == "READ_DEVICE" || opcode == "READ_RESP") {
            command >> offset >> length;
            if (offset > kRegionSize || length > kRegionSize - offset) {
                std::cout << "ERROR" << std::endl;
                continue;
            }
            std::vector<std::uint8_t> bytes(length);
            bool ok = true;
            if (opcode == "READ_DEVICE") {
                ok = aclrtMemcpy(bytes.data(), bytes.size(),
                                 static_cast<std::uint8_t*>(deviceMemory) + offset, length,
                                 ACL_MEMCPY_DEVICE_TO_HOST) == ACL_SUCCESS;
            } else {
                std::memcpy(bytes.data(), static_cast<std::uint8_t*>(responseMemory) + offset,
                            length);
            }
            std::cout << (ok ? "DATA " + EncodeHex(bytes.data(), bytes.size()) : "ERROR")
                      << std::endl;
            continue;
        }
        std::cout << "ERROR" << std::endl;
    }

cleanup:
    if (initialized) { (void)manager.Shutdown(); }
    if (responseMemory != nullptr) { (void)aclrtFreeHost(responseMemory); }
    if (deviceMemory != nullptr) { (void)aclrtFree(deviceMemory); }
    (void)aclrtResetDevice(deviceId);
    (void)aclFinalize();
    return result;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 4) { return 1; }
    try {
        return RunPeer(argv[1], argv[2], std::stoi(argv[3]));
    } catch (const std::exception&) {
        return 1;
    }
}

#else

#include "dram/cc/drampool/completion_poller.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <gtest/gtest.h>
#include <memory>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include "dram/cc/drampool/buffer_manager.h"
#include "dram/cc/drampool/drampool_config.h"
#include "dram/cc/drampool/metadata.h"

namespace UC::DramPool {
namespace {

constexpr std::size_t kPoolSlotCount = 128;
constexpr auto kWaitInterval = std::chrono::milliseconds(1);
constexpr auto kWaitTimeout = std::chrono::seconds(30);

int EnvInt(const char* name, int fallback)
{
    const auto* value = std::getenv(name);
    if (value == nullptr || *value == '\0') { return fallback; }
    char* end = nullptr;
    const auto parsed = std::strtol(value, &end, 10);
    return end != nullptr && *end == '\0' ? static_cast<int>(parsed) : fallback;
}

std::string EnvText(const char* name, const char* fallback)
{
    const auto* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? fallback : value;
}

bool WaitUntil(const std::function<bool()>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) { return true; }
        std::this_thread::sleep_for(kWaitInterval);
    }
    return predicate();
}

BlockId MakeKey(std::uint8_t suffix)
{
    BlockId key{};
    key.back() = static_cast<std::byte>(suffix);
    return key;
}

MetadataConfig MakeMetadataConfig()
{
    return {EvictionPolicyType::TTL, EvictionPolicyType::POSITION,
            std::chrono::milliseconds(100), 0.0};
}

class PeerProcess final {
public:
    ~PeerProcess() { Stop(); }

    bool Start(const std::string& managerId, const std::string& host, int deviceId)
    {
        int commands[2] = {-1, -1};
        int responses[2] = {-1, -1};
        if (pipe(commands) != 0 || pipe(responses) != 0) { return false; }

        char executable[4096]{};
        const auto executableLength = readlink("/proc/self/exe", executable,
                                               sizeof(executable) - 1);
        if (executableLength <= 0) { return false; }
        executable[executableLength] = '\0';
        std::string helperPath(executable);
        const auto slash = helperPath.find_last_of('/');
        helperPath.replace(slash + 1, std::string::npos, "completion_poller_peer");

        pid_ = fork();
        if (pid_ < 0) { return false; }
        if (pid_ == 0) {
            (void)dup2(commands[0], STDIN_FILENO);
            (void)dup2(responses[1], STDOUT_FILENO);
            close(commands[0]);
            close(commands[1]);
            close(responses[0]);
            close(responses[1]);
            const auto device = std::to_string(deviceId);
            execl(helperPath.c_str(), helperPath.c_str(), managerId.c_str(), host.c_str(),
                  device.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        close(commands[0]);
        close(responses[1]);
        commandStream_ = fdopen(commands[1], "w");
        responseStream_ = fdopen(responses[0], "r");
        if (commandStream_ == nullptr || responseStream_ == nullptr) { return false; }
        setvbuf(commandStream_, nullptr, _IOLBF, 0);

        std::string ready;
        if (!ReadProtocolLine(ready) || ready.rfind("READY ", 0) != 0) { return false; }
        std::istringstream values(ready.substr(6));
        values >> deviceAddress_ >> responseAddress_;
        return values && deviceAddress_ != 0 && responseAddress_ != 0;
    }

    void Stop()
    {
        if (commandStream_ != nullptr && responseStream_ != nullptr) {
            std::string ignored;
            (void)Command("STOP", ignored);
        }
        if (commandStream_ != nullptr) {
            fclose(commandStream_);
            commandStream_ = nullptr;
        }
        if (responseStream_ != nullptr) {
            fclose(responseStream_);
            responseStream_ = nullptr;
        }
        if (pid_ > 0) {
            int status = 0;
            (void)waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    bool ClearResponses()
    {
        std::string response;
        return Command("CLEAR_RESP", response) && response == "OK";
    }

    bool WriteDevice(std::size_t offset, const std::vector<std::uint8_t>& data)
    {
        std::string response;
        return Command("WRITE_DEVICE " + std::to_string(offset) + " " +
                           EncodeHex(data.data(), data.size()),
                       response) &&
               response == "OK";
    }

    bool ReadDevice(std::size_t offset, std::size_t length, std::vector<std::uint8_t>& data)
    {
        return ReadBytes("READ_DEVICE", offset, length, data);
    }

    bool ReadResponse(std::size_t offset, std::size_t length, std::vector<std::uint8_t>& data)
    {
        return ReadBytes("READ_RESP", offset, length, data);
    }

    std::uint64_t DeviceAddress(std::size_t offset) const { return deviceAddress_ + offset; }
    std::uint64_t ResponseAddress(std::size_t offset) const { return responseAddress_ + offset; }

private:
    bool ReadBytes(const char* opcode, std::size_t offset, std::size_t length,
                   std::vector<std::uint8_t>& data)
    {
        std::string response;
        if (!Command(std::string(opcode) + " " + std::to_string(offset) + " " +
                         std::to_string(length),
                     response) ||
            response.rfind("DATA ", 0) != 0) {
            return false;
        }
        return DecodeHex(response.substr(5), data) && data.size() == length;
    }

    bool Command(const std::string& command, std::string& response)
    {
        if (commandStream_ == nullptr ||
            std::fprintf(commandStream_, "%s\n", command.c_str()) < 0 ||
            std::fflush(commandStream_) != 0) {
            return false;
        }
        return ReadProtocolLine(response);
    }

    bool ReadProtocolLine(std::string& line)
    {
        const auto deadline = std::chrono::steady_clock::now() + kWaitTimeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            pollfd descriptor{fileno(responseStream_), POLLIN, 0};
            int ready = 0;
            do {
                ready = poll(&descriptor, 1, static_cast<int>(remaining.count()));
            } while (ready < 0 && errno == EINTR);
            if (ready <= 0 || (descriptor.revents & (POLLIN | POLLHUP)) == 0) { return false; }
            char* buffer = nullptr;
            std::size_t capacity = 0;
            const auto length = ::getline(&buffer, &capacity, responseStream_);
            if (length <= 0) {
                std::free(buffer);
                return false;
            }
            line.assign(buffer, static_cast<std::size_t>(length));
            std::free(buffer);
            if (!line.empty() && line.back() == '\n') { line.pop_back(); }
            if (line == "OK" || line == "ERROR" || line == "BYE" ||
                line.rfind("READY ", 0) == 0 || line.rfind("DATA ", 0) == 0) {
                return true;
            }
        }
        return false;
    }

    pid_t pid_{-1};
    FILE* commandStream_{nullptr};
    FILE* responseStream_{nullptr};
    std::uint64_t deviceAddress_{0};
    std::uint64_t responseAddress_{0};
};

class PollerRunner final {
public:
    explicit PollerRunner(DramPoolRuntime& runtime)
        : poller_(runtime), thread_([this]() { poller_.Run(stop_); })
    {
    }
    ~PollerRunner() { Stop(); }
    void Stop()
    {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) { thread_.join(); }
    }

private:
    CompletionPoller poller_;
    std::atomic_bool stop_{false};
    std::thread thread_;
};

class CompletionPollerRealTransportTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        localDeviceId_ = EnvInt("DRAMPOOL_POLLER_TEST_LOCAL_DEVICE", 4);
        peerDeviceId_ = EnvInt("DRAMPOOL_POLLER_TEST_PEER_DEVICE", 5);
        host_ = EnvText("DRAMPOOL_POLLER_TEST_HOST", "127.0.0.1");
        const auto portBase = EnvInt("DRAMPOOL_POLLER_TEST_PORT_BASE", 53100);
        localManagerId_ = host_ + ":" + std::to_string(portBase);
        peerManagerId_ = host_ + ":" + std::to_string(portBase + 1);

        ASSERT_TRUE(peer_.Start(peerManagerId_, host_, peerDeviceId_));
        const auto aclStatus = aclInit(nullptr);
        if (aclStatus == ACL_SUCCESS) {
            ownsAclRuntime_ = true;
        } else {
            ASSERT_EQ(aclStatus, ACL_ERROR_REPEAT_INITIALIZE);
        }
        ASSERT_EQ(aclrtSetDevice(localDeviceId_), ACL_SUCCESS);

        bufferManager_ = std::make_unique<BufferManager>(
            std::vector<std::pair<std::size_t, std::size_t>>{{kValueSize, kPoolSlotCount}});
        metadata_ = std::make_unique<MetadataManager>(MakeMetadataConfig(), *bufferManager_);
        ASSERT_TRUE(flagBufferPool_.Init("completion_poller_real_transport_flag_pool",
                                         BufferPool::MemoryType::HOST, kResponseStride,
                                         kPoolSlotCount)
                        .Success());

        localManager_ = std::make_unique<transport::TransportManager>(localManagerId_);
        ASSERT_EQ(localManager_->InstallTransport(transport::TransportProtocol::Hixl,
                                                  MakeHixlAttrs(host_, localDeviceId_)),
                  transport::Status::Ok);
        transport::MemoryHandle handle = transport::kInvalidMemoryHandle;
        for (const auto& region : bufferManager_->MemoryRegions()) {
            ASSERT_EQ(localManager_->RegisterMemory(region, handle), transport::Status::Ok);
        }
        const transport::MemoryRegion flagRegion{flagBufferPool_.GetLocalAddr(),
                                                  flagBufferPool_.GetTotalSize(),
                                                  transport::MemoryType::Host};
        ASSERT_EQ(localManager_->RegisterMemory(flagRegion, handle), transport::Status::Ok);
        ASSERT_EQ(localManager_->Init(), transport::Status::Ok);
        ASSERT_TRUE(WaitUntil([]() {
            return localManager_->ExchangeMetadata(peerManagerId_) == transport::Status::Ok;
        }));
        ASSERT_EQ(localManager_->Connect(transport::TransportProtocol::Hixl, peerManagerId_),
                  transport::Status::Ok);
    }

    static void TearDownTestSuite()
    {
        if (localManager_) { EXPECT_EQ(localManager_->Shutdown(), transport::Status::Ok); }
        localManager_.reset();
        metadata_.reset();
        flagBufferPool_.Reset();
        bufferManager_.reset();
        (void)aclrtResetDevice(localDeviceId_);
        if (ownsAclRuntime_) {
            (void)aclFinalize();
            ownsAclRuntime_ = false;
        }
        peer_.Stop();
    }

    void SetUp() override
    {
        savedConfig_ = g_config;
        g_config.pollerPendingDepth = 2;
        g_config.opTimeoutMs = 30'000;
        requestQueue_.Setup(kPoolSlotCount);
        completionQueue_.Setup(kPoolSlotCount);
        ASSERT_TRUE(peer_.ClearResponses());
        runtime_ = std::make_unique<DramPoolRuntime>(*metadata_, flagBufferPool_, *localManager_,
                                                     protocols_, requestQueue_, completionQueue_);
    }

    void TearDown() override
    {
        runtime_.reset();
        g_config = std::move(savedConfig_);
    }

    std::uint64_t ResponseAddress(std::size_t index) const
    {
        return peer_.ResponseAddress(index * kResponseStride);
    }

    std::uint64_t DeviceAddress(std::size_t index) const
    {
        return peer_.DeviceAddress(index * kValueSize);
    }

    bool WaitForResponse(std::size_t index, KvOpcode opcode, std::uint16_t resultCount,
                         KvResponse& response)
    {
        const auto packedSize = protocols_.GetPackedResponseSize(opcode, resultCount);
        return WaitUntil([&, packedSize]() {
            std::vector<std::uint8_t> bytes;
            return peer_.ReadResponse(index * kResponseStride, packedSize, bytes) &&
                   protocols_.UnpackResponse(bytes.data(), opcode, resultCount, response).Success();
        });
    }

    CompletionRecord MakeDumpRecord(std::uint8_t keySuffix, std::size_t memoryIndex,
                                    std::size_t responseIndex,
                                    const std::vector<std::uint8_t>& value)
    {
        EXPECT_EQ(value.size(), kValueSize);
        EXPECT_TRUE(peer_.WriteDevice(memoryIndex * kValueSize, value));

        auto entry = std::make_shared<Entry>();
        entry->key = MakeKey(keySuffix);
        entry->size = kValueSize;
        entry->lifeTimeout = std::chrono::system_clock::now() + std::chrono::hours(1);
        EXPECT_TRUE(metadata_->StoreBegin(entry->key, entry).Success());

        transport::Operation operation;
        operation.opcode = transport::Opcode::Read;
        operation.direct = transport::OperationDirect::RemoteDeviceHost;
        operation.target_manager = peerManagerId_;
        operation.ops.emplace_back(
            transport::Segment{entry->buffer.addr, DeviceAddress(memoryIndex), kValueSize});
        transport::TransferHandle handle = transport::kInvalidTransferHandle;
        EXPECT_EQ(localManager_->ExecuteAsync(operation, handle), transport::Status::Ok);
        EXPECT_NE(handle, transport::kInvalidTransferHandle);

        CompletionRecord record;
        record.stage = CompletionStage::PollDataTransfer;
        record.opcode = KvOpcode::Dump;
        record.data_handle = handle;
        record.response_addr = ResponseAddress(responseIndex);
        record.peer_one_sided_id = peerManagerId_;
        record.results = {static_cast<std::uint8_t>(DumpLoadResult::Failed)};
        record.transfer_items = {TransferItem{0, entry->key}};
        record.submit_ms = SteadyNowMs();
        return record;
    }

    static inline int localDeviceId_{4};
    static inline int peerDeviceId_{5};
    static inline bool ownsAclRuntime_{false};
    static inline std::string host_;
    static inline std::string localManagerId_;
    static inline std::string peerManagerId_;
    static inline PeerProcess peer_;
    static inline std::unique_ptr<BufferManager> bufferManager_;
    static inline std::unique_ptr<MetadataManager> metadata_;
    static inline BufferPool flagBufferPool_;
    static inline std::unique_ptr<transport::TransportManager> localManager_;

    RequestQueue requestQueue_;
    CompletionQueue completionQueue_;
    ProtocolManager protocols_;
    std::unique_ptr<DramPoolRuntime> runtime_;
    DramPoolConfig savedConfig_;
};

TEST_F(CompletionPollerRealTransportTest, CompletesDumpAndPublishesTransferredData)
{
    std::vector<std::uint8_t> value(kValueSize);
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<std::uint8_t>(index + 1);
    }
    completionQueue_.Push(MakeDumpRecord(1, 0, 0, value));

    PollerRunner runner(*runtime_);
    KvResponse response;
    ASSERT_TRUE(WaitForResponse(0, KvOpcode::Dump, 1, response));
    runner.Stop();

    EXPECT_EQ(response.results,
              (std::vector<std::uint8_t>{static_cast<std::uint8_t>(DumpLoadResult::Ok)}));
    EntryPtr stored;
    ASSERT_TRUE(metadata_->LoadBegin(MakeKey(1), stored).Success());
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(std::memcmp(stored->buffer.addr, value.data(), value.size()), 0);
    EXPECT_TRUE(metadata_->LoadEnd(MakeKey(1)).Success());
}

TEST_F(CompletionPollerRealTransportTest, CompletesLoadAndReleasesMetadataReader)
{
    auto entry = std::make_shared<Entry>();
    entry->key = MakeKey(2);
    entry->size = kValueSize;
    entry->lifeTimeout = std::chrono::system_clock::now() + std::chrono::hours(1);
    ASSERT_TRUE(metadata_->StoreBegin(entry->key, entry).Success());
    std::vector<std::uint8_t> expected(kValueSize, 0xA5);
    std::memcpy(entry->buffer.addr, expected.data(), expected.size());
    ASSERT_TRUE(metadata_->StoreEnd(entry->key).Success());
    EntryPtr loaded;
    ASSERT_TRUE(metadata_->LoadBegin(entry->key, loaded).Success());

    transport::Operation operation;
    operation.opcode = transport::Opcode::Write;
    operation.direct = transport::OperationDirect::RemoteDeviceHost;
    operation.target_manager = peerManagerId_;
    operation.ops.emplace_back(transport::Segment{loaded->buffer.addr, DeviceAddress(20),
                                                  kValueSize});
    transport::TransferHandle handle = transport::kInvalidTransferHandle;
    ASSERT_EQ(localManager_->ExecuteAsync(operation, handle), transport::Status::Ok);

    CompletionRecord record;
    record.stage = CompletionStage::PollDataTransfer;
    record.opcode = KvOpcode::Load;
    record.data_handle = handle;
    record.response_addr = ResponseAddress(1);
    record.peer_one_sided_id = peerManagerId_;
    record.results = {static_cast<std::uint8_t>(DumpLoadResult::Failed)};
    record.transfer_items = {TransferItem{0, entry->key}};
    record.submit_ms = SteadyNowMs();
    completionQueue_.Push(std::move(record));

    PollerRunner runner(*runtime_);
    KvResponse response;
    ASSERT_TRUE(WaitForResponse(1, KvOpcode::Load, 1, response));
    runner.Stop();
    EXPECT_EQ(response.results,
              (std::vector<std::uint8_t>{static_cast<std::uint8_t>(DumpLoadResult::Ok)}));
    std::vector<std::uint8_t> actual;
    ASSERT_TRUE(peer_.ReadDevice(20 * kValueSize, kValueSize, actual));
    EXPECT_EQ(actual, expected);
    EXPECT_TRUE(metadata_->Delete(entry->key).Success());
}

TEST_F(CompletionPollerRealTransportTest, InvalidDataHandleAbortsDumpAndReturnsFailure)
{
    auto entry = std::make_shared<Entry>();
    entry->key = MakeKey(3);
    entry->size = kValueSize;
    entry->lifeTimeout = std::chrono::system_clock::now() + std::chrono::hours(1);
    ASSERT_TRUE(metadata_->StoreBegin(entry->key, entry).Success());

    CompletionRecord record;
    record.stage = CompletionStage::PollDataTransfer;
    record.opcode = KvOpcode::Dump;
    record.data_handle = transport::kInvalidTransferHandle;
    record.response_addr = ResponseAddress(2);
    record.peer_one_sided_id = peerManagerId_;
    record.results = {static_cast<std::uint8_t>(DumpLoadResult::Ok)};
    record.transfer_items = {TransferItem{0, entry->key}};
    record.submit_ms = SteadyNowMs();
    completionQueue_.Push(std::move(record));

    PollerRunner runner(*runtime_);
    KvResponse response;
    ASSERT_TRUE(WaitForResponse(2, KvOpcode::Dump, 1, response));
    runner.Stop();
    EXPECT_EQ(response.results,
              (std::vector<std::uint8_t>{static_cast<std::uint8_t>(DumpLoadResult::Failed)}));
    EXPECT_FALSE(metadata_->Query(MakeKey(3)));
}

TEST_F(CompletionPollerRealTransportTest, RefillsPendingWindowUntilEveryRecordCompletes)
{
    constexpr std::size_t kRecordCount = 6;
    g_config.pollerPendingDepth = 2;
    for (std::size_t index = 0; index < kRecordCount; ++index) {
        std::vector<std::uint8_t> value(kValueSize, static_cast<std::uint8_t>(0x20 + index));
        completionQueue_.Push(MakeDumpRecord(static_cast<std::uint8_t>(10 + index), index + 1,
                                             index + 3, value));
    }

    PollerRunner runner(*runtime_);
    for (std::size_t index = 0; index < kRecordCount; ++index) {
        KvResponse response;
        ASSERT_TRUE(WaitForResponse(index + 3, KvOpcode::Dump, 1, response));
        EXPECT_EQ(response.results,
                  (std::vector<std::uint8_t>{static_cast<std::uint8_t>(DumpLoadResult::Ok)}));
        EXPECT_TRUE(metadata_->Exist(MakeKey(static_cast<std::uint8_t>(10 + index))));
    }
    runner.Stop();
}

TEST_F(CompletionPollerRealTransportTest, SubmitsLookupResponseAndReleasesFlagBufferSlot)
{
    CompletionRecord record;
    record.stage = CompletionStage::SubmitResponse;
    record.opcode = KvOpcode::Lookup;
    record.response_addr = ResponseAddress(10);
    record.peer_one_sided_id = peerManagerId_;
    record.results = {1, 0, 1, 1, 0};
    completionQueue_.Push(std::move(record));

    PollerRunner runner(*runtime_);
    KvResponse response;
    ASSERT_TRUE(WaitForResponse(10, KvOpcode::Lookup, 5, response));
    runner.Stop();
    EXPECT_EQ(response.results, (std::vector<std::uint8_t>{1, 0, 1, 1, 0}));

    std::vector<BufferPool::Slot> slots(kPoolSlotCount);
    for (auto& slot : slots) { ASSERT_TRUE(flagBufferPool_.Allocate(slot).Success()); }
    BufferPool::Slot exhausted;
    EXPECT_EQ(flagBufferPool_.Allocate(exhausted), Status::NoSpace());
    for (const auto& slot : slots) { EXPECT_TRUE(flagBufferPool_.Free(slot.slot_index).Success()); }
}

TEST_F(CompletionPollerRealTransportTest, StopDrainsEveryQueuedResponse)
{
    constexpr std::size_t kResponseCount = 8;
    g_config.pollerPendingDepth = 2;
    for (std::size_t index = 0; index < kResponseCount; ++index) {
        CompletionRecord record;
        record.stage = CompletionStage::SubmitResponse;
        record.opcode = KvOpcode::Lookup;
        record.response_addr = ResponseAddress(index + 20);
        record.peer_one_sided_id = peerManagerId_;
        record.results = {static_cast<std::uint8_t>(index & 1U)};
        completionQueue_.Push(std::move(record));
    }

    PollerRunner runner(*runtime_);
    runner.Stop();
    for (std::size_t index = 0; index < kResponseCount; ++index) {
        KvResponse response;
        ASSERT_TRUE(WaitForResponse(index + 20, KvOpcode::Lookup, 1, response));
        EXPECT_EQ(response.results,
                  (std::vector<std::uint8_t>{static_cast<std::uint8_t>(index & 1U)}));
    }
}

}  // namespace
}  // namespace UC::DramPool

#endif
