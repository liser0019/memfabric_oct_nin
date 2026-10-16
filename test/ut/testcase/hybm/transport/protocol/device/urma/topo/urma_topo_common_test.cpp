/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#define private public
#include "hybm_define.h"
#include "dl_acl_api.h"
#include "dl_hal_api.h"
#include "hybm_gva_version.h"
#include "urma_topo_common.h"
#include "urma_topo_helper.h"
#include "urma_topo_json_parser.h"
#include "urma_topo_manager.h"
#include "urma_topo_product.h"
#undef private

using namespace ock::mf;
using namespace ock::mf::transport;
namespace dev = ock::mf::transport::device;

// ============================ EID hex conversion ============================

TEST(UrmaTopoCommonTest, HexStrToEid_NormalLowercase)
{
    std::string hex = "e0e1e2e3e4e5e6e7e8e9eaebecedeeef";
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    HexStrToEid(hex, eid);
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        EXPECT_EQ(eid[i], static_cast<uint8_t>(0xe0 + i));
    }
}

TEST(UrmaTopoCommonTest, HexStrToEid_NormalUppercase)
{
    std::string hex = "AABBCCDDEEFF0011";
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    HexStrToEid(hex, eid);
    EXPECT_EQ(eid[0], 0xAA);
    EXPECT_EQ(eid[1], 0xBB);
    EXPECT_EQ(eid[2], 0xCC);
    EXPECT_EQ(eid[3], 0xDD);
    EXPECT_EQ(eid[4], 0xEE);
    EXPECT_EQ(eid[5], 0xFF);
    EXPECT_EQ(eid[6], 0x00);
    EXPECT_EQ(eid[7], 0x11);
    for (size_t i = 8; i < COMM_ADDR_EID_LEN; ++i) {
        EXPECT_EQ(eid[i], 0);
    }
}

TEST(UrmaTopoCommonTest, HexStrToEid_EmptyString)
{
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    eid[0] = 0xFF;
    HexStrToEid("", eid);
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        EXPECT_EQ(eid[i], 0);
    }
}

TEST(UrmaTopoCommonTest, HexStrToEid_ShortString)
{
    std::string hex = "ab";
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    HexStrToEid(hex, eid);
    EXPECT_EQ(eid[0], 0xAB);
    for (size_t i = 1; i < COMM_ADDR_EID_LEN; ++i) {
        EXPECT_EQ(eid[i], 0);
    }
}

TEST(UrmaTopoCommonTest, EidToHexStr_RoundTrip)
{
    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        eid[i] = static_cast<uint8_t>(i * 16 + (i % 16));
    }
    std::string hex;
    EidToHexStr(*reinterpret_cast<dcmi_urma_eid *>(eid.data()), hex);
    EXPECT_EQ(hex.size(), COMM_ADDR_EID_LEN * 2);
    std::array<uint8_t, COMM_ADDR_EID_LEN> decoded{};
    HexStrToEid(hex, decoded);
    for (size_t i = 0; i < COMM_ADDR_EID_LEN; ++i) {
        EXPECT_EQ(decoded[i], eid[i]);
    }
}

// ============================ Card EID extraction ============================

TEST(UrmaTopoCommonTest, GetCardPortId_AllZeros)
{
    dcmi_urma_eid eid{};
    EXPECT_EQ(GetCardPortId(eid), 0);
}

TEST(UrmaTopoCommonTest, GetCardPortId_MaxPort)
{
    dcmi_urma_eid eid{};
    eid.raw[DCMI_URMA_EID_SIZE - 1] = 0x7E;
    EXPECT_EQ(GetCardPortId(eid), 0x0F);
}

TEST(UrmaTopoCommonTest, GetCardDieId_Bit2Set)
{
    dcmi_urma_eid eid{};
    eid.raw[DCMI_URMA_EID_SIZE - 1] = 0x04;
    EXPECT_EQ(GetCardDieId(eid), 1);
}

TEST(UrmaTopoCommonTest, GetCardDieId_Bit2Clear)
{
    dcmi_urma_eid eid{};
    eid.raw[DCMI_URMA_EID_SIZE - 1] = 0x00;
    EXPECT_EQ(GetCardDieId(eid), 0);
}

// ============================ Server/Pod EID extraction ============================

TEST(UrmaTopoCommonTest, UrmaEidGetFeId_Basic)
{
    dcmi_urma_eid eid{};
    eid.raw[6] = 0x45;
    EXPECT_EQ(UrmaEidGetFeId(eid), 0x45);
}

TEST(UrmaTopoCommonTest, UrmaEidGetFeId_MasksHighBit)
{
    dcmi_urma_eid eid{};
    eid.raw[6] = 0x80;
    EXPECT_EQ(UrmaEidGetFeId(eid), 0);
    eid.raw[6] = 0xFF;
    EXPECT_EQ(UrmaEidGetFeId(eid), 0x7F);
}

TEST(UrmaTopoCommonTest, UrmaEidGetPortId_Basic)
{
    dcmi_urma_eid eid{};
    eid.raw[5] = 0x2A;
    EXPECT_EQ(UrmaEidGetPortId(eid), 0x2A);
}

TEST(UrmaTopoCommonTest, UrmaEidGetPortId_MasksUpperBits)
{
    dcmi_urma_eid eid{};
    eid.raw[5] = 0xFF;
    EXPECT_EQ(UrmaEidGetPortId(eid), 0x3F);
}

TEST(UrmaTopoCommonTest, UrmaEidGetDieId_Basic)
{
    dcmi_urma_eid eid{};
    eid.raw[5] = 0xC0;
    EXPECT_EQ(UrmaEidGetDieId(eid), 3);
    eid.raw[5] = 0x00;
    EXPECT_EQ(UrmaEidGetDieId(eid), 0);
}

TEST(UrmaTopoCommonTest, UrmaEidIsPortGroup_True)
{
    dcmi_urma_eid eid{};
    eid.raw[5] = 0x3F;
    EXPECT_TRUE(UrmaEidIsPortGroup(eid));
}

TEST(UrmaTopoCommonTest, UrmaEidIsPortGroup_False)
{
    dcmi_urma_eid eid{};
    eid.raw[5] = 0x3E;
    EXPECT_FALSE(UrmaEidIsPortGroup(eid));
}

TEST(UrmaTopoCommonTest, UrmaEidIsUBOE_True)
{
    dcmi_urma_eid eid{};
    eid.raw[7] = 0xC0;
    EXPECT_TRUE(UrmaEidIsUBOE(eid));
    eid.raw[7] = 0xFF;
    EXPECT_TRUE(UrmaEidIsUBOE(eid));
}

TEST(UrmaTopoCommonTest, UrmaEidIsUBOE_False)
{
    dcmi_urma_eid eid{};
    eid.raw[7] = 0x00;
    EXPECT_FALSE(UrmaEidIsUBOE(eid));
    eid.raw[7] = 0x80;
    EXPECT_FALSE(UrmaEidIsUBOE(eid));
}

TEST(UrmaTopoCommonTest, UrmaEidIsUbRtp_True)
{
    dcmi_urma_eid eid{};
    eid.raw[7] = 0x80;
    EXPECT_TRUE(UrmaEidIsUbRtp(eid));
}

TEST(UrmaTopoCommonTest, UrmaEidIsUbRtp_False)
{
    dcmi_urma_eid eid{};
    eid.raw[7] = 0x00;
    EXPECT_FALSE(UrmaEidIsUbRtp(eid));
    eid.raw[7] = 0xC0;
    EXPECT_FALSE(UrmaEidIsUbRtp(eid));
}

// ============================ UrmaEid2CNA ============================

TEST(UrmaTopoCommonTest, UrmaEid2CNA_Basic)
{
    dcmi_urma_eid eid{};
    eid.raw[6] = 192;
    eid.raw[12] = 168;
    eid.raw[13] = 1;
    eid.raw[14] = 100;
    std::string cna;
    UrmaEid2CNA(eid, cna);
    EXPECT_EQ(cna, "192.168.1.100");
}

// ============================ UrmaEntity helpers ============================

TEST(UrmaTopoCommonTest, UrmaEntityGetId_EmptyReturnsNegOne)
{
    urmaEntity ue{};
    EXPECT_EQ(UrmaEntityGetId(ue), -1);
}

TEST(UrmaTopoCommonTest, UrmaEntityGetId_NonEmpty)
{
    urmaEntity ue{};
    ue.eidNum = 1;
    ue.eidList[0].eid.raw[6] = 0x33;
    EXPECT_EQ(UrmaEntityGetId(ue), 0x33);
}

