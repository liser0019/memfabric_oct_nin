/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
 * MemFabric_Hybrid is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
*/
#ifndef MF_SMEM_TRANS_ENTRY_H
#define MF_SMEM_TRANS_ENTRY_H

#include <thread>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <condition_variable>
#include <future>

#include "smem_common_includes.h"
#include "smem_config_store.h"
#include "smem_group_manager.h"
#include "smem_tcp_config_store.h"
#include "mf_net.h"
#include "hybm_def.h"
#include "mf_rwlock.h"
#include "smem_trans.h"
#include "smem_net_common.h"

namespace ock {
namespace smem {

/*
 * lookup key of peer transfer entry
 */
using PeerEntryKey = std::pair<std::string, uint32_t>;
/*
 * peer transfer entry value, to store peer address etc.
 */
struct PeerEntryValue {
    void *address = nullptr;
};

struct WorkerUniqueId {
    ock::mf::net_addr_t address{};
    uint32_t pid{0};
    uint16_t port{0};
    uint16_t reserved{0}; // used in join
};

using WorkerId = std::array<uint8_t, sizeof(WorkerUniqueId)>;

struct WorkerIdHash {
    size_t operator()(const WorkerId &id) const
    {
        return std::hash<std::string>()(std::string(id.begin(), id.end()));
    }
};

union WorkerIdUnion {
    WorkerUniqueId session;
    WorkerId workerId;

    explicit WorkerIdUnion(WorkerUniqueId ws) : session(ws) {}
    explicit WorkerIdUnion(WorkerId id) : workerId{id} {}
    explicit WorkerIdUnion() {}
};

struct LocalMapAddress {
    void *address;
    uint64_t size;
    LocalMapAddress() : address{nullptr}, size{0} {}
    LocalMapAddress(void *p, uint64_t s) : address{p}, size{s} {}
};

struct SmemTransExchangeInfo {
    hybm_exchange_info hybmInfo;
    union U {
        WorkerUniqueId session;
        LocalMapAddress address;
        U() noexcept {}
    } u;
};

class SmemTransEntry;
using SmemTransEntryPtr = SmRef<SmemTransEntry>;
using Local2GlobalMap = std::map<const void *, LocalMapAddress, std::greater<const void *>>;

class SmemTransEntry : public SmReferable {
public:
    static SmemTransEntryPtr Create(const std::string &name, const std::string &storeUrl,
                                    const smem_trans_config_t &config,
                                    const std::vector<std::pair<const void *, size_t>> &initialMemories = {});

public:
    explicit SmemTransEntry(const smem_trans_config_t &config, std::string &name, uint32_t rank, uint32_t id,
                            StorePtr &store)
        : config_(config), name_(name), store_(store), entityId_(id), rankId_(rank)
    {
        entityInfo_.hybmInfo = {};
    }

    ~SmemTransEntry() override;

    const std::string &Name() const;
    const smem_trans_config_t &Config() const;

    Result Initialize();
    void UnInitialize();

    Result RegisterLocalMemory(const void *address, uint64_t size, uint32_t flags);
    Result RegisterLocalMemories(const std::vector<std::pair<const void *, size_t>> &regMemories, uint32_t flags);
    Result SyncTransfer(void *localAddr, const std::string &remoteUniqueId, void *remoteAddr, size_t dataSize,
                        smem_bm_copy_type opcode, void *stream, uint32_t flags);
    Result BatchSyncTransfer(void *localAddrs[], const std::string &remoteUniqueId, void *remoteAddrs[],
                             const size_t dataSizes[], uint32_t batchSize, smem_bm_copy_type opcode, void *stream,
                             uint32_t flags);
    Result BatchQuantTransfer(smem_trans_quant_copy_param_t *params, smem_bm_copy_type opcode);

    void SetPeerDownCallback(smem_trans_peer_down_callback_t callback, void *userData);

    // Pure helpers (no instance state); exposed public so unit tests can cover align/merge logic
    static void AlignMemory(const void *&address, uint64_t &size);
    static std::vector<std::pair<const void *, size_t>>
    CombineMemories(std::vector<std::pair<const void *, size_t>> &input);

private:
    Result Join(uint32_t flags);
    Result Update(uint32_t flags);
    Result Leave(uint32_t flags);

    int SetupGroupManagerCallbacks() noexcept;
    int OnAddToWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) noexcept;
    int OnRemoveFromWhitelist(uint32_t rankId, const std::vector<RankFullInfo> &others, uint64_t reqId) noexcept;
    int OnEstablishConnection(uint32_t rankId, const std::vector<RankFullInfo> &peers, uint64_t reqId) noexcept;
    std::vector<ock::smem::LinkStateEntry> OnQueryLinkState() noexcept;
    int OnAddSlices(uint32_t extendingRankId, const MultiBytes &newSlices, uint64_t reqId) noexcept;
    int ImportPeerSlicesToMap(const RankFullInfo &other, std::vector<void *> &global,
                              std::vector<SmemTransExchangeInfo> &slices) noexcept;
    int ImportPeerEntities(const std::vector<RankFullInfo> &others,
                           const std::vector<SmemTransExchangeInfo> &entities) noexcept;
    // These helpers require remoteSliceRwMutex_ to be held for writing.
    int RemovePeerImported(uint32_t groupRank) noexcept;
    int PreparePeerReplacement(uint32_t groupRank, uint32_t newHybmRank, const Bytes &baseInfo) noexcept;

