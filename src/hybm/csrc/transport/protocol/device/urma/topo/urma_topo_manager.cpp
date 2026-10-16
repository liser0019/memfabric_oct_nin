/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2026. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "hybm_logger.h"
#include "hybm_types.h"
#include "urma_topo_def.h"
#include "urma_topo_manager.h"

#include "dl_acl_api.h"
#include "urma_topo_common.h"
#include "urma_topo_json_parser.h"

namespace ock {
namespace mf {
namespace transport {

namespace device {
namespace {

bool ContainsValue(const std::vector<std::string> &values, const std::string &value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

const char *FindPeer2NetTopoProtocol(CommProtocol protocol)
{
    for (const auto &entry : PEER2NET_PROTOCOL_TABLE) {
        if (entry.protocol == protocol) {
            return entry.topoProtocol;
        }
    }
    return nullptr;
}

using Peer2NetPorts = std::unordered_map<int32_t, std::vector<std::string>>;

Peer2NetPorts CollectPeer2NetPorts(const TopoInfo &topo, int32_t localId, const char *protocol)
{
    Peer2NetPorts layerPorts;
    for (const auto &edge : topo.edgeList) {
        if (edge.localA != localId || edge.linkType != "PEER2NET" || edge.position != "DEVICE" ||
            !ContainsValue(edge.protocols, protocol)) {
            continue;
        }
        auto &ports = layerPorts[edge.netLayer];
        ports.insert(ports.end(), edge.localAPorts.begin(), edge.localAPorts.end());
    }
    return layerPorts;
}

const RankAddr *FindPeer2NetAddr(const Rank &rank, const Peer2NetPorts &layerPorts, int32_t &netLayer)
{
    const RankAddr *bestAddr = nullptr;
    for (const auto &level : rank.levelList) {
        const auto ports = layerPorts.find(level.netLayer);
        if (ports == layerPorts.end()) {
            continue;
        }
        for (const auto &addr : level.rankAddrList) {
            if (addr.addrType != ADDR_TYPE_EID ||
                (bestAddr != nullptr && addr.ports.size() <= bestAddr->ports.size())) {
                continue;
            }
            const bool overlaps = std::any_of(addr.ports.begin(), addr.ports.end(), [&ports](const std::string &port) {
                return ContainsValue(ports->second, port);
            });
            if (overlaps) {
                bestAddr = &addr;
                netLayer = level.netLayer;
            }
        }
    }
    return bestAddr;
}

Result ParsePeer2NetEid(const std::string &hex, CommProtocol protocol, std::array<uint8_t, COMM_ADDR_EID_LEN> &eid)
{
    const bool validHex = std::all_of(hex.begin(), hex.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F');
    });
    if (hex.size() != COMM_ADDR_EID_LEN * 2U || !validHex) {
        BM_LOG_ERROR("TopoManager: invalid peer2net EID format, protocol=" << static_cast<int>(protocol) << " length="
                                                                           << hex.size() << " validHex=" << validHex);
        return BM_INVALID_PARAM;
    }
    std::array<uint8_t, COMM_ADDR_EID_LEN> parsed{};
    HexStrToEid(hex, parsed);
    dcmi_urma_eid urmaEid{};
    std::memcpy(&urmaEid, parsed.data(), parsed.size());
    if (protocol == COMM_PROTOCOL_UB_RTP && !UrmaEidIsUbRtp(urmaEid)) {
        BM_LOG_ERROR("TopoManager: peer2net EID is not RTP, protocol=" << static_cast<int>(protocol));
        return BM_INVALID_PARAM;
    }
    eid = parsed;
    return BM_OK;
}

} // anonymous namespace
} // namespace device

void UrmaTopoManager::ParseRankAddrObj(const std::string &objJson, RankAddr &addr)
{
    (void)device::ExtractStrField(objJson, "\"addr_type\"", addr.addrType);
    (void)device::ExtractStrField(objJson, "\"addr\"", addr.addr);
    (void)device::ExtractStrField(objJson, "\"plane_id\"", addr.planeId);
    (void)device::ParseStrArray(objJson, "\"ports\"", addr.ports);
}

Result UrmaTopoManager::ParseRankAddrList(const std::string &json, std::vector<RankAddr> &addrList)
{
    size_t pos = 0;
    auto ret = device::ParseJsonArrayHeader(json, "\"rank_addr_list\"", pos);
    if (ret != BM_OK) {
        return BM_OK;
    }
    const char *begin = json.data();
    const char *end = begin + json.size();
    bool expectComma = false;
    while (pos < json.size() && json[pos] != ']') {
        if (expectComma) {
            if (json[pos] != ',') {
                return BM_INVALID_PARAM;
            }
            ++pos;
            pos = device::SkipWs(begin, end, pos);
        }
        if (json[pos] != '{') {
            return BM_INVALID_PARAM;
        }
        auto vr = device::SkipValue(begin, end, pos, 0);
        if (vr.result != BM_OK) {
            return vr.result;
        }
        std::string objJson(begin + pos, vr.offset - pos);
        RankAddr addr;
        ParseRankAddrObj(objJson, addr);
        addrList.push_back(std::move(addr));
        pos = vr.offset;
        pos = device::SkipWs(begin, end, pos);
        expectComma = true;
    }
    return BM_OK;
}

void UrmaTopoManager::ParseLevelObj(const std::string &objJson, Level &level)
{
    (void)device::ExtractIntField(objJson, "\"net_layer\"", level.netLayer);
    (void)device::ExtractStrField(objJson, "\"net_instance_id\"", level.netInstanceId);
    (void)device::ExtractStrField(objJson, "\"net_type\"", level.netType);
    (void)device::ExtractStrField(objJson, "\"net_attr\"", level.netAttr);
    (void)ParseRankAddrList(objJson, level.rankAddrList);
}

Result UrmaTopoManager::ParseLevelList(const std::string &json, std::vector<Level> &levelList)
{
    size_t pos = 0;
    auto ret = device::ParseJsonArrayHeader(json, "\"level_list\"", pos);
    if (ret != BM_OK) {
        return BM_OK;
    }
    const char *begin = json.data();
    const char *end = begin + json.size();
    bool expectComma = false;
    while (pos < json.size() && json[pos] != ']') {
        if (expectComma) {
            if (json[pos] != ',') {
                return BM_INVALID_PARAM;
            }
            ++pos;
            pos = device::SkipWs(begin, end, pos);
        }
        if (json[pos] != '{') {
            return BM_INVALID_PARAM;
        }
        auto vr = device::SkipValue(begin, end, pos, 0);
        if (vr.result != BM_OK) {
            return vr.result;
        }
        std::string objJson(begin + pos, vr.offset - pos);
        Level level;
        ParseLevelObj(objJson, level);
        levelList.push_back(std::move(level));
        pos = vr.offset;
        pos = device::SkipWs(begin, end, pos);
        expectComma = true;
    }
    return BM_OK;
}

void UrmaTopoManager::ParseRankObj(const std::string &objJson, Rank &rank)
{
    (void)device::ExtractIntField(objJson, "\"device_id\"", rank.deviceId);
    (void)device::ExtractIntField(objJson, "\"local_id\"", rank.localId);
    (void)ParseLevelList(objJson, rank.levelList);
}

Result UrmaTopoManager::ParseRankListFull(const std::string &json, std::vector<Rank> &rankList)
{
    size_t pos = 0;
    auto ret = device::ParseJsonArrayHeader(json, "\"rank_list\"", pos);
    if (ret != BM_OK) {
        BM_LOG_ERROR("TopoManager: rank_list not found or invalid in rootinfo");
        return ret;
    }
    const char *begin = json.data();
    const char *end = begin + json.size();
    bool expectComma = false;
    while (pos < json.size() && json[pos] != ']') {
        if (expectComma) {
            if (json[pos] != ',') {
                return BM_INVALID_PARAM;
            }
            ++pos;
            pos = device::SkipWs(begin, end, pos);
        }
        if (json[pos] != '{') {
            return BM_INVALID_PARAM;
        }
        auto vr = device::SkipValue(begin, end, pos, 0);
        if (vr.result != BM_OK) {
            return vr.result;
        }
        std::string objJson(begin + pos, vr.offset - pos);
        Rank rank;
        ParseRankObj(objJson, rank);
        if (rank.deviceId == phyDeviceId_) {
            rankList.push_back(std::move(rank));
        }
        pos = vr.offset;
        pos = device::SkipWs(begin, end, pos);
        expectComma = true;
    }
    return BM_OK;
}

Result UrmaTopoManager::ParsePeerList(const std::string &json, std::vector<int> &peerList)
{
    size_t pos = 0;
    auto ret = device::ParseJsonArrayHeader(json, "\"peer_list\"", pos);
    if (ret != BM_OK) {
        return BM_OK;
    }
    const char *begin = json.data();
    const char *end = begin + json.size();
    bool expectComma = false;
    while (pos < json.size() && json[pos] != ']') {
        if (expectComma) {
            if (json[pos] != ',') {
                return BM_INVALID_PARAM;
            }
            ++pos;
            pos = device::SkipWs(begin, end, pos);
        }
        auto vr = device::SkipValue(begin, end, pos, 0);
        if (vr.result != BM_OK) {
            return vr.result;
        }
        int32_t localId = -1;
        (void)device::ExtractIntInRange(begin, pos, vr.offset, "\"local_id\"", localId);
        if (localId >= 0) {
            peerList.push_back(localId);
        }
        pos = vr.offset;
        pos = device::SkipWs(begin, end, pos);
        expectComma = true;
    }
    return BM_OK;
}

Result UrmaTopoManager::ParseEdgeList(const std::string &json, std::vector<Edge> &edgeList)
{
    size_t pos = 0;
    auto ret = device::ParseJsonArrayHeader(json, "\"edge_list\"", pos);
    if (ret != BM_OK) {
        return BM_OK;
    }
    const char *begin = json.data();
    const char *end = begin + json.size();
    bool expectComma = false;
    while (pos < json.size() && json[pos] != ']') {
        if (expectComma) {
            if (json[pos] != ',') {
                return BM_INVALID_PARAM;
            }
            ++pos;
            pos = device::SkipWs(begin, end, pos);
        }
        if (json[pos] != '{') {
            return BM_INVALID_PARAM;
        }
        auto vr = device::SkipValue(begin, end, pos, 0);
        if (vr.result != BM_OK) {
            return vr.result;
        }
        std::string edgeJson(begin + pos, vr.offset - pos);
        Edge edge{};
        (void)device::ExtractStrField(edgeJson, "\"link_type\"", edge.linkType);
        (void)device::ExtractStrField(edgeJson, "\"topo_type\"", edge.topoType);
        (void)device::ExtractStrField(edgeJson, "\"topo_attr\"", edge.topoAttr);
        (void)device::ExtractStrField(edgeJson, "\"position\"", edge.position);
        (void)device::ExtractIntField(edgeJson, "\"net_layer\"", edge.netLayer);
        (void)device::ExtractIntField(edgeJson, "\"topo_instance_id\"", edge.topoInstanceId);
        (void)device::ExtractIntField(edgeJson, "\"local_a\"", edge.localA);
        (void)device::ExtractIntField(edgeJson, "\"local_b\"", edge.localB);
        (void)device::ParseStrArray(edgeJson, "\"local_a_ports\"", edge.localAPorts);
        (void)device::ParseStrArray(edgeJson, "\"local_b_ports\"", edge.localBPorts);
        (void)device::ParseStrArray(edgeJson, "\"protocols\"", edge.protocols);
        if (edge.localA == localId_ || edge.localB == localId_) {
            edgeList.push_back(std::move(edge));
        }
        pos = vr.offset;
        pos = device::SkipWs(begin, end, pos);
        expectComma = true;
    }
    return BM_OK;
}

Result UrmaTopoManager::ParseTopoInfo(const std::string &json, TopoInfo &topoInfo)
{
    (void)device::ExtractStrField(json, "\"version\"", topoInfo.version);
    (void)device::ExtractStrField(json, "\"hardwareType\"", topoInfo.hardwareType);
    (void)device::ExtractIntField(json, "\"peer_count\"", topoInfo.peerCount);
    (void)device::ExtractIntField(json, "\"edge_count\"", topoInfo.edgeCount);
    auto ret = ParsePeerList(json, topoInfo.peerList);
    if (ret != BM_OK) {
        return ret;
    }
    return ParseEdgeList(json, topoInfo.edgeList);
}

Result UrmaTopoManager::Initialize(int32_t deviceId, uint32_t rankId)
{
    if (initialized_) {
        if (userDeviceId_ == deviceId) {
            BM_LOG_INFO("TopoManager: already initialized, deviceId=" << deviceId << " rankId=" << rankId);
            return BM_OK;
        }
        BM_LOG_ERROR("TopoManager: already initialized with deviceId="
                     << userDeviceId_ << ", reject re-init with deviceId=" << deviceId << " rankId=" << rankId);
        return BM_ERROR;
    }
    userDeviceId_ = deviceId;
    rankId_ = rankId;
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(SetDeviceIdentity(), "TopoManager: SetDeviceIdentity failed, deviceId="
                                                              << userDeviceId_ << " rankId=" << rankId_);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(SetDeviceBaseInfo(), "TopoManager: SetDeviceBaseInfo failed, deviceId="
                                                              << userDeviceId_ << " rankId=" << rankId_);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(
        BuildRootInfo(), "TopoManager: BuildRootInfo failed, deviceId=" << userDeviceId_ << " rankId=" << rankId_);
    if (rootInfo_.rankList.empty()) {
        BM_LOG_ERROR("TopoManager: local rank not found in rootinfo, deviceId="
                     << userDeviceId_ << " phyDeviceId=" << phyDeviceId_ << " rankId=" << rankId_);
        return BM_ERROR;
    }
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(
        BuildTopoInfo(), "TopoManager: BuildTopoInfo failed, deviceId=" << userDeviceId_ << " rankId=" << rankId_);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(BuildPeer2PeerMap(), "TopoManager: BuildPeer2PeerMap failed, deviceId="
                                                              << userDeviceId_ << " rankId=" << rankId_);
    initialized_ = true;
    return BM_OK;
}

Result UrmaTopoManager::SetDeviceBaseInfo()
{
    int64_t value = 0;
    auto ret = DlAclApi::RtGetDeviceInfo(userDeviceId_, 0, INFO_TYPE_MAINBOARD_ID, &value);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to get mainboardId: " << ret << " userId:" << userDeviceId_
                                                   << " logicId:" << logicDeviceId_);
        return BM_DL_FUNCTION_FAILED;
    }
    mainboardId_ = static_cast<uint32_t>(value);
    topoProductPtr_ = UrmaTopoProduct::GetUrmaTopoProduct(userDeviceId_, logicDeviceId_, phyDeviceId_, mainboardId_);
    if (topoProductPtr_ == nullptr) {
        BM_LOG_ERROR("Failed to get topoProductPtr from UrmaTopoProduct");
        return BM_ERROR;
    }
    ret = topoProductPtr_->GetLocalId(localId_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("Failed to get localId: " << ret << " localId:" << localId_);
        return ret;
    }
    BM_LOG_INFO("SetDeviceBaseInfo userDeviceId:" << userDeviceId_ << " logicId:" << logicDeviceId_
                                                  << " phyDeviceId:" << phyDeviceId_ << " localId:" << localId_);
    return BM_OK;
}

Result UrmaTopoManager::SetDeviceIdentity()
{
    auto ret = DlAclApi::RtGetLogicDevIdByUserDevId(userDeviceId_, &logicDeviceId_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("get logic deviceId failed: " << ret << " userId:" << userDeviceId_);
        return BM_DL_FUNCTION_FAILED;
    }
    // 实测需要使用userDeviceId
    ret = DlAclApi::AclrtGetPhyDevIdByLogicDevId(userDeviceId_, &phyDeviceId_);
    if (ret != 0) {
        BM_LOG_WARN("Failed to get phy deviceId by logicDevId, fallback to logicId: user="
                    << userDeviceId_ << ", logic=" << logicDeviceId_ << ", ret=" << ret);
        phyDeviceId_ = logicDeviceId_;
    }
    return BM_OK;
}

Result UrmaTopoManager::BuildRootInfo()
{
    auto ret = BuildRootInfoFromFile();
    if (ret == BM_OK) {
        BM_LOG_INFO("BuildRootInfoFromFile:" << RootInfoToString(rootInfo_));
        return BM_OK;
    }
    BM_LOG_INFO("TopoManager: read rootinfo file not found or not match, try auto build it");
    ret = topoProductPtr_->GetRootInfo(rootInfo_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("TopoManager: failed to auto build rootInfo board id : " << mainboardId_);
        return ret;
    }
    BM_LOG_INFO("BuildRootInfo:" << RootInfoToString(rootInfo_));
    return BM_OK;
}

Result UrmaTopoManager::BuildRootInfoFromFile()
{
    rootInfo_ = {};
    std::string rootInfoStr;
    auto ret = device::ReadFileToString(ROOTINFO_PATH, rootInfoStr);
    if (ret != BM_OK) {
        BM_LOG_INFO("TopoManager: read rootinfo failed, path=" << ROOTINFO_PATH << " deviceId=" << userDeviceId_
                                                               << " rankId=" << rankId_);
        return ret;
    }
    ret = device::ExtractStrField(rootInfoStr, "\"topo_file_path\"", rootInfo_.topoFilePath);
    if (ret != BM_OK) {
        BM_LOG_ERROR("TopoManager: topo_file_path not found, deviceId=" << userDeviceId_ << " rankId=" << rankId_);
        return ret;
    }
    (void)device::ExtractStrField(rootInfoStr, "\"version\"", rootInfo_.version);
    (void)device::ExtractIntField(rootInfoStr, "\"rank_count\"", rootInfo_.rankCount);
    ret = ParseRankListFull(rootInfoStr, rootInfo_.rankList);
    if (ret != BM_OK) {
        BM_LOG_ERROR("TopoManager: parse rank_list failed, deviceId=" << userDeviceId_ << " rankId=" << rankId_);
        return ret;
    }
    return BM_OK;
}

Result UrmaTopoManager::BuildTopoInfo()
{
    std::string topoFilePath = rootInfo_.topoFilePath;
    std::string topoStr;
    auto ret = device::ReadFileToString(topoFilePath, topoStr);
    if (ret != BM_OK) {
        BM_LOG_ERROR("TopoManager: read topo failed, path=" << topoFilePath << " deviceId=" << userDeviceId_
                                                            << " rankId=" << rankId_);
        return ret;
    }
    ret = ParseTopoInfo(topoStr, topoInfo_);
    if (ret != BM_OK) {
        BM_LOG_ERROR("TopoManager: parse topo failed, path=" << topoFilePath << " deviceId=" << userDeviceId_
                                                             << " rankId=" << rankId_);
        return ret;
    }
    BM_LOG_INFO("BuildTopoInfo:" << TopoInfoToString(topoInfo_));
    return BM_OK;
}

Result UrmaTopoManager::BuildPeer2PeerMap()
{
    if (rootInfo_.rankList.empty()) {
        BM_LOG_ERROR("BuildPeer2PeerMap: rootInfo rankList is empty, phyDeviceId=" << phyDeviceId_);
        return BM_ERROR;
    }
    const auto &rank = rootInfo_.rankList[0];
    for (const auto &edge : topoInfo_.edgeList) {
        if (edge.linkType != "PEER2PEER" || (edge.localA != localId_ && edge.localB != localId_)) {
            continue;
        }
        const bool localIsA = edge.localA == localId_;
        const auto &localPorts = localIsA ? edge.localAPorts : edge.localBPorts;
        for (const auto &edgePort : localPorts) {
            std::array<uint8_t, COMM_ADDR_EID_LEN> eid{};
            bool found = false;
            for (const auto &level : rank.levelList) {
                if (level.netType == NET_TYPE_CLOS) {
                    continue;
                }
                for (const auto &addr : level.rankAddrList) {
                    if (std::find(addr.ports.begin(), addr.ports.end(), edgePort) != addr.ports.end()) {
                        HexStrToEid(addr.addr, eid);
                        found = true;
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
            if (found) {
                if (localIsA) {
                    peer2PeerMap_[edge.localB] = eid;
                } else {
                    peer2PeerMap_[edge.localA] = eid;
                }
                break;
            }
        }
    }
    BM_LOG_INFO("BuildPeer2PeerMap: peerCount=" << peer2PeerMap_.size() << " phyDeviceId=" << phyDeviceId_
                                                << " localId=" << localId_);
    return BM_OK;
}

void UrmaTopoManager::Uninitialize()
{
    peer2PeerMap_.clear();
    rootInfo_ = RootInfo{};
    topoInfo_ = TopoInfo{};
    logicDeviceId_ = -1;
    phyDeviceId_ = -1;
    userDeviceId_ = -1;
    rankId_ = -1;
    localId_ = -1;
    mainboardId_ = 0;
    initialized_ = false;
    topoProductPtr_.reset();
}

Result UrmaTopoManager::GetPeer2NetEid(CommProtocol protocol, std::array<uint8_t, COMM_ADDR_EID_LEN> &eidData)
{
    const char *topoProtocol = device::FindPeer2NetTopoProtocol(protocol);
    if (topoProtocol == nullptr) {
        BM_LOG_ERROR("TopoManager: GetPeer2NetEid unsupported protocol="
                     << static_cast<int>(protocol) << " mainboardId=" << mainboardId_ << " deviceId=" << userDeviceId_);
        return BM_NOT_SUPPORTED;
    }
    const auto layerPorts = device::CollectPeer2NetPorts(topoInfo_, localId_, topoProtocol);
    if (layerPorts.empty()) {
        BM_LOG_ERROR("TopoManager: GetPeer2NetEid no peer2net edge, protocol="
                     << topoProtocol << " localId=" << localId_ << " mainboardId=" << mainboardId_);
        return BM_NOT_SUPPORTED;
    }
    const auto localRank = std::find_if(rootInfo_.rankList.begin(), rootInfo_.rankList.end(), [this](const Rank &rank) {
        return rank.deviceId >= 0 && rank.deviceId == phyDeviceId_;
    });
    if (localRank == rootInfo_.rankList.end()) {
        BM_LOG_ERROR("TopoManager: GetPeer2NetEid local rank not found, phyDeviceId="
                     << phyDeviceId_ << " rankId=" << rankId_ << " protocol=" << topoProtocol);
        return BM_ERROR;
    }
    int32_t netLayer = -1;
    const auto *bestAddr = device::FindPeer2NetAddr(*localRank, layerPorts, netLayer);
    if (bestAddr == nullptr) {
        BM_LOG_ERROR("TopoManager: GetPeer2NetEid no EID with overlapping ports, protocol="
                     << topoProtocol << " phyDeviceId=" << phyDeviceId_ << " rankId=" << rankId_
                     << " localId=" << localId_ << " netLayers=" << layerPorts.size());
        return BM_ERROR;
    }
    const auto ret = device::ParsePeer2NetEid(bestAddr->addr, protocol, eidData);
    BM_LOG_ERROR_RETURN_IT_IF_NOT_OK(ret, "TopoManager: invalid peer2net EID, protocol="
                                              << topoProtocol << " phyDeviceId=" << phyDeviceId_
                                              << " rankId=" << rankId_ << " netLayer=" << netLayer);
    BM_LOG_INFO("TopoManager: GetPeer2NetEid protocol=" << topoProtocol << " netLayer=" << netLayer
                                                        << " portsCount=" << bestAddr->ports.size()
                                                        << " phyDeviceId=" << phyDeviceId_ << " rankId=" << rankId_);
    return BM_OK;
}

Result UrmaTopoManager::GetPeer2PeerEids(std::unordered_map<int32_t, std::array<uint8_t, COMM_ADDR_EID_LEN>> &eidInfos)
{
    eidInfos = peer2PeerMap_;
    return BM_OK;
}

Result UrmaTopoManager::GetRootInfo(RootInfo &rootInfo)
{
    rootInfo.version = rootInfo_.version;
    rootInfo.topoFilePath = rootInfo_.topoFilePath;
    rootInfo.rankCount = rootInfo_.rankCount;
    rootInfo.rankList.clear();
    for (const auto &rank : rootInfo_.rankList) {
        rootInfo.rankList.push_back(rank);
    }
    return BM_OK;
}

Result UrmaTopoManager::GetLocalId(int32_t &localId)
{
    localId = localId_;
    return BM_OK;
}
} // namespace transport
} // namespace mf
} // namespace ock