TEST(UrmaTopoCommonTest, UrmaEntityGetDieId_EmptyReturnsNegOne)
{
    urmaEntity ue{};
    EXPECT_EQ(UrmaEntityGetDieId(ue), -1);
}

TEST(UrmaTopoCommonTest, UrmaEntityGetPortGroupIdx_NotFound)
{
    urmaEntity ue{};
    ue.eidNum = 2;
    ue.eidList[0].eid.raw[5] = 0x01;
    ue.eidList[1].eid.raw[5] = 0x02;
    EXPECT_EQ(UrmaEntityGetPortGroupIdx(ue), -1);
}

TEST(UrmaTopoCommonTest, UrmaEntityGetPortGroupIdx_Found)
{
    urmaEntity ue{};
    ue.eidNum = 3;
    ue.eidList[0].eid.raw[5] = 0x01;
    ue.eidList[1].eid.raw[5] = 0x3F;
    ue.eidList[2].eid.raw[5] = 0x02;
    EXPECT_EQ(UrmaEntityGetPortGroupIdx(ue), 1);
}

// ============================ ToString ============================

TEST(UrmaTopoCommonTest, RootInfoToString_Empty)
{
    RootInfo ri{};
    std::string s = RootInfoToString(ri);
    EXPECT_NE(s.find("rankCount:0"), std::string::npos);
}

TEST(UrmaTopoCommonTest, RootInfoToString_WithData)
{
    RootInfo ri{};
    ri.version = "2.0";
    ri.topoFilePath = "/test.json";
    ri.rankCount = 1;
    Rank r;
    r.deviceId = 5;
    r.localId = 5;
    Level lvl;
    lvl.netLayer = 0;
    lvl.netInstanceId = "sp_1";
    lvl.netType = "TOPO_FILE_DESC";
    RankAddr addr;
    addr.addrType = "EID";
    addr.addr = "e0e1e2e3e4e5e6e7e8e9eaebecedeeef";
    addr.ports = {"0/0", "0/1"};
    addr.planeId = "plane_0";
    lvl.rankAddrList.push_back(std::move(addr));
    r.levelList.push_back(std::move(lvl));
    ri.rankList.push_back(std::move(r));
    std::string s = RootInfoToString(ri);
    EXPECT_NE(s.find("ver:2.0"), std::string::npos);
    EXPECT_NE(s.find("/test.json"), std::string::npos);
    EXPECT_NE(s.find("dev:5"), std::string::npos);
    EXPECT_NE(s.find("EID"), std::string::npos);
}

TEST(UrmaTopoCommonTest, TopoInfoToString_WithData)
{
    TopoInfo ti{};
    ti.version = "1.0";
    ti.hardwareType = "Atlas 850";
    ti.peerCount = 2;
    ti.peerList = {0, 1};
    ti.edgeCount = 1;
    Edge e;
    e.netLayer = 0;
    e.linkType = "HCCS";
    e.localA = 0;
    e.localB = 1;
    e.localAPorts = {"0/0"};
    e.localBPorts = {"1/0"};
    e.protocols = {"HCCS-4P"};
    ti.edgeList.push_back(std::move(e));
    std::string s = TopoInfoToString(ti);
    EXPECT_NE(s.find("Atlas 850"), std::string::npos);
    EXPECT_NE(s.find("HCCS"), std::string::npos);
}

// ============================ JSON parser: SkipWs ============================

TEST(UrmaTopoJsonParserTest, SkipWs_Spaces)
{
    std::string json = "   abc";
    size_t pos = dev::SkipWs(json.data(), json.data() + json.size(), 0);
    EXPECT_EQ(pos, 3U);
}

TEST(UrmaTopoJsonParserTest, SkipWs_NoWhitespace)
{
    std::string json = "abc";
    size_t pos = dev::SkipWs(json.data(), json.data() + json.size(), 0);
    EXPECT_EQ(pos, 0U);
}

// ============================ JSON parser: ExtractStrField ============================

TEST(UrmaTopoJsonParserTest, ExtractStrField_Found)
{
    std::string json = R"({"name":"hello","val":42})";
    std::string out;
    EXPECT_EQ(dev::ExtractStrField(json, "\"name\"", out), BM_OK);
    EXPECT_EQ(out, "hello");
}

TEST(UrmaTopoJsonParserTest, ExtractStrField_NotFound)
{
    std::string json = R"({"other":"hello"})";
    std::string out;
    EXPECT_NE(dev::ExtractStrField(json, "\"name\"", out), BM_OK);
}

TEST(UrmaTopoJsonParserTest, ExtractStrField_WithEscapedQuote)
{
    std::string json = R"({"path":"a\"b"})";
    std::string out;
    EXPECT_EQ(dev::ExtractStrField(json, "\"path\"", out), BM_OK);
    EXPECT_EQ(out, "a\\\"b");
}

// ============================ JSON parser: ExtractIntField ============================

TEST(UrmaTopoJsonParserTest, ExtractIntField_Found)
{
    std::string json = R"({"count":42,"other":"x"})";
    int32_t out = 0;
    EXPECT_EQ(dev::ExtractIntField(json, "\"count\"", out), BM_OK);
    EXPECT_EQ(out, 42);
}

TEST(UrmaTopoJsonParserTest, ExtractIntField_NotFound)
{
    std::string json = R"({"other":"x"})";
    int32_t out = 0;
    EXPECT_NE(dev::ExtractIntField(json, "\"count\"", out), BM_OK);
}

TEST(UrmaTopoJsonParserTest, ExtractIntField_NegativeRejected)
{
    std::string json = R"({"val":-5})";
    int32_t out = 0;
    EXPECT_NE(dev::ExtractIntField(json, "\"val\"", out), BM_OK);
}

// ============================ JSON parser: ParseJsonArrayHeader ============================

TEST(UrmaTopoJsonParserTest, ParseJsonArrayHeader_Valid)
{
    std::string json = R"({"items":[1, 2, 3]})";
    size_t pos = 0;
    EXPECT_EQ(dev::ParseJsonArrayHeader(json, "\"items\"", pos), BM_OK);
    EXPECT_LT(pos, json.size());
}

TEST(UrmaTopoJsonParserTest, ParseJsonArrayHeader_NotArray)
{
    std::string json = R"({"items":"not_array"})";
    size_t pos = 0;
    EXPECT_NE(dev::ParseJsonArrayHeader(json, "\"items\"", pos), BM_OK);
}

TEST(UrmaTopoJsonParserTest, ParseJsonArrayHeader_KeyNotFound)
{
    std::string json = R"({"other":[1,2]})";
    size_t pos = 0;
    EXPECT_NE(dev::ParseJsonArrayHeader(json, "\"items\"", pos), BM_OK);
}

// ============================ JSON parser: ParseStrArray ============================

TEST(UrmaTopoJsonParserTest, ParseStrArray_WithItems)
{
    std::string json = R"({"ports":["0/0","0/1","1/2"]})";
    std::vector<std::string> out;
    EXPECT_EQ(dev::ParseStrArray(json, "\"ports\"", out), BM_OK);
    EXPECT_EQ(out.size(), 3U);
    EXPECT_EQ(out[0], "0/0");
    EXPECT_EQ(out[1], "0/1");
    EXPECT_EQ(out[2], "1/2");
}

TEST(UrmaTopoJsonParserTest, ParseStrArray_Empty)
{
    std::string json = R"({"ports":[]})";
    std::vector<std::string> out;
    EXPECT_EQ(dev::ParseStrArray(json, "\"ports\"", out), BM_OK);
    EXPECT_EQ(out.size(), 0U);
}

TEST(UrmaTopoJsonParserTest, ParseStrArray_KeyNotFound)
{
    std::string json = R"({"other":["a"]})";
    std::vector<std::string> out;
    EXPECT_EQ(dev::ParseStrArray(json, "\"ports\"", out), BM_OK);
    EXPECT_EQ(out.size(), 0U);
}

// ============================ JSON parser: SkipValue ============================

TEST(UrmaTopoJsonParserTest, SkipValue_String)
{
    std::string json = R"("hello world")";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, json.size());
}