    // Invoke the peer-down callback for a rank that left the group (worker notification)
    void NotifyPeerDown(uint32_t leavingRankId, const WorkerId &workerId) noexcept;

    // Group-manager callback plumbing (extracted to keep SetupGroupManagerCallbacks small)
    int RegisterAsyncCallbacks(SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept;
    int RegisterExecutorCallbacks(SmemGroupManagerClient *executor, SmemGroupCommandAsyncDispatcher *asyncMgr) noexcept;

    // Parse a peer's baseInfo/externalInfo into exchange info (shared by whitelist/establish paths)
    bool ParseExchangeInfo(const RankFullInfo &peer, SmemTransExchangeInfo &entityInfo, smem_trans_role_t &role,
                           WorkerId &id) noexcept;
    int ParsePeerSlices(const RankFullInfo &peer, std::vector<SmemTransExchangeInfo> &slices,
                        std::vector<LocalMapAddress> &local) noexcept;
    // Per-peer import/dedup/record for the establish path (extracted from OnEstablishConnection)。
    // 返回 SMEM_OK/SMEM_ERROR：import 失败需上抛，交由上层状态机重试，避免静默吞错。
    int EstablishPeerConnection(const RankFullInfo &peer) noexcept;

    // Apply one data-op flag to the hybm options (shared by the four op-type branches in Initialize)
    bool ApplyDataOpType(hybm_options &options, uint32_t flag, hybm_data_op_type hybmFlag, const char *opName) noexcept;

    void AddRemoteInfo(uint32_t rk, smem_trans_role_t role, WorkerId &id, std::vector<void *> &global,
                       std::vector<LocalMapAddress> &local);
    smem_trans_role_t QueryRole(uint32_t rk);

    bool ParseTransName(const std::string &name, ock::mf::net_addr_t &ip, uint16_t &port);
    void RemoveRanks(std::vector<uint32_t> &rankSet);
    Result ParseNameToUniqueId(const std::string &name, WorkerId &uniqueId);
    Result RegisterOneMemory(const void *address, uint64_t size, uint32_t flags);
    // Caller holds memMutex_, or the entry is not yet exposed to users.
    Result PrepareLocalMemories(const std::vector<std::pair<const void *, size_t>> &regMemories, uint32_t flags);
    void ClearRegisteredInfos();
    hybm_options GenerateHybmOptions();
    Result TransformAddr(Local2GlobalMap &maps, std::vector<void *> &addr, void *remoteAddrs[],
                         const size_t dataSizes[], uint32_t size);

private:
    hybm_entity_t entity_ = nullptr; /* local hybm entity */

    uint32_t rankId_ = 0;
    uint16_t entityId_ = 0;

    std::mutex memMutex_;
    std::vector<SmemTransExchangeInfo> registedInfo_;
    std::unordered_set<uint64_t> exportedSliceIds_; /* 已 export/广播过的 slice ID，用于 fake 注册去重 */

    const std::string name_;
    UrlExtraction storeUrlExtraction_;
    smem_trans_config_t config_{}; /* config of transfer entry */
    WorkerUniqueId workerUniqueId_;

    SmemTransExchangeInfo entityInfo_;
    ock::mf::ReadWriteLock remoteSliceRwMutex_;
    std::unordered_map<WorkerId, Local2GlobalMap, WorkerIdHash> remoteSlices_;
    std::unordered_map<uint32_t, WorkerId> rankToWorkerId_;
    std::unordered_map<uint32_t, uint32_t> rankToHybmRank_;
    std::unordered_map<uint32_t, Bytes> rankToPeerBase_;
    std::map<std::string, WorkerId> nameToWorkerId_; /* To accelerate name parsed */
    std::unordered_map<uint32_t, smem_trans_role_t> ranksRole_;

    StorePtr store_;
    bool joined_ = false;
    std::shared_ptr<std::promise<void>> joinComplete_;

    // peer down callback
    smem_trans_peer_down_callback_t peerDownCallback_ = nullptr;
    void *peerDownUserData_ = nullptr;
};

inline const std::string &SmemTransEntry::Name() const
{
    return name_;
}

inline const smem_trans_config_t &SmemTransEntry::Config() const
{
    return config_;
}
} // namespace smem
} // namespace ock

#endif // MF_SMEM_TRANS_ENTRY_H
