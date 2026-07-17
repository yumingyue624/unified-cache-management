/**
 * MIT License
 *
 * Copyright (c) 2026 Huawei Technologies Co., Ltd. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * */
#include "kv_protocol.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace UC::DramPool {
namespace {

BlockId MakeKey(std::uint8_t seed)
{
    BlockId key{};
    for (std::size_t i = 0; i < key.size(); ++i) { key[i] = static_cast<std::byte>(seed + i); }
    return key;
}

void ExpectLe16(const std::vector<std::uint8_t>& buf, std::size_t offset, std::uint16_t value)
{
    EXPECT_EQ(buf[offset], static_cast<std::uint8_t>(value & 0xFF));
    EXPECT_EQ(buf[offset + 1], static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void ExpectLe32(const std::vector<std::uint8_t>& buf, std::size_t offset, std::uint32_t value)
{
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        EXPECT_EQ(buf[offset + i], static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

void ExpectLe64(const std::vector<std::uint8_t>& buf, std::size_t offset, std::uint64_t value)
{
    for (std::size_t i = 0; i < sizeof(value); ++i) {
        EXPECT_EQ(buf[offset + i], static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
    }
}

class KvProtocolTest : public ::testing::Test {
protected:
    ProtocolManager mgr_;
};

TEST_F(KvProtocolTest, PackDumpRequestMatchesLayout)
{
    KvDumpEntry entry;
    entry.key = MakeKey(0x10);
    entry.addr = 0x1122334455667788ULL;
    entry.len = 0xAABBCCDD;
    entry.idx = 0x12345678;

    KvDumpRequest req;
    req.opcode = KvOpcode::Dump;
    req.resp_addr = 0x0102030405060708ULL;
    req.batch_size = 1;
    req.ttl = 0x99AABBCCU;
    req.entries = {entry};

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    auto status = mgr_.PackRequest(packed.data(), req.opcode, req);
    ASSERT_TRUE(status.Success()) << status.ToString();

    EXPECT_EQ(packed[0], static_cast<std::uint8_t>(KvOpcode::Dump));
    ExpectLe64(packed, 1, req.resp_addr);
    ExpectLe16(packed, 9, req.batch_size);
    ExpectLe32(packed, kDumpTtlOffset, req.ttl);
    EXPECT_EQ(packed.size(), kKvDumpHeaderSize + kKvDumpEntrySize);
    EXPECT_EQ(std::memcmp(packed.data() + kKvDumpHeaderSize, entry.key.data(), kKvKeySize), 0);
    ExpectLe64(packed, kKvDumpHeaderSize + 16, entry.addr);
    ExpectLe32(packed, kKvDumpHeaderSize + 24, entry.len);
    ExpectLe32(packed, kKvDumpHeaderSize + 28, entry.idx);
}

TEST_F(KvProtocolTest, PackLoadRequestMatchesLayout)
{
    KvLoadEntry entry;
    entry.key = MakeKey(0x20);
    entry.addr = 0x8877665544332211ULL;
    entry.len = 0x1000;
    entry.idx = 7;

    KvLoadRequest req;
    req.opcode = KvOpcode::Load;
    req.resp_addr = 0x1111222233334444ULL;
    req.batch_size = 1;
    req.entries = {entry};

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    auto status = mgr_.PackRequest(packed.data(), req.opcode, req);
    ASSERT_TRUE(status.Success()) << status.ToString();

    EXPECT_EQ(packed[0], static_cast<std::uint8_t>(KvOpcode::Load));
    ExpectLe64(packed, 1, req.resp_addr);
    ExpectLe16(packed, 9, req.batch_size);
    EXPECT_EQ(std::memcmp(packed.data() + kKvHeaderSize, entry.key.data(), kKvKeySize), 0);
}

TEST_F(KvProtocolTest, PackLookupRequestMatchesLayout)
{
    KvLookupEntry entry0;
    entry0.key = MakeKey(0x30);
    KvLookupEntry entry1;
    entry1.key = MakeKey(0x40);

    KvLookupRequest req;
    req.opcode = KvOpcode::Lookup;
    req.resp_addr = 0x1234000056780000ULL;
    req.batch_size = 2;
    req.entries = {entry0, entry1};

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    auto status = mgr_.PackRequest(packed.data(), req.opcode, req);
    ASSERT_TRUE(status.Success()) << status.ToString();

    EXPECT_EQ(packed[0], static_cast<std::uint8_t>(KvOpcode::Lookup));
    ExpectLe64(packed, 1, req.resp_addr);
    ExpectLe16(packed, 9, req.batch_size);
    EXPECT_EQ(std::memcmp(packed.data() + kKvHeaderSize, entry0.key.data(), kKvKeySize), 0);
    EXPECT_EQ(std::memcmp(packed.data() + kKvHeaderSize + kKvLookupEntrySize, entry1.key.data(),
                          kKvKeySize),
              0);
}

TEST_F(KvProtocolTest, RejectsBatchSizeMismatch)
{
    KvLookupEntry entry;
    entry.key = MakeKey(0x50);

    KvLookupRequest req;
    req.opcode = KvOpcode::Lookup;
    req.resp_addr = 0x1000;
    req.batch_size = 2;
    req.entries = {entry};

    std::vector<std::uint8_t> packed(kKvHeaderSize + 2 * kKvLookupEntrySize, 0);
    auto status = mgr_.PackRequest(packed.data(), req.opcode, req);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("batch_size"), std::string::npos);
}

TEST_F(KvProtocolTest, RejectsAllZeroKey)
{
    KvLookupEntry entry;

    KvLookupRequest req;
    req.opcode = KvOpcode::Lookup;
    req.resp_addr = 0x1000;
    req.batch_size = 1;
    req.entries = {entry};

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    auto status = mgr_.PackRequest(packed.data(), req.opcode, req);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("key"), std::string::npos);
}

TEST_F(KvProtocolTest, RejectsZeroDumpLoadAddrAndLen)
{
    KvDumpEntry entry;
    entry.key = MakeKey(0x60);
    entry.addr = 0;
    entry.len = 1;

    KvDumpRequest req;
    req.opcode = KvOpcode::Dump;
    req.resp_addr = 0x1000;
    req.batch_size = 1;
    req.entries = {entry};

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    auto status = mgr_.PackRequest(packed.data(), req.opcode, req);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("addr"), std::string::npos);

    req.entries[0].addr = 0x2000;
    req.entries[0].len = 0;
    status = mgr_.PackRequest(packed.data(), req.opcode, req);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("len"), std::string::npos);
}

TEST_F(KvProtocolTest, UnpackRequestRejectsWrongSize)
{
    KvLookupEntry entry;
    entry.key = MakeKey(0x70);

    KvLookupRequest req;
    req.opcode = KvOpcode::Lookup;
    req.resp_addr = 0x1000;
    req.batch_size = 1;
    req.entries = {entry};

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    auto status = mgr_.PackRequest(packed.data(), req.opcode, req);
    ASSERT_TRUE(status.Success()) << status.ToString();

    std::unique_ptr<KvRequest> parsed;
    status = mgr_.UnpackRequest(packed.data(), packed.size() - 1, parsed);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("size"), std::string::npos);
}

TEST_F(KvProtocolTest, UnpackResponseReadsPackedResults)
{
    std::uint8_t lookupFlag[] = {static_cast<std::uint8_t>(ResponseStatus::Ready), 0x8D, 0x01};
    KvResponse resp;
    auto status = mgr_.UnpackResponse(lookupFlag, KvOpcode::Lookup, 9, resp);
    ASSERT_TRUE(status.Success()) << status.ToString();
    EXPECT_EQ(resp.results, (std::vector<std::uint8_t>{1, 0, 1, 1, 0, 0, 0, 1, 1}));

    std::uint8_t dumpFlag[] = {static_cast<std::uint8_t>(ResponseStatus::Ready), 0x10, 0xF2, 0x03};
    resp.results.clear();
    status = mgr_.UnpackResponse(dumpFlag, KvOpcode::Dump, 5, resp);
    ASSERT_TRUE(status.Success()) << status.ToString();
    EXPECT_EQ(resp.results, (std::vector<std::uint8_t>{0, 1, 2, 15, 3}));
}

TEST_F(KvProtocolTest, ServerRoundTripDumpLoad)
{
    KvLoadEntry entry;
    entry.key = MakeKey(0x80);
    entry.addr = 0xAABBCCDDEEFF0011ULL;
    entry.len = 0x2000;
    entry.idx = 0x55;

    KvLoadRequest req;
    req.opcode = KvOpcode::Load;
    req.resp_addr = 0x9988776655443322ULL;
    req.batch_size = 1;
    req.entries = {entry};

    // client packs
    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    ASSERT_TRUE(mgr_.PackRequest(packed.data(), req.opcode, req).Success());

    // server unpacks (validation merged into UnpackRequest)
    std::unique_ptr<KvRequest> parsed;
    ASSERT_TRUE(mgr_.UnpackRequest(packed.data(), packed.size(), parsed).Success());
    auto& dl = static_cast<KvLoadRequest&>(*parsed);
    EXPECT_EQ(dl.opcode, req.opcode);
    EXPECT_EQ(dl.resp_addr, req.resp_addr);
    EXPECT_EQ(dl.batch_size, req.batch_size);
    EXPECT_EQ(std::memcmp(dl.entries[0].key.data(), entry.key.data(), kKvKeySize), 0);
    EXPECT_EQ(dl.entries[0].addr, entry.addr);
    EXPECT_EQ(dl.entries[0].len, entry.len);
    EXPECT_EQ(dl.entries[0].idx, entry.idx);

    // server packs response, client unpacks it
    KvResponse resp;
    resp.results = {0x0};  // batch_size == 1 errcode
    std::vector<std::uint8_t> flag(mgr_.GetPackedResponseSize(req.opcode, resp.results.size()),
                                   0xFF);
    ASSERT_TRUE(mgr_.PackResponse(flag.data(), req.opcode, resp).Success());
    KvResponse resp2;
    ASSERT_TRUE(mgr_.UnpackResponse(flag.data(), req.opcode, req.batch_size, resp2).Success());
    ASSERT_EQ(resp2.results.size(), 1u);
    EXPECT_EQ(resp2.results[0], 0x0U);
}

TEST_F(KvProtocolTest, ServerRoundTripLookup)
{
    KvLookupEntry e0;
    e0.key = MakeKey(0x90);
    KvLookupEntry e1;
    e1.key = MakeKey(0xA0);
    KvLookupRequest req;
    req.opcode = KvOpcode::Lookup;
    req.resp_addr = 0x0E0E0E0E0E0E0E0EULL;
    req.batch_size = 2;
    req.entries = {e0, e1};

    // client packs
    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    ASSERT_TRUE(mgr_.PackRequest(packed.data(), req.opcode, req).Success());

    // server verifies + unpacks
    std::unique_ptr<KvRequest> parsed;
    ASSERT_TRUE(mgr_.UnpackRequest(packed.data(), packed.size(), parsed).Success());
    auto& lk = static_cast<KvLookupRequest&>(*parsed);
    EXPECT_EQ(lk.opcode, req.opcode);
    EXPECT_EQ(lk.resp_addr, req.resp_addr);
    EXPECT_EQ(lk.batch_size, req.batch_size);
    EXPECT_EQ(std::memcmp(lk.entries[0].key.data(), e0.key.data(), kKvKeySize), 0);
    EXPECT_EQ(std::memcmp(lk.entries[1].key.data(), e1.key.data(), kKvKeySize), 0);

    // server packs one existence bit per key, client unpacks it
    KvResponse resp;
    resp.results = {1, 0};
    std::vector<std::uint8_t> flag(mgr_.GetPackedResponseSize(req.opcode, resp.results.size()),
                                   0xFF);
    ASSERT_TRUE(mgr_.PackResponse(flag.data(), req.opcode, resp).Success());
    KvResponse resp2;
    ASSERT_TRUE(mgr_.UnpackResponse(flag.data(), req.opcode, req.batch_size, resp2).Success());
    EXPECT_EQ(resp2.results, resp.results);
}

// ---------------------------------------------------------------------------
// Boundary values: every field set to max
// ---------------------------------------------------------------------------

TEST_F(KvProtocolTest, DumpLoadMaxFieldValuesRoundTrip)
{
    KvLoadEntry entry;
    entry.key.fill(std::byte{0xFF});
    entry.addr = 0xFFFFFFFFFFFFFFFFULL;
    entry.len = 0xFFFFFFFFU;
    entry.idx = 0xFFFFFFFFU;

    KvLoadRequest req;
    req.opcode = KvOpcode::Load;
    req.resp_addr = 0xFFFFFFFFFFFFFFFFULL;
    req.batch_size = 1;
    req.entries = {entry};

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    ASSERT_TRUE(mgr_.PackRequest(packed.data(), req.opcode, req).Success());

    std::unique_ptr<KvRequest> parsed;
    ASSERT_TRUE(mgr_.UnpackRequest(packed.data(), packed.size(), parsed).Success());
    auto& dl = static_cast<KvLoadRequest&>(*parsed);
    EXPECT_EQ(dl.resp_addr, 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_EQ(dl.entries[0].addr, 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_EQ(dl.entries[0].len, 0xFFFFFFFFU);
    EXPECT_EQ(dl.entries[0].idx, 0xFFFFFFFFU);
    EXPECT_EQ(std::memcmp(dl.entries[0].key.data(), entry.key.data(), kKvKeySize), 0);
}

// ---------------------------------------------------------------------------
// Multi-entry: batch_size > 1, verify every entry survives the round-trip
// ---------------------------------------------------------------------------

TEST_F(KvProtocolTest, DumpLoadMultiEntryRoundTrip)
{
    constexpr std::uint16_t kBatch = 5;
    KvDumpRequest req;
    req.opcode = KvOpcode::Dump;
    req.resp_addr = 0xA5A5A5A5A5A5A5A5ULL;
    req.batch_size = kBatch;
    req.ttl = 0x10203040U;
    for (std::uint16_t i = 0; i < kBatch; ++i) {
        KvDumpEntry e;
        e.key = MakeKey(static_cast<std::uint8_t>(i * 0x10));
        e.addr = 0x1000ULL * (i + 1);
        e.len = 0x200U * (i + 1);
        e.idx = i;
        req.entries.push_back(e);
    }

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    ASSERT_TRUE(mgr_.PackRequest(packed.data(), req.opcode, req).Success());

    std::unique_ptr<KvRequest> parsed;
    ASSERT_TRUE(mgr_.UnpackRequest(packed.data(), packed.size(), parsed).Success());
    auto& dl = static_cast<KvDumpRequest&>(*parsed);
    EXPECT_EQ(dl.ttl, req.ttl);
    ASSERT_EQ(dl.entries.size(), kBatch);
    for (std::uint16_t i = 0; i < kBatch; ++i) {
        EXPECT_EQ(std::memcmp(dl.entries[i].key.data(), req.entries[i].key.data(), kKvKeySize), 0)
            << "entry " << i;
        EXPECT_EQ(dl.entries[i].addr, req.entries[i].addr) << "entry " << i;
        EXPECT_EQ(dl.entries[i].len, req.entries[i].len) << "entry " << i;
        EXPECT_EQ(dl.entries[i].idx, req.entries[i].idx) << "entry " << i;
    }
}

TEST_F(KvProtocolTest, LookupMultiEntryRoundTrip)
{
    constexpr std::uint16_t kBatch = 4;
    KvLookupRequest req;
    req.opcode = KvOpcode::Lookup;
    req.resp_addr = 0xB0B0B0B0B0B0B0B0ULL;
    req.batch_size = kBatch;
    for (std::uint16_t i = 0; i < kBatch; ++i) {
        KvLookupEntry e;
        e.key = MakeKey(static_cast<std::uint8_t>(0x50 + i));
        req.entries.push_back(e);
    }

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    ASSERT_TRUE(mgr_.PackRequest(packed.data(), req.opcode, req).Success());

    std::unique_ptr<KvRequest> parsed;
    ASSERT_TRUE(mgr_.UnpackRequest(packed.data(), packed.size(), parsed).Success());
    auto& lk = static_cast<KvLookupRequest&>(*parsed);
    ASSERT_EQ(lk.entries.size(), kBatch);
    for (std::uint16_t i = 0; i < kBatch; ++i) {
        EXPECT_EQ(std::memcmp(lk.entries[i].key.data(), req.entries[i].key.data(), kKvKeySize), 0)
            << "entry " << i;
    }
}

// ---------------------------------------------------------------------------
// opcode validation
// ---------------------------------------------------------------------------

TEST_F(KvProtocolTest, RejectsNoneOpcodeOnDumpLoad)
{
    KvDumpRequest req;  // opcode defaults to None
    req.resp_addr = 0x1000;
    req.batch_size = 1;
    req.entries = {
        KvDumpEntry{MakeKey(0x10), 0x2000, 0x100, 0}
    };

    std::vector<std::uint8_t> buf(mgr_.GetPackedSize(KvOpcode::Dump, req), 0);
    auto status = mgr_.PackRequest(buf.data(), KvOpcode::None, req);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("opcode"), std::string::npos);
}

TEST_F(KvProtocolTest, RejectsOpcodeMismatch)
{
    KvDumpRequest req;
    req.opcode = KvOpcode::Lookup;  // wrong opcode for a Dump request
    req.resp_addr = 0x1000;
    req.batch_size = 1;
    req.entries = {
        KvDumpEntry{MakeKey(0x10), 0x2000, 0x100, 0}
    };

    std::vector<std::uint8_t> buf(mgr_.GetPackedSize(KvOpcode::Dump, req), 0);
    auto status = mgr_.PackRequest(buf.data(), KvOpcode::Dump, req);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("opcode"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Server UnpackRequest edge cases (validation is now merged into UnpackRequest)
// ---------------------------------------------------------------------------

TEST_F(KvProtocolTest, UnpackRequestRejectsUnknownOpcode)
{
    std::vector<std::uint8_t> buf(kKvHeaderSize + kKvLookupEntrySize, 0);
    buf[0] = 0xEE;  // unknown opcode
    std::unique_ptr<KvRequest> parsed;
    auto status = mgr_.UnpackRequest(buf.data(), buf.size(), parsed);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("unknown opcode"), std::string::npos);
}

TEST_F(KvProtocolTest, UnpackRequestRejectsExtraBytes)
{
    KvLookupRequest req;
    req.opcode = KvOpcode::Lookup;
    req.resp_addr = 0x1000;
    req.batch_size = 1;
    req.entries = {KvLookupEntry{MakeKey(0x70)}};

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    ASSERT_TRUE(mgr_.PackRequest(packed.data(), req.opcode, req).Success());
    packed.push_back(0x00);  // one extra byte
    std::unique_ptr<KvRequest> parsed;
    auto status = mgr_.UnpackRequest(packed.data(), packed.size(), parsed);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("size"), std::string::npos);
}

TEST_F(KvProtocolTest, UnpackRequestRejectsNull)
{
    std::unique_ptr<KvRequest> out;
    auto status = mgr_.UnpackRequest(nullptr, kKvHeaderSize, out);
    EXPECT_FALSE(status.Success());
}

TEST_F(KvProtocolTest, UnpackRequestRejectsTooSmall)
{
    std::vector<std::uint8_t> buf(kKvHeaderSize - 1, 0);
    buf[0] = static_cast<std::uint8_t>(KvOpcode::Dump);
    std::unique_ptr<KvRequest> out;
    auto status = mgr_.UnpackRequest(buf.data(), buf.size(), out);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("header"), std::string::npos);
}

TEST_F(KvProtocolTest, UnpackRequestRejectsSizeMismatch)
{
    KvDumpRequest req;
    req.opcode = KvOpcode::Dump;
    req.resp_addr = 0x1000;
    req.batch_size = 2;
    req.entries = {
        KvDumpEntry{MakeKey(0x10), 0x2000, 0x100, 0},
        KvDumpEntry{MakeKey(0x20), 0x3000, 0x200, 1}
    };

    std::vector<std::uint8_t> packed(mgr_.GetPackedSize(req.opcode, req), 0);
    ASSERT_TRUE(mgr_.PackRequest(packed.data(), req.opcode, req).Success());
    std::unique_ptr<KvRequest> out;
    // truncated by 1 byte
    auto status = mgr_.UnpackRequest(packed.data(), packed.size() - 1, out);
    EXPECT_FALSE(status.Success());
    EXPECT_NE(status.ToString().find("size"), std::string::npos);
}

// ---------------------------------------------------------------------------
// PackResponse edge cases
// ---------------------------------------------------------------------------

TEST_F(KvProtocolTest, PackResponseRejectsNullData)
{
    KvResponse resp;
    resp.results = {0x0};
    auto status = mgr_.PackResponse(nullptr, KvOpcode::Dump, resp);
    EXPECT_FALSE(status.Success());
}

TEST_F(KvProtocolTest, PackResponseRejectsValuesThatDoNotFitWireWidth)
{
    std::uint8_t flag[2] = {0};
    KvResponse lookup;
    lookup.results = {2};
    auto status = mgr_.PackResponse(flag, KvOpcode::Lookup, lookup);
    EXPECT_FALSE(status.Success()) << status.ToString();
    EXPECT_EQ(flag[kResponseStatusOffset], static_cast<std::uint8_t>(ResponseStatus::Pending));

    KvResponse dump;
    dump.results = {16};
    status = mgr_.PackResponse(flag, KvOpcode::Dump, dump);
    EXPECT_FALSE(status.Success()) << status.ToString();
    EXPECT_EQ(flag[kResponseStatusOffset], static_cast<std::uint8_t>(ResponseStatus::Pending));
}

TEST_F(KvProtocolTest, PackResponseLookupRejectsZeroCount)
{
    KvResponse resp;  // empty results
    std::uint8_t flag[1] = {0};
    auto status = mgr_.PackResponse(flag, KvOpcode::Lookup, resp);
    EXPECT_FALSE(status.Success());
}

TEST_F(KvProtocolTest, PackResponseDumpLoadZeroErrcodes)
{
    KvResponse resp;  // empty, result_count=0
    std::uint8_t flag[1] = {0xFF};
    auto status = mgr_.PackResponse(flag, KvOpcode::Dump, resp);
    EXPECT_TRUE(status.Success());
    EXPECT_EQ(flag[kResponseStatusOffset], static_cast<std::uint8_t>(ResponseStatus::Ready));
}

// ---------------------------------------------------------------------------
// UnpackResponse edge cases
// ---------------------------------------------------------------------------

TEST_F(KvProtocolTest, UnpackResponseRejectsNullData)
{
    KvResponse resp;
    auto status = mgr_.UnpackResponse(nullptr, KvOpcode::Dump, 1, resp);
    EXPECT_FALSE(status.Success());
}

TEST_F(KvProtocolTest, ReportsPendingAndReadyResponseStatus)
{
    bool ready = true;
    const std::uint8_t pending[] = {static_cast<std::uint8_t>(ResponseStatus::Pending)};
    auto status = mgr_.IsResponseReady(pending, ready);
    ASSERT_TRUE(status.Success()) << status.ToString();
    EXPECT_FALSE(ready);

    const std::uint8_t completed[] = {static_cast<std::uint8_t>(ResponseStatus::Ready)};
    status = mgr_.IsResponseReady(completed, ready);
    ASSERT_TRUE(status.Success()) << status.ToString();
    EXPECT_TRUE(ready);
}

TEST_F(KvProtocolTest, ResponseStatusRejectsNullAndUnknownValues)
{
    bool ready = true;
    auto status = mgr_.IsResponseReady(nullptr, ready);
    EXPECT_TRUE(status.Failure());
    EXPECT_FALSE(ready);

    const std::uint8_t invalid[] = {2};
    ready = true;
    status = mgr_.IsResponseReady(invalid, ready);
    EXPECT_TRUE(status.Failure());
    EXPECT_FALSE(ready);
}

TEST_F(KvProtocolTest, UnpackResponseReturnsRetryWithoutChangingResultsWhilePending)
{
    const std::uint8_t pending[] = {static_cast<std::uint8_t>(ResponseStatus::Pending), 0xFF};
    KvResponse response;
    response.results = {7, 8};

    const auto status = mgr_.UnpackResponse(pending, KvOpcode::Dump, 2, response);

    EXPECT_EQ(status, Status::Retry());
    EXPECT_EQ(response.results, (std::vector<std::uint8_t>{7, 8}));
}

TEST_F(KvProtocolTest, PackedResponseSizesRoundUpAtBitBoundaries)
{
    EXPECT_EQ(mgr_.GetPackedResponseSize(KvOpcode::Lookup, 1), 2U);
    EXPECT_EQ(mgr_.GetPackedResponseSize(KvOpcode::Lookup, 8), 2U);
    EXPECT_EQ(mgr_.GetPackedResponseSize(KvOpcode::Lookup, 9), 3U);
    EXPECT_EQ(mgr_.GetPackedResponseSize(KvOpcode::Dump, 1), 2U);
    EXPECT_EQ(mgr_.GetPackedResponseSize(KvOpcode::Dump, 2), 2U);
    EXPECT_EQ(mgr_.GetPackedResponseSize(KvOpcode::Load, 3), 3U);
    EXPECT_EQ(mgr_.GetPackedResponseSize(KvOpcode::None, 3), 0U);
}

// ---------------------------------------------------------------------------
// Full response symmetry: PackResponse -> UnpackResponse exact match
// ---------------------------------------------------------------------------

TEST_F(KvProtocolTest, ResponseSymmetryMultipleErrcodes)
{
    KvResponse resp;
    resp.results = {0x0, 0x1, 0x2, 0xE, 0xF};
    constexpr std::uint16_t kCount = 5;
    std::vector<std::uint8_t> flag(mgr_.GetPackedResponseSize(KvOpcode::Dump, kCount), 0xFF);

    ASSERT_TRUE(mgr_.PackResponse(flag.data(), KvOpcode::Dump, resp).Success());
    EXPECT_EQ(flag, (std::vector<std::uint8_t>{static_cast<std::uint8_t>(ResponseStatus::Ready),
                                               0x10, 0xE2, 0x0F}));
    KvResponse resp2;
    ASSERT_TRUE(mgr_.UnpackResponse(flag.data(), KvOpcode::Dump, kCount, resp2).Success());
    ASSERT_EQ(resp2.results.size(), kCount);
    for (std::uint16_t i = 0; i < kCount; ++i) {
        EXPECT_EQ(resp2.results[i], resp.results[i]) << "result " << i;
    }
}

// ---------------------------------------------------------------------------
// Multi-round: same manager handles many sequential operations
// ---------------------------------------------------------------------------

TEST_F(KvProtocolTest, MultiRoundSequentialPacks)
{
    for (std::uint8_t round = 0; round < 10; ++round) {
        const auto opcode = (round % 2 == 0) ? KvOpcode::Dump : KvOpcode::Load;
        const auto resp_addr = 0x1000ULL * (round + 1);
        const auto addr = 0x2000ULL * (round + 1);
        const auto len = 0x100U * (round + 1);

        std::vector<std::uint8_t> packed;
        if (opcode == KvOpcode::Dump) {
            KvDumpRequest req;
            req.opcode = opcode;
            req.resp_addr = resp_addr;
            req.batch_size = 1;
            req.ttl = 0x500U * (round + 1);
            req.entries = {
                KvDumpEntry{MakeKey(round), addr, len, round}
            };
            packed.resize(mgr_.GetPackedSize(opcode, req), 0);
            ASSERT_TRUE(mgr_.PackRequest(packed.data(), opcode, req).Success())
                << "round " << round;
        } else {
            KvLoadRequest req;
            req.opcode = opcode;
            req.resp_addr = resp_addr;
            req.batch_size = 1;
            req.entries = {
                KvLoadEntry{MakeKey(round), addr, len, round}
            };
            packed.resize(mgr_.GetPackedSize(opcode, req), 0);
            ASSERT_TRUE(mgr_.PackRequest(packed.data(), opcode, req).Success())
                << "round " << round;
        }

        std::unique_ptr<KvRequest> parsed;
        ASSERT_TRUE(mgr_.UnpackRequest(packed.data(), packed.size(), parsed).Success())
            << "round " << round;
        if (opcode == KvOpcode::Dump) {
            auto& dl = static_cast<KvDumpRequest&>(*parsed);
            EXPECT_EQ(dl.opcode, opcode) << "round " << round;
            EXPECT_EQ(dl.resp_addr, resp_addr) << "round " << round;
            EXPECT_EQ(dl.ttl, 0x500U * (round + 1)) << "round " << round;
            EXPECT_EQ(dl.entries[0].idx, round) << "round " << round;
        } else {
            auto& dl = static_cast<KvLoadRequest&>(*parsed);
            EXPECT_EQ(dl.opcode, opcode) << "round " << round;
            EXPECT_EQ(dl.resp_addr, resp_addr) << "round " << round;
            EXPECT_EQ(dl.entries[0].idx, round) << "round " << round;
        }

        // response round-trip each iteration
        KvResponse resp;
        resp.results = {round};
        std::uint8_t flag[2] = {0xFF, 0xFF};
        ASSERT_TRUE(mgr_.PackResponse(flag, opcode, resp).Success()) << "round " << round;
        KvResponse resp2;
        ASSERT_TRUE(mgr_.UnpackResponse(flag, opcode, 1, resp2).Success()) << "round " << round;
        EXPECT_EQ(resp2.results[0], round) << "round " << round;
    }
}

// ---------------------------------------------------------------------------
// Full client-server round-trip with response values spanning the range
// ---------------------------------------------------------------------------

TEST_F(KvProtocolTest, LookupPackedResponseOverwritesNonZeroBufferAndRoundTrips)
{
    KvResponse resp;
    resp.results = {1, 0, 1, 1, 0, 0, 0, 1, 1};
    std::vector<std::uint8_t> flag(
        mgr_.GetPackedResponseSize(KvOpcode::Lookup, resp.results.size()), 0xFF);

    ASSERT_TRUE(mgr_.PackResponse(flag.data(), KvOpcode::Lookup, resp).Success());
    EXPECT_EQ(flag, (std::vector<std::uint8_t>{static_cast<std::uint8_t>(ResponseStatus::Ready),
                                               0x8D, 0x01}));

    KvResponse unpacked;
    ASSERT_TRUE(mgr_.UnpackResponse(flag.data(), KvOpcode::Lookup,
                                    static_cast<std::uint16_t>(resp.results.size()), unpacked)
                    .Success());
    EXPECT_EQ(unpacked.results, resp.results);
}

}  // namespace
}  // namespace UC::DramPool