TEST(UrmaTopoJsonParserTest, SkipValue_Number)
{
    std::string json = "12345,";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 5U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_Object)
{
    std::string json = R"({"a":1},"x")";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 7U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_Array)
{
    std::string json = R"([1,2,3],"x")";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 7U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_LiteralTrue)
{
    std::string json = "true,";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 4U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_LiteralFalse)
{
    std::string json = "false,";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 5U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_LiteralNull)
{
    std::string json = "null,";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 4U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_Invalid)
{
    std::string json = "abc";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_NE(r.result, BM_OK);
}

// ============================ JSON parser: FindQuotedKeyInRange / ExtractIntInRange ============================

TEST(UrmaTopoJsonParserTest, ExtractIntInRange_Found)
{
    std::string json = R"({"local_id":42,"other":"x"})";
    int32_t out = 0;
    EXPECT_EQ(dev::ExtractIntInRange(json.data(), 0, json.size(), "\"local_id\"", out), BM_OK);
    EXPECT_EQ(out, 42);
}

TEST(UrmaTopoJsonParserTest, ExtractIntInRange_NotFound)
{
    std::string json = R"({"other":"x"})";
    int32_t out = 0;
    EXPECT_NE(dev::ExtractIntInRange(json.data(), 0, json.size(), "\"local_id\"", out), BM_OK);
}

// ============================ Manager: ParseRankListFull ============================

class UrmaTopoManagerTest : public testing::Test {
protected:
    void SetUp() override
    {
        UrmaTopoManager::GetInstance().Uninitialize();
    }
    void TearDown() override
    {
        UrmaTopoManager::GetInstance().Uninitialize();
    }
};

namespace {
constexpr int32_t MOCK_USER_DEVICE_ID = 0;
constexpr int32_t MOCK_LOGIC_DEVICE_ID = 1;
constexpr int32_t MOCK_PHY_DEVICE_ID = 1;
constexpr int32_t kLocalDeviceId = 0;
constexpr int32_t kMeshLayer = 0;
constexpr int32_t kCtpLayer = 1;
constexpr int32_t kRtpLayer = 2;

int32_t MockGetLogicIdForVisibleDevice(int32_t userDeviceId, int32_t *logicDeviceId)
{
    EXPECT_EQ(userDeviceId, MOCK_USER_DEVICE_ID);
    *logicDeviceId = MOCK_LOGIC_DEVICE_ID;
    return BM_OK;
}

int32_t MockGetPhyIdForVisibleDevice(int32_t deviceId, int32_t *phyDeviceId)
{
    EXPECT_EQ(deviceId, MOCK_USER_DEVICE_ID);
    *phyDeviceId = MOCK_PHY_DEVICE_ID;
    return BM_OK;
}

int32_t MockGetDeviceInfoForVisibleDevice(uint32_t deviceId, int32_t, int32_t infoType, int64_t *value)
{
    EXPECT_EQ(deviceId, static_cast<uint32_t>(MOCK_USER_DEVICE_ID));
    *value = (infoType == INFO_TYPE_MAINBOARD_ID) ? MAIN_BOARD_ID_POD : 0;
    return BM_OK;
}

Rank &AddLocalRank(UrmaTopoManager &mgr)
{
    mgr.phyDeviceId_ = kLocalDeviceId;
    mgr.localId_ = kLocalDeviceId;
    Rank rank{};
    rank.deviceId = kLocalDeviceId;
    rank.localId = kLocalDeviceId;
    mgr.rootInfo_.rankList.push_back(std::move(rank));
    return mgr.rootInfo_.rankList.back();
}

void AddEidAddr(Rank &rank, int32_t netLayer, const std::string &eid, const std::vector<std::string> &ports)
{
    Level level{};
    level.netLayer = netLayer;
    RankAddr addr{};
    addr.addrType = ADDR_TYPE_EID;
    addr.addr = eid;
    addr.ports = ports;
    level.rankAddrList.push_back(std::move(addr));
    rank.levelList.push_back(std::move(level));
}

void AddPeer2NetEdge(UrmaTopoManager &mgr, int32_t netLayer, const std::string &protocol,
                     const std::vector<std::string> &ports)
{
    Edge edge{};
    edge.netLayer = netLayer;
    edge.linkType = "PEER2NET";
    edge.localA = kLocalDeviceId;
    edge.protocols = {protocol};
    edge.position = "DEVICE";
    edge.localAPorts = ports;
    mgr.topoInfo_.edgeList.push_back(std::move(edge));
}
} // namespace

TEST_F(UrmaTopoManagerTest, SetDeviceBaseInfoUsesUserDeviceId)
{
    MOCKER(&DlAclApi::RtGetLogicDevIdByUserDevId).stubs().will(invoke(MockGetLogicIdForVisibleDevice));
    MOCKER(&DlAclApi::AclrtGetPhyDevIdByLogicDevId).stubs().will(invoke(MockGetPhyIdForVisibleDevice));
    MOCKER(&DlAclApi::RtGetDeviceInfo).stubs().will(invoke(MockGetDeviceInfoForVisibleDevice));

    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.userDeviceId_ = MOCK_USER_DEVICE_ID;
    ASSERT_EQ(mgr.SetDeviceIdentity(), BM_OK);
    EXPECT_EQ(mgr.SetDeviceBaseInfo(), BM_OK);
    EXPECT_EQ(mgr.logicDeviceId_, MOCK_LOGIC_DEVICE_ID);
    EXPECT_EQ(mgr.phyDeviceId_, MOCK_PHY_DEVICE_ID);
    EXPECT_EQ(mgr.localId_, MOCK_PHY_DEVICE_ID);
    GlobalMockObject::verify();
}

TEST_F(UrmaTopoManagerTest, ParseRankListFull_FiltersByPhyDeviceId)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.phyDeviceId_ = 3;
    std::string json = R"({"rank_list":[{"device_id":1,"local_id":1,"level_list":[]},)"
                       R"({"device_id":3,"local_id":3,"level_list":[]},)"
                       R"({"device_id":5,"local_id":5,"level_list":[]}]})";
    std::vector<Rank> ranks;
    EXPECT_EQ(mgr.ParseRankListFull(json, ranks), BM_OK);
    ASSERT_EQ(ranks.size(), 1U);
    EXPECT_EQ(ranks[0].deviceId, 3);
    EXPECT_EQ(ranks[0].localId, 3);
}

TEST_F(UrmaTopoManagerTest, ParseRankListFull_NoMatch)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.phyDeviceId_ = 99;
    std::string json = R"({"rank_list":[{"device_id":1,"local_id":1,"level_list":[]}]})";
    std::vector<Rank> ranks;
    EXPECT_EQ(mgr.ParseRankListFull(json, ranks), BM_OK);
    EXPECT_EQ(ranks.size(), 0U);
}

TEST_F(UrmaTopoManagerTest, ParseRankListFull_InvalidJson)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.phyDeviceId_ = 1;
    std::string json = R"({"rank_list":"not_array"})";
    std::vector<Rank> ranks;
    EXPECT_NE(mgr.ParseRankListFull(json, ranks), BM_OK);
}

TEST_F(UrmaTopoManagerTest, ParseRankListFull_KeyMissing)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.phyDeviceId_ = 1;
    std::string json = R"({"other":[]})";
    std::vector<Rank> ranks;
    EXPECT_NE(mgr.ParseRankListFull(json, ranks), BM_OK);
}

// ============================ Manager: ParseEdgeList ============================

TEST_F(UrmaTopoManagerTest, ParseEdgeList_FiltersByLocalId)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.localId_ = 0;
    std::string json =
        R"({"edge_list":[)"
        R"({"link_type":"PEER2PEER","local_a":0,"local_b":1,"local_a_ports":["0/0"],"local_b_ports":["1/0"]},)"
        R"({"link_type":"PEER2PEER","local_a":2,"local_b":3,"local_a_ports":["0/1"],"local_b_ports":["1/1"]}]})";
    std::vector<Edge> edges;
    EXPECT_EQ(mgr.ParseEdgeList(json, edges), BM_OK);
    ASSERT_EQ(edges.size(), 1U);
    EXPECT_EQ(edges[0].localA, 0);
    EXPECT_EQ(edges[0].localB, 1);
    ASSERT_EQ(edges[0].localAPorts.size(), 1U);
    EXPECT_EQ(edges[0].localAPorts[0], "0/0");
}

TEST_F(UrmaTopoManagerTest, ParseTopoInfo_SupportsPeer2PeerAndPeer2NetConsumers)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    auto &rank = AddLocalRank(mgr);
    AddEidAddr(rank, kMeshLayer, "22222222222222222222222222222222", {"1/7"});
    rank.levelList.back().netType = NET_TYPE_TOPO_FILE_DESC;
    AddEidAddr(rank, kCtpLayer, "33333333333333333333333333333333", {"1/8"});
    rank.levelList.back().netType = NET_TYPE_CLOS;
    std::string json = R"({"edge_count":2,"peer_list":[],"edge_list":[)"
                       R"({"net_layer":0,"link_type":"PEER2PEER","local_a":7,"local_b":0,)"
                       R"("local_a_ports":["0/6"],"local_b_ports":["1/7"]},)"
                       R"({"net_layer":1,"link_type":"PEER2NET","local_a":0,"local_a_ports":["1/8"],)"
                       R"("protocols":["UB_CTP"],"position":"DEVICE"}]})";

    EXPECT_EQ(mgr.ParseTopoInfo(json, mgr.topoInfo_), BM_OK);
    ASSERT_EQ(mgr.topoInfo_.edgeList.size(), 2U);
    EXPECT_EQ(mgr.BuildPeer2PeerMap(), BM_OK);
    ASSERT_EQ(mgr.peer2PeerMap_.size(), 1U);
    const auto peer = mgr.peer2PeerMap_.find(7);
    ASSERT_NE(peer, mgr.peer2PeerMap_.end());
    std::array<uint8_t, COMM_ADDR_EID_LEN> expected{};
    expected.fill(0x22U);
    EXPECT_EQ(peer->second, expected);

    std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
    EXPECT_EQ(mgr.GetPeer2NetEid(COMM_PROTOCOL_UBC_CTP, eid), BM_OK);
    expected.fill(0x33U);
    EXPECT_EQ(eid, expected);
}

TEST_F(UrmaTopoManagerTest, ParseEdgeList_NoMatch)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.localId_ = 99;
    std::string json = R"({"edge_list":[{"local_a":0,"local_b":1}]})";
    std::vector<Edge> edges;
    EXPECT_EQ(mgr.ParseEdgeList(json, edges), BM_OK);
    EXPECT_EQ(edges.size(), 0U);
}

TEST_F(UrmaTopoManagerTest, ParseEdgeList_KeyMissing)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.localId_ = 0;
    std::string json = R"({"other":[]})";
    std::vector<Edge> edges;
    EXPECT_EQ(mgr.ParseEdgeList(json, edges), BM_OK);
    EXPECT_EQ(edges.size(), 0U);
}

// ============================ Manager: GetPeer2NetEid ============================

TEST_F(UrmaTopoManagerTest, GetPeer2NetEid_SelectsLargestAddressWithOverlappingPorts)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    auto &rank = AddLocalRank(mgr);
    AddEidAddr(rank, kMeshLayer, "11111111111111111111111111111111", {"1/1", "1/2", "1/3", "1/4", "1/5", "1/6"});
    AddEidAddr(rank, kCtpLayer, "22222222222222222222222222222222", {"1/1", "1/2"});
    AddEidAddr(rank, kCtpLayer, "33333333333333333333333333333333", {"1/2", "1/8", "1/9"});
    AddEidAddr(rank, kCtpLayer, "44444444444444444444444444444444", {"1/3", "1/4", "1/5", "1/6", "1/7"});
    AddPeer2NetEdge(mgr, kCtpLayer, "UB_CTP", {"1/1", "1/2"});
    Edge ignored{};
    ignored.netLayer = kCtpLayer;
    ignored.linkType = "PEER2PEER";
    ignored.localA = kLocalDeviceId;
    ignored.protocols = {"UB_CTP"};
    ignored.position = "DEVICE";
    ignored.localAPorts = {"1/3"};
    mgr.topoInfo_.edgeList.push_back(std::move(ignored));
    Edge nonDevice = mgr.topoInfo_.edgeList.back();
    nonDevice.linkType = "PEER2NET";
    nonDevice.position = "SERVER";
    mgr.topoInfo_.edgeList.push_back(std::move(nonDevice));
    Edge wrongProtocol = mgr.topoInfo_.edgeList.back();
    wrongProtocol.position = "DEVICE";
    wrongProtocol.protocols = {"UB_RTP"};
    mgr.topoInfo_.edgeList.push_back(std::move(wrongProtocol));
    Edge wrongLocal = mgr.topoInfo_.edgeList.back();
    wrongLocal.protocols = {"UB_CTP"};
    wrongLocal.localA = kLocalDeviceId + 1;
    mgr.topoInfo_.edgeList.push_back(std::move(wrongLocal));
    std::array<uint8_t, COMM_ADDR_EID_LEN> out{};
    EXPECT_EQ(mgr.GetPeer2NetEid(COMM_PROTOCOL_UBC_CTP, out), BM_OK);
    std::array<uint8_t, COMM_ADDR_EID_LEN> expected{};
    expected.fill(0x33U);
    EXPECT_EQ(out, expected);
}

TEST_F(UrmaTopoManagerTest, GetPeer2NetEid_MeshWithoutClosAddressFails)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    auto &rank = AddLocalRank(mgr);
    AddEidAddr(rank, kMeshLayer, "11111111111111111111111111111111", {"0/0"});
    AddPeer2NetEdge(mgr, kMeshLayer, "UB_CTP", {"0/1"});
    AddPeer2NetEdge(mgr, kCtpLayer, "UB_CTP", {"1/1"});
    std::array<uint8_t, COMM_ADDR_EID_LEN> out{};
    EXPECT_NE(mgr.GetPeer2NetEid(COMM_PROTOCOL_UBC_CTP, out), BM_OK);
}

TEST_F(UrmaTopoManagerTest, GetPeer2NetEid_SeparatesCtpAndRtpLayers)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    auto &rank = AddLocalRank(mgr);
    AddEidAddr(rank, kCtpLayer, "11111111111111111111111111111111", {"1/0"});
    AddEidAddr(rank, kRtpLayer, "0000000000080a80001000000a0f6106", {"2/0"});
    AddPeer2NetEdge(mgr, kCtpLayer, "UB_CTP", {"1/0"});
    AddPeer2NetEdge(mgr, kRtpLayer, "UB_RTP", {"2/0"});
    std::array<uint8_t, COMM_ADDR_EID_LEN> ctp{};
    std::array<uint8_t, COMM_ADDR_EID_LEN> rtp{};
    EXPECT_EQ(mgr.GetPeer2NetEid(COMM_PROTOCOL_UBC_CTP, ctp), BM_OK);
    EXPECT_EQ(mgr.GetPeer2NetEid(COMM_PROTOCOL_UB_RTP, rtp), BM_OK);
    std::array<uint8_t, COMM_ADDR_EID_LEN> expectedCtp{};
    expectedCtp.fill(0x11U);
    const std::array<uint8_t, COMM_ADDR_EID_LEN> expectedRtp = {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x08U, 0x0AU, 0x80U,
                                                                0x00U, 0x10U, 0x00U, 0x00U, 0x0AU, 0x0FU, 0x61U, 0x06U};
    EXPECT_EQ(ctp, expectedCtp);
    EXPECT_EQ(rtp, expectedRtp);
}

// ============================ Manager: ParsePeerList ============================

TEST_F(UrmaTopoManagerTest, ParsePeerList_Basic)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::string json = R"({"peer_list":[{"local_id":0},{"local_id":1},{"local_id":2}]})";
    std::vector<int> peers;
    EXPECT_EQ(mgr.ParsePeerList(json, peers), BM_OK);
    EXPECT_EQ(peers.size(), 3U);
    EXPECT_EQ(peers[0], 0);
    EXPECT_EQ(peers[1], 1);
    EXPECT_EQ(peers[2], 2);
}

TEST_F(UrmaTopoManagerTest, ParsePeerList_Empty)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::string json = R"({"peer_list":[]})";
    std::vector<int> peers;
    EXPECT_EQ(mgr.ParsePeerList(json, peers), BM_OK);
    EXPECT_EQ(peers.size(), 0U);
}

TEST_F(UrmaTopoManagerTest, ParsePeerList_KeyMissing)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::string json = R"({"other":[]})";
    std::vector<int> peers;
    EXPECT_EQ(mgr.ParsePeerList(json, peers), BM_OK);
    EXPECT_EQ(peers.size(), 0U);
}

// ============================ Manager: ParseRankAddrObj / ParseRankObj ============================

TEST_F(UrmaTopoManagerTest, ParseRankAddrObj_Basic)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::string json = R"({"addr_type":"EID","addr":"aabb","ports":["0/0"],"plane_id":"plane_0"})";
    RankAddr addr;
    mgr.ParseRankAddrObj(json, addr);
    EXPECT_EQ(addr.addrType, "EID");
    EXPECT_EQ(addr.addr, "aabb");
    ASSERT_EQ(addr.ports.size(), 1U);
    EXPECT_EQ(addr.ports[0], "0/0");
    EXPECT_EQ(addr.planeId, "plane_0");
}

TEST_F(UrmaTopoManagerTest, ParseRankObj_Basic)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::string json = R"({"device_id":7,"local_id":8,"level_list":[]})";
    Rank rank;
    mgr.ParseRankObj(json, rank);
    EXPECT_EQ(rank.deviceId, 7);
    EXPECT_EQ(rank.localId, 8);
    EXPECT_EQ(rank.levelList.size(), 0U);
}

// ============================ Manager: ParseTopoInfo ============================

TEST_F(UrmaTopoManagerTest, ParseTopoInfo_Basic)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::string json = R"({"version":"1.0","hardwareType":"Atlas 850","peer_count":2,)"
                       R"("peer_list":[{"local_id":0},{"local_id":1}],"edge_count":0,"edge_list":[]})";
    TopoInfo info;
    EXPECT_EQ(mgr.ParseTopoInfo(json, info), BM_OK);
    EXPECT_EQ(info.version, "1.0");
    EXPECT_EQ(info.hardwareType, "Atlas 850");
    EXPECT_EQ(info.peerCount, 2);
    EXPECT_EQ(info.peerList.size(), 2U);
    EXPECT_EQ(info.edgeCount, 0);
    EXPECT_EQ(info.edgeList.size(), 0U);
}

// ============================ Additional JSON parser edge cases ============================

TEST(UrmaTopoJsonParserTest, SkipString_WithEscapes)
{
    std::string json = R"("a\nb\tc")";
    auto r = dev::SkipString(json.data(), json.data() + json.size(), 0, 256);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, json.size());
}

TEST(UrmaTopoJsonParserTest, SkipString_UnicodeEscape)
{
    std::string json = R"("\u00e9")";
    auto r = dev::SkipString(json.data(), json.data() + json.size(), 0, 256);
    EXPECT_EQ(r.result, BM_OK);
}

TEST(UrmaTopoJsonParserTest, SkipString_Unterminated)
{
    std::string json = "\"unterminated";
    auto r = dev::SkipString(json.data(), json.data() + json.size(), 0, 256);
    EXPECT_NE(r.result, BM_OK);
}

TEST(UrmaTopoJsonParserTest, SkipString_NotStartingWithQuote)
{
    std::string json = "abc";
    auto r = dev::SkipString(json.data(), json.data() + json.size(), 0, 256);
    EXPECT_NE(r.result, BM_OK);
}

TEST(UrmaTopoJsonParserTest, SkipString_ControlChar)
{
    std::string json = "\"a\x01b\"";
    auto r = dev::SkipString(json.data(), json.data() + json.size(), 0, 256);
    EXPECT_NE(r.result, BM_OK);
}

TEST(UrmaTopoJsonParserTest, SkipNumber_Float)
{
    std::string json = "3.14,";
    auto r = dev::SkipNumber(json.data(), json.data() + json.size(), 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 4U);
}

TEST(UrmaTopoJsonParserTest, SkipNumber_Exponent)
{
    std::string json = "1e10,";
    auto r = dev::SkipNumber(json.data(), json.data() + json.size(), 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 4U);
}

TEST(UrmaTopoJsonParserTest, SkipNumber_NegativeExponent)
{
    std::string json = "-5e-2,";
    auto r = dev::SkipNumber(json.data(), json.data() + json.size(), 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 5U);
}

TEST(UrmaTopoJsonParserTest, SkipNumber_Invalid)
{
    std::string json = "abc";
    auto r = dev::SkipNumber(json.data(), json.data() + json.size(), 0);
    EXPECT_NE(r.result, BM_OK);
}

TEST(UrmaTopoJsonParserTest, SkipNumber_LeadingZero)
{
    std::string json = "0123";
    auto r = dev::SkipNumber(json.data(), json.data() + json.size(), 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 1U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_EmptyObject)
{
    std::string json = "{}";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 2U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_EmptyArray)
{
    std::string json = "[]";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 2U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_NestedObject)
{
    std::string json = R"({"a":{"b":1}},"x")";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 13U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_NestedArray)
{
    std::string json = R"([[1,2],[3,4]],"x")";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_EQ(r.result, BM_OK);
    EXPECT_EQ(r.offset, 13U);
}

TEST(UrmaTopoJsonParserTest, SkipValue_InvalidChar)
{
    std::string json = "xyz";
    auto r = dev::SkipValue(json.data(), json.data() + json.size(), 0, 0);
    EXPECT_NE(r.result, BM_OK);
}

TEST(UrmaTopoJsonParserTest, ExtractStrField_MalformedValue)
{
    std::string json = R"({"name":123})";
    std::string out;
    EXPECT_NE(dev::ExtractStrField(json, "\"name\"", out), BM_OK);
}

TEST(UrmaTopoJsonParserTest, ExtractStrField_NoColon)
{
    std::string json = R"({"name" "hello"})";
    std::string out;
    EXPECT_NE(dev::ExtractStrField(json, "\"name\"", out), BM_OK);
}

TEST(UrmaTopoJsonParserTest, ExtractIntField_Zero)
{
    std::string json = R"({"val":0})";
    int32_t out = -1;
    EXPECT_EQ(dev::ExtractIntField(json, "\"val\"", out), BM_OK);
    EXPECT_EQ(out, 0);
}

TEST(UrmaTopoJsonParserTest, ParseJsonArrayHeader_EmptyArray)
{
    std::string json = R"({"items":[]})";
    size_t pos = 0;
    EXPECT_EQ(dev::ParseJsonArrayHeader(json, "\"items\"", pos), BM_OK);
}

TEST(UrmaTopoJsonParserTest, ParseStrArray_SingleItem)
{
    std::string json = R"({"ports":["0/0"]})";
    std::vector<std::string> out;
    EXPECT_EQ(dev::ParseStrArray(json, "\"ports\"", out), BM_OK);
    EXPECT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0], "0/0");
}

TEST(UrmaTopoJsonParserTest, ParseStrArray_Malformed)
{
    std::string json = R"({"ports":[123]})";
    std::vector<std::string> out;
    EXPECT_NE(dev::ParseStrArray(json, "\"ports\"", out), BM_OK);
}

TEST(UrmaTopoJsonParserTest, FindQuotedKeyInRange_Found)
{
    std::string json = R"({"local_id":42})";
    std::string key = "\"local_id\"";
    size_t pos = dev::FindQuotedKeyInRange(json.data(), 0, json.size(), key.c_str(), key.size());
    EXPECT_NE(pos, SIZE_MAX);
}

TEST(UrmaTopoJsonParserTest, FindQuotedKeyInRange_NotFound)
{
    std::string json = R"({"other":42})";
    size_t pos = dev::FindQuotedKeyInRange(json.data(), 0, json.size(), "\"local_id\"", 11);
    EXPECT_EQ(pos, SIZE_MAX);
}

TEST(UrmaTopoJsonParserTest, ExtractIntInRange_Zero)
{
    std::string json = R"({"local_id":0})";
    int32_t out = -1;
    EXPECT_EQ(dev::ExtractIntInRange(json.data(), 0, json.size(), "\"local_id\"", out), BM_OK);
    EXPECT_EQ(out, 0);
}

// ============================ Additional manager tests ============================

TEST_F(UrmaTopoManagerTest, ParseRankListFull_WithLevels)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.phyDeviceId_ = 2;
    std::string json =
        R"({"rank_list":[{"device_id":2,"local_id":2,"level_list":[{"net_layer":0,"net_type":"TOPO_FILE_DESC","rank_addr_list":[]}]}]})";
    std::vector<Rank> ranks;
    EXPECT_EQ(mgr.ParseRankListFull(json, ranks), BM_OK);
    ASSERT_EQ(ranks.size(), 1U);
    EXPECT_EQ(ranks[0].deviceId, 2);
    ASSERT_EQ(ranks[0].levelList.size(), 1U);
    EXPECT_EQ(ranks[0].levelList[0].netLayer, 0);
}

TEST_F(UrmaTopoManagerTest, ParseEdgeList_WithAllFields)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.localId_ = 0;
    std::string json = R"({"edge_list":[)"
                       R"({"net_layer":0,"link_type":"PEER2PEER","topo_type":"mesh",)"
                       R"("topo_instance_id":1,"topo_attr":"attr","local_a":0,"local_b":3,)"
                       R"("local_a_ports":["0/0","0/1"],"local_b_ports":["1/0","1/1"],)"
                       R"("protocols":["HCCS-4P"],"position":"P2P"}]})";
    std::vector<Edge> edges;
    EXPECT_EQ(mgr.ParseEdgeList(json, edges), BM_OK);
    ASSERT_EQ(edges.size(), 1U);
    EXPECT_EQ(edges[0].linkType, "PEER2PEER");
    EXPECT_EQ(edges[0].topoType, "mesh");
    EXPECT_EQ(edges[0].localB, 3);
    EXPECT_EQ(edges[0].position, "P2P");
    ASSERT_EQ(edges[0].protocols.size(), 1U);
    EXPECT_EQ(edges[0].protocols[0], "HCCS-4P");
}

TEST_F(UrmaTopoManagerTest, ParseEdgeList_MalformedJson)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.localId_ = 0;
    std::string json = R"({"edge_list":[123]})";
    std::vector<Edge> edges;
    EXPECT_NE(mgr.ParseEdgeList(json, edges), BM_OK);
}

TEST_F(UrmaTopoManagerTest, GetPeer2NetEid_UnsupportedProtocol)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::array<uint8_t, COMM_ADDR_EID_LEN> out{};
    EXPECT_EQ(mgr.GetPeer2NetEid(COMM_PROTOCOL_UBC_TP, out), BM_NOT_SUPPORTED);
}

TEST_F(UrmaTopoManagerTest, GetPeer2NetEid_NoLocalRankFails)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.phyDeviceId_ = kLocalDeviceId;
    mgr.localId_ = kLocalDeviceId;
    Rank rank{};
    rank.deviceId = kLocalDeviceId + 1;
    mgr.rootInfo_.rankList.push_back(std::move(rank));
    AddPeer2NetEdge(mgr, kCtpLayer, "UB_CTP", {"1/0"});
    std::array<uint8_t, COMM_ADDR_EID_LEN> out{};
    EXPECT_NE(mgr.GetPeer2NetEid(COMM_PROTOCOL_UBC_CTP, out), BM_OK);
}

TEST_F(UrmaTopoManagerTest, GetPeer2NetEid_EmptyPeer2NetPortsFail)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    auto &rank = AddLocalRank(mgr);
    AddEidAddr(rank, kCtpLayer, "11111111111111111111111111111111", {"1/0"});
    AddPeer2NetEdge(mgr, kCtpLayer, "UB_CTP", {});
    std::array<uint8_t, COMM_ADDR_EID_LEN> out{};
    EXPECT_NE(mgr.GetPeer2NetEid(COMM_PROTOCOL_UBC_CTP, out), BM_OK);
}

TEST_F(UrmaTopoManagerTest, GetPeer2NetEid_InvalidLengthFails)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    auto &rank = AddLocalRank(mgr);
    AddEidAddr(rank, kCtpLayer, "1111111111111111111111111111111", {"1/0"});
    AddPeer2NetEdge(mgr, kCtpLayer, "UB_CTP", {"1/0"});
    std::array<uint8_t, COMM_ADDR_EID_LEN> out{};
    EXPECT_EQ(mgr.GetPeer2NetEid(COMM_PROTOCOL_UBC_CTP, out), BM_INVALID_PARAM);
}

TEST_F(UrmaTopoManagerTest, GetPeer2NetEid_NonHexValueFails)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    auto &rank = AddLocalRank(mgr);
    AddEidAddr(rank, kCtpLayer, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", {"1/0"});
    AddPeer2NetEdge(mgr, kCtpLayer, "UB_CTP", {"1/0"});
    std::array<uint8_t, COMM_ADDR_EID_LEN> out{};
    EXPECT_EQ(mgr.GetPeer2NetEid(COMM_PROTOCOL_UBC_CTP, out), BM_INVALID_PARAM);
}

TEST_F(UrmaTopoManagerTest, GetPeer2NetEid_RtpRejectsCtpEid)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    auto &rank = AddLocalRank(mgr);
    AddEidAddr(rank, kRtpLayer, "11111111111111111111111111111111", {"2/0"});
    AddPeer2NetEdge(mgr, kRtpLayer, "UB_RTP", {"2/0"});
    std::array<uint8_t, COMM_ADDR_EID_LEN> out{};
    constexpr uint8_t kSentinel = 0xA5U;
    out.fill(kSentinel);
    const auto original = out;
    EXPECT_EQ(mgr.GetPeer2NetEid(COMM_PROTOCOL_UB_RTP, out), BM_INVALID_PARAM);
    EXPECT_EQ(out, original);
}

TEST_F(UrmaTopoManagerTest, Uninitialize_ClearsAllState)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    mgr.phyDeviceId_ = 5;
    mgr.localId_ = 3;
    Rank r;
    r.deviceId = 1;
    mgr.rootInfo_.rankList.push_back(std::move(r));
    Edge e;
    e.localA = 0;
    mgr.topoInfo_.edgeList.push_back(std::move(e));
    mgr.Uninitialize();
    EXPECT_EQ(mgr.phyDeviceId_, -1);
    EXPECT_EQ(mgr.localId_, -1);
    EXPECT_EQ(mgr.rootInfo_.rankList.size(), 0U);
    EXPECT_EQ(mgr.topoInfo_.edgeList.size(), 0U);
}

TEST_F(UrmaTopoManagerTest, ParseRankAddrList_Basic)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::string json = R"({"rank_addr_list":[{"addr_type":"EID","addr":"aabb","ports":["0/0"],"plane_id":"p0"},)"
                       R"({"addr_type":"IPV4","addr":"1.2.3.4","ports":["d2h"],"plane_id":"p1"}]})";
    std::vector<RankAddr> addrs;
    EXPECT_EQ(mgr.ParseRankAddrList(json, addrs), BM_OK);
    EXPECT_EQ(addrs.size(), 2U);
    EXPECT_EQ(addrs[0].addrType, "EID");
    EXPECT_EQ(addrs[1].addrType, "IPV4");
}

TEST_F(UrmaTopoManagerTest, ParseRankAddrList_KeyMissing)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::string json = R"({"other":[]})";
    std::vector<RankAddr> addrs;
    EXPECT_EQ(mgr.ParseRankAddrList(json, addrs), BM_OK);
    EXPECT_EQ(addrs.size(), 0U);
}

TEST_F(UrmaTopoManagerTest, ParseLevelObj_Basic)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::string json = R"({"net_layer":1,"net_instance_id":"sp_1","net_type":"CLOS",)"
                       R"("net_attr":"attr","rank_addr_list":[]})";
    Level level;
    mgr.ParseLevelObj(json, level);
    EXPECT_EQ(level.netLayer, 1);
    EXPECT_EQ(level.netInstanceId, "sp_1");
    EXPECT_EQ(level.netType, "CLOS");
    EXPECT_EQ(level.netAttr, "attr");
}

TEST_F(UrmaTopoManagerTest, ParseLevelList_Basic)
{
    auto &mgr = UrmaTopoManager::GetInstance();
    std::string json = R"({"level_list":[{"net_layer":0,"net_type":"TOPO_FILE_DESC","rank_addr_list":[]},)";
    json += R"({"net_layer":3,"net_type":"CLOS","rank_addr_list":[]}]})";
    std::vector<Level> levels;
    EXPECT_EQ(mgr.ParseLevelList(json, levels), BM_OK);
    EXPECT_EQ(levels.size(), 2U);
    EXPECT_EQ(levels[0].netLayer, 0);
    EXPECT_EQ(levels[1].netLayer, 3);
}

// ============================ HAL mock tests ============================

int32_t MockRtGetDeviceInfo(uint32_t devId, int32_t moduleType, int32_t infoType, int64_t *value)
{
    (void)devId;
    (void)moduleType;
    if (value == nullptr) {
        return -1;
    }
    switch (infoType) {
        case 29:
            *value = 1;
            break;
        case 27:
            *value = 2;
            break;
        case 48:
            *value = 3;
            break;
        case 49:
            *value = 0;
            break;
        default:
            *value = 0;
            break;
    }
    return 0;
}

int32_t MockRtGetDeviceInfoFail(uint32_t devId, int32_t moduleType, int32_t infoType, int64_t *value)
{
    (void)devId;
    (void)moduleType;
    (void)infoType;
    (void)value;
    return -1;
}

int MockDcmiGetUrmaDeviceCnt(int npuId, unsigned int *devCnt)
{
    (void)npuId;
    if (devCnt != nullptr) {
        *devCnt = 2;
    }
    return 0;
}

int MockDcmiGetUrmaDeviceCntZero(int npuId, unsigned int *devCnt)
{
    (void)npuId;
    if (devCnt != nullptr) {
        *devCnt = 0;
    }
    return 0;
}

int MockDcmiGetUrmaDeviceCntFail(int npuId, unsigned int *devCnt)
{
    (void)npuId;
    (void)devCnt;
    return -1;
}

int g_mockEidCnt = 1;

int MockDcmiGetEidList(int npuId, int urmaDevIndex, dcmi_urma_eid_info *eidList, int *eidCnt)
{
    (void)npuId;
    (void)urmaDevIndex;
    if (eidList != nullptr && eidCnt != nullptr && *eidCnt >= 1) {
        dcmi_urma_eid eid;
        eid.raw[0] = 0xAA;
        eid.raw[5] = 0x0A;
        eid.raw[6] = 0x33;
        eid.raw[15] = 0xBB;
        eidList[0].eid = eid;
        eidList[0].eid_index = 0;
        *eidCnt = g_mockEidCnt;
    }
    return 0;
}

int MockDcmiGetEidListFail(int npuId, int urmaDevIndex, dcmi_urma_eid_info *eidList, int *eidCnt)
{
    (void)npuId;
    (void)urmaDevIndex;
    (void)eidList;
    (void)eidCnt;
    return -1;
}

void MockHalGvaGetDriverInstallPath(std::string &driverInstallPath)
{
    driverInstallPath = "/usr/local/Ascend";
}

int32_t MockTopologyRtGetDeviceInfo(uint32_t devId, int32_t moduleType, int32_t infoType, int64_t *value)
{
    constexpr int64_t kServerIndex = 10;
    const auto ret = MockRtGetDeviceInfo(devId, moduleType, infoType, value);
    if (ret == BM_OK && infoType == INFO_TYPE_SERVER_ID) {
        *value = kServerIndex;
    } else if (ret == BM_OK && infoType == INFO_TYPE_SPOD_TYPE) {
        *value = TOPO_TYPE_SERVER_16FM;
    }
    return ret;
}

int MockTopologyEidList(int npuId, int urmaDevIndex, dcmi_urma_eid_info *eidList, int *eidCnt)
{
    constexpr size_t kDiePortByte = 5U;
    constexpr size_t kFeByte = 6U;
    constexpr uint8_t kDieOneBits = 0x40U;
    constexpr uint8_t kPortGroupId = 0x3FU;
    constexpr uint8_t kDieZeroFe = 3U;
    constexpr uint8_t kDieOneFe = 2U;
    (void)npuId;
    const std::vector<uint8_t> ports = urmaDevIndex == 0 ? std::vector<uint8_t>{0U, 1U, kPortGroupId}
                                                         : std::vector<uint8_t>{0U, 1U, 2U, 3U, 5U, 6U, kPortGroupId};
    if (eidList == nullptr || eidCnt == nullptr || *eidCnt < static_cast<int>(ports.size())) {
        return BM_INVALID_PARAM;
    }
    for (size_t i = 0; i < ports.size(); ++i) {
        eidList[i] = {};
        eidList[i].eid.raw[kDiePortByte] = ports[i] | (urmaDevIndex == 0 ? 0U : kDieOneBits);
        eidList[i].eid.raw[kFeByte] = urmaDevIndex == 0 ? kDieZeroFe : kDieOneFe;
        eidList[i].eid_index = static_cast<uint32_t>(i);
    }
    *eidCnt = static_cast<int>(ports.size());
    return BM_OK;
}

class UrmaTopoCommonMockTest : public testing::Test {
protected:
    void SetUp() override
    {
        GlobalMockObject::reset();
    }
    void TearDown() override
    {
        GlobalMockObject::verify();
    }
};

TEST_F(UrmaTopoCommonMockTest, GetSpodInfo_Success)
{
    MOCKER(&DlAclApi::RtGetDeviceInfo).stubs().will(invoke(MockRtGetDeviceInfo));
    dcmi_spod_info spod{};
    EXPECT_EQ(GetSpodInfo(0, spod), BM_OK);
    EXPECT_EQ(spod.super_pod_id, 1U);
    EXPECT_EQ(spod.server_index, 2U);
    EXPECT_EQ(spod.chassis_id, 3U);
    EXPECT_EQ(spod.super_pod_type, 0U);
}

TEST_F(UrmaTopoCommonMockTest, GetSpodInfo_FailSpodId)
{
    MOCKER(&DlAclApi::RtGetDeviceInfo).stubs().will(invoke(MockRtGetDeviceInfoFail));
    dcmi_spod_info spod{};
    EXPECT_NE(GetSpodInfo(0, spod), BM_OK);
}

TEST_F(UrmaTopoCommonMockTest, GetTopoFilePath_Server16FM)
{
    MOCKER(&HalGvaGetDriverInstallPath).stubs().will(invoke(MockHalGvaGetDriverInstallPath));
    std::string path;
    EXPECT_EQ(UrmaTopoProduct::GetTopoFilePath(MAIN_BOARD_ID_SERVER_8PMESH_UBOE, 3, path), BM_OK);
    EXPECT_NE(path.find("atlas_850_2"), std::string::npos);
}

TEST_F(UrmaTopoCommonMockTest, GetTopoFilePath_Pod)
{
    MOCKER(&HalGvaGetDriverInstallPath).stubs().will(invoke(MockHalGvaGetDriverInstallPath));
    std::string path;
    EXPECT_EQ(UrmaTopoProduct::GetTopoFilePath(MAIN_BOARD_ID_POD, 99, path), BM_OK);
    EXPECT_NE(path.find("atlas_950_1"), std::string::npos);
}

TEST_F(UrmaTopoCommonMockTest, GetTopoFilePath_PodFlex)
{
    MOCKER(&HalGvaGetDriverInstallPath).stubs().will(invoke(MockHalGvaGetDriverInstallPath));
    std::string path;
    EXPECT_EQ(UrmaTopoProduct::GetTopoFilePath(MAIN_BOARD_ID_POD_FLEX_RTP, 99, path), BM_OK);
    EXPECT_NE(path.find("atlas_950_2"), std::string::npos);
}

TEST_F(UrmaTopoCommonMockTest, GetTopoFilePath_UnknownBoard)
{
    MOCKER(&HalGvaGetDriverInstallPath).stubs().will(invoke(MockHalGvaGetDriverInstallPath));
    std::string path;
    EXPECT_NE(UrmaTopoProduct::GetTopoFilePath(0xFFFF, 99, path), BM_OK);
}

TEST_F(UrmaTopoCommonMockTest, GetUrmaEntityList_Success)
{
    MOCKER(&DlHalApi::DcmiGetUrmaDeviceCnt).stubs().will(invoke(MockDcmiGetUrmaDeviceCnt));
    MOCKER(&DlHalApi::DcmiGetEidListByUrmaDevIndex).stubs().will(invoke(MockDcmiGetEidList));
    g_mockEidCnt = 1;
    urmaEntityList ueList{};
    EXPECT_EQ(GetUrmaEntityList(0, ueList), BM_OK);
    EXPECT_EQ(ueList.ueNum, 2U);
    EXPECT_EQ(ueList.ueList[0].eidNum, 1U);
    EXPECT_EQ(ueList.ueList[0].eidList[0].eid.raw[0], 0xAA);
}

TEST_F(UrmaTopoCommonMockTest, GetUrmaEntityList_DevCntFail)
{
    MOCKER(&DlHalApi::DcmiGetUrmaDeviceCnt).stubs().will(invoke(MockDcmiGetUrmaDeviceCntFail));
    urmaEntityList ueList{};
    EXPECT_NE(GetUrmaEntityList(0, ueList), BM_OK);
}

TEST_F(UrmaTopoCommonMockTest, GetUrmaEntityList_ZeroDev)
{
    MOCKER(&DlHalApi::DcmiGetUrmaDeviceCnt).stubs().will(invoke(MockDcmiGetUrmaDeviceCntZero));
    urmaEntityList ueList{};
    EXPECT_EQ(GetUrmaEntityList(0, ueList), BM_OK);
    EXPECT_EQ(ueList.ueNum, 0U);
}

TEST_F(UrmaTopoCommonMockTest, GetUrmaEntityList_EidListFail)
{
    MOCKER(&DlHalApi::DcmiGetUrmaDeviceCnt).stubs().will(invoke(MockDcmiGetUrmaDeviceCnt));
    MOCKER(&DlHalApi::DcmiGetEidListByUrmaDevIndex).stubs().will(invoke(MockDcmiGetEidListFail));
    urmaEntityList ueList{};
    EXPECT_EQ(GetUrmaEntityList(0, ueList), BM_OK);
    EXPECT_EQ(ueList.ueNum, 2U);
    EXPECT_EQ(ueList.ueList[0].eidNum, 0U);
}

TEST_F(UrmaTopoCommonMockTest, Server16FmRootInfoPreservesRankAndClosPortGroups)
{
    constexpr int32_t kDeviceId = 0;
    constexpr int32_t kPhysicalId = 3;
    constexpr int32_t kExpectedLocalId = 83;
    constexpr size_t kRankCount = 1U;
    constexpr size_t kLayerCount = 2U;
    constexpr size_t kMeshPortCount = 6U;
    MOCKER(&DlAclApi::RtGetDeviceInfo).stubs().will(invoke(MockTopologyRtGetDeviceInfo));
    MOCKER(&DlHalApi::DcmiGetUrmaDeviceCnt).stubs().will(invoke(MockDcmiGetUrmaDeviceCnt));
    MOCKER(&DlHalApi::DcmiGetEidListByUrmaDevIndex).stubs().will(invoke(MockTopologyEidList));
    MOCKER(&HalGvaGetDriverInstallPath).stubs().will(invoke(MockHalGvaGetDriverInstallPath));
    auto product = UrmaTopoProduct::GetUrmaTopoProduct(kDeviceId, kDeviceId, kPhysicalId, MAIN_BOARD_ID_SERVER_TYPE1);
    ASSERT_NE(product, nullptr);
    int32_t localId = -1;
    ASSERT_EQ(product->GetLocalId(localId), BM_OK);
    EXPECT_EQ(localId, kExpectedLocalId);
    RootInfo root;
    ASSERT_EQ(product->GetRootInfo(root), BM_OK);
    EXPECT_EQ(root.version, "2.0");
    EXPECT_EQ(root.topoFilePath, "/usr/local/Ascend/driver/topo/950/atlas_850_2.json");
    EXPECT_EQ(root.rankCount, static_cast<int>(kRankCount));
    ASSERT_EQ(root.rankList.size(), kRankCount);
    const auto &rank = root.rankList.front();
    EXPECT_EQ(rank.deviceId, kPhysicalId);
    EXPECT_EQ(rank.localId, localId);
    ASSERT_EQ(rank.levelList.size(), kLayerCount);
    EXPECT_EQ(rank.levelList[0U].netType, NET_TYPE_TOPO_FILE_DESC);
    ASSERT_EQ(rank.levelList[0U].rankAddrList.size(), kMeshPortCount);
    EXPECT_EQ(rank.levelList[0U].rankAddrList.front().ports, (std::vector<std::string>{"1/0"}));
    const auto &clos = rank.levelList[1U];
    EXPECT_EQ(clos.netType, NET_TYPE_CLOS);
    EXPECT_EQ(clos.netInstanceId, "sp_1");
    ASSERT_EQ(clos.rankAddrList.size(), kLayerCount);
    EXPECT_EQ(clos.rankAddrList[0U].ports, (std::vector<std::string>{"0/0", "0/1"}));
    EXPECT_EQ(clos.rankAddrList[0U].planeId, "plane_clos_0");
    EXPECT_EQ(clos.rankAddrList[1U].ports, (std::vector<std::string>{"1/0", "1/1", "1/2", "1/3", "1/5", "1/6"}));
    EXPECT_EQ(clos.rankAddrList[1U].planeId, "plane_clos_1");
}

TEST_F(UrmaTopoCommonMockTest, PodRootInfoWrapsServerIndexAndPrioritizesClosPortGroup)
{
    constexpr int32_t kDeviceId = 0;
    constexpr int32_t kPhysicalId = 3;
    constexpr int32_t kExpectedLocalId = 19;
    constexpr size_t kRankCount = 1U;
    constexpr size_t kLayerCount = 2U;
    constexpr size_t kMeshPortCount = 2U;
    MOCKER(&DlAclApi::RtGetDeviceInfo).stubs().will(invoke(MockTopologyRtGetDeviceInfo));
    MOCKER(&DlHalApi::DcmiGetUrmaDeviceCnt).stubs().will(invoke(MockDcmiGetUrmaDeviceCnt));
    MOCKER(&DlHalApi::DcmiGetEidListByUrmaDevIndex).stubs().will(invoke(MockTopologyEidList));
    MOCKER(&HalGvaGetDriverInstallPath).stubs().will(invoke(MockHalGvaGetDriverInstallPath));
    auto product = UrmaTopoProduct::GetUrmaTopoProduct(kDeviceId, kDeviceId, kPhysicalId, MAIN_BOARD_ID_POD);
    ASSERT_NE(product, nullptr);
    int32_t localId = -1;
    ASSERT_EQ(product->GetLocalId(localId), BM_OK);
    EXPECT_EQ(localId, kExpectedLocalId);
    RootInfo root;
    ASSERT_EQ(product->GetRootInfo(root), BM_OK);
    EXPECT_EQ(root.version, "2.0");
    EXPECT_EQ(root.topoFilePath, "/usr/local/Ascend/driver/topo/950/atlas_950_1.json");
    EXPECT_EQ(root.rankCount, static_cast<int>(kRankCount));
    ASSERT_EQ(root.rankList.size(), kRankCount);
    const auto &rank = root.rankList.front();
    EXPECT_EQ(rank.deviceId, kPhysicalId);
    EXPECT_EQ(rank.localId, localId);
    ASSERT_EQ(rank.levelList.size(), kLayerCount);
    const auto &mesh = rank.levelList[0U];
    EXPECT_EQ(mesh.netType, NET_TYPE_MESH);
    EXPECT_EQ(mesh.netInstanceId, "sp1_srv10");
    ASSERT_EQ(mesh.rankAddrList.size(), kMeshPortCount);
    EXPECT_EQ(mesh.rankAddrList[0U].ports, (std::vector<std::string>{"0/0"}));
    EXPECT_EQ(mesh.rankAddrList[1U].ports, (std::vector<std::string>{"0/1"}));
    const auto &clos = rank.levelList[1U];
    EXPECT_EQ(clos.netType, NET_TYPE_CLOS);
    EXPECT_EQ(clos.netInstanceId, "superpod_1");
    ASSERT_EQ(clos.rankAddrList.size(), kRankCount);
    EXPECT_EQ(clos.rankAddrList.front().ports, (std::vector<std::string>{"1/0", "1/1", "1/2", "1/3", "1/5", "1/6"}));
    EXPECT_EQ(clos.rankAddrList.front().planeId, "plane_0");
}

TEST_F(UrmaTopoCommonMockTest, UBGetMaxEntityId_Basic)
{
    urmaEntityList ueList{};
    ueList.ueNum = 2;
    ueList.ueList[0].eidNum = 1;
    ueList.ueList[0].eidList[0].eid.raw[5] = 0xC0;
    ueList.ueList[0].eidList[0].eid.raw[6] = 0x05;
    ueList.ueList[1].eidNum = 1;
    ueList.ueList[1].eidList[0].eid.raw[5] = 0xC0;
    ueList.ueList[1].eidList[0].eid.raw[6] = 0x0A;
    EXPECT_EQ(UBGetMaxEntityId(ueList, 3), 0x0A);
}

TEST_F(UrmaTopoCommonMockTest, UBGetMaxEntityId_NoMatch)
{
    urmaEntityList ueList{};
    ueList.ueNum = 1;
    ueList.ueList[0].eidNum = 1;
    ueList.ueList[0].eidList[0].eid.raw[5] = 0x00;
    EXPECT_EQ(UBGetMaxEntityId(ueList, 3), -1);
}

TEST_F(UrmaTopoCommonMockTest, UBGetMaxEntityId_EmptyList)
{
    urmaEntityList ueList{};
    EXPECT_EQ(UBGetMaxEntityId(ueList, 0), -1);
}
