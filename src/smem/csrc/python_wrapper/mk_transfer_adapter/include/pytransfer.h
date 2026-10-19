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
#ifndef PYTRANSFER_H
#define PYTRANSFER_H

#include <pybind11/pybind11.h>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "smem_bm_def.h"
#include "smem_trans.h"

#ifdef UINTPTR_MAX
using uintptr_t = ::uintptr_t;
inline auto to_uintptr(const void *p) -> uintptr_t
{
    return reinterpret_cast<uintptr_t>(p);
}
#else
using uintptr_t = fallback_uintptr;
inline auto to_uintptr(const void *p) -> fallback_uintptr
{
    return fallback_uintptr(p);
}
#endif

class TransferAdapterPy {
public:
    enum class TransferOpcode { READ = 0, WRITE = 1 };
    enum class TransDataOpType {
        SDMA = SMEMB_DATA_OP_SDMA,
        HOST_RDMA = SMEMB_DATA_OP_HOST_RDMA,
        DEVICE_RDMA = SMEMB_DATA_OP_DEVICE_RDMA,
        DEVICE_URMA = SMEMB_DATA_OP_DEVICE_URMA,
        DEVICE_UBOE = SMEMB_DATA_OP_DEVICE_UBOE
    };

public:
    TransferAdapterPy();

    ~TransferAdapterPy();

    int Initialize(const char *storeUrl, const char *uniqueId, const char *role, uint32_t deviceId,
                   TransDataOpType dataOpType, const char *storeServerRole = "Decode", const char *nic = nullptr);

    int GetRpcPort();

    int TransferSyncWrite(const char *destUniqueId, uintptr_t buffer, uintptr_t peer_buffer_address, size_t length,
                          uint32_t flags);

    int BatchTransferSyncWrite(const char *destUniqueId, std::vector<uintptr_t> buffers,
                               std::vector<uintptr_t> peer_buffer_addresses, std::vector<size_t> lengths,
                               uint32_t flags);

    int TransferSyncRead(const char *destUniqueId, uintptr_t buffer, uintptr_t peer_buffer_address, size_t length,
                         uint32_t flags);

    int BatchTransferSyncRead(const char *destUniqueId, std::vector<uintptr_t> buffers,
                              std::vector<uintptr_t> peer_buffer_addresses, std::vector<size_t> lengths,
                              uint32_t flags);

    int TransferAsyncReadSubmit(const char *destUniqueId, uintptr_t buffer, uintptr_t peer_buffer_address,
                                size_t length, uintptr_t stream, uint32_t flags);

    int TransferAsyncWriteSubmit(const char *destUniqueId, uintptr_t buffer, uintptr_t peer_buffer_address,
                                 size_t length, uintptr_t stream, uint32_t flags);

    int BatchTransferAsyncWriteSubmit(const char *destUniqueId, std::vector<uintptr_t> buffers,
                                      std::vector<uintptr_t> peer_buffer_addresses, std::vector<size_t> lengths,
                                      uintptr_t stream, uint32_t flags);

    int BatchTransferAsyncReadSubmit(const char *destUniqueId, std::vector<uintptr_t> buffers,
                                     std::vector<uintptr_t> peer_buffer_addresses, std::vector<size_t> lengths,
                                     uintptr_t stream, uint32_t flags);

    int BatchTransferWriteWithQuant(const char *destUniqueId, std::vector<uintptr_t> buffers,
                                    std::vector<uintptr_t> peer_buffer_addresses, std::vector<size_t> lengths,
                                    std::vector<uintptr_t> scale_addresses, std::vector<uintptr_t> offset_addresses,
                                    uint32_t unit_num, uint32_t input_type, uintptr_t stream, uint32_t flags);

    int RegisterMemory(uintptr_t buffer_addr, size_t capacity);

    int UnregisterMemory(uintptr_t buffer_addr);

    int BatchRegisterMemory(std::vector<uintptr_t> buffer_addrs, std::vector<size_t> capacities);

    void TransferDestroy();

    void UnInitialize();

private:
    // === configuration ===
    std::string GetConfigStoreProtocol(const std::string &storeUrl);

    // === store initialization ===
    int InitStoreServer(const std::string &ip, uint16_t port);

    // store-client path: no real listener; picks a port
    int InitStoreClient(const std::string &ip, uint16_t port);

    // === connection management ===
    smem_trans_t GetOrCreateConnection(const std::string &sessionId);
    // Register the initial snapshot before Join; caller holds regMemMutex_. Returns null on failure.
    smem_trans_t CreateAndReplayConnection(const std::string &sessionId, const std::string &storeUrl);
    // Clear the pending flag and wake up the waiting threads; connMutex_ must be held
    void FinishPendingConnection(const std::string &sessionId);
    // Caller holds regMemMutex_; lock order is registration, then connections.
    int RegisterOnConnections(std::vector<void *> &addresses, std::vector<size_t> &capacities);

    // === link down async cleanup ===
    static void PeerDownCallback(const char *peerAddr, void *userData);
    void OnLinkDownByPeerAddr(const std::string &peerAddr);
    void StartLinkDownConsumer();
    void StopLinkDownConsumer();

    // ordered teardown used by destroy()/unInitialize() and the destructor
    void Shutdown();

    smem_trans_t handle_ = nullptr;   // direct handle (receiver or legacy single-store sender)
    bool smemInited_ = false;         // true after smem_trans_init succeeded
    std::string sessionId_;           // local session id, format "IP:PORT"
    smem_trans_config_t config_{};    // transfer config
    std::string configStoreProtocol_; // transfer config store protocol
    uint16_t rpcPort_ = 0;            // real listening port (set in Initialize)

    // session_id → connection
    struct DConnection {
        smem_trans_t handle = nullptr;
        bool active = false;
    };
    std::mutex connMutex_;
    std::condition_variable connCv_;
    std::set<std::string> pendingConnections_;
    std::map<std::string, DConnection> connections_;

    // registered memories for replay
    struct RegMem {
        uintptr_t addr;
        size_t capacity;
    };
    std::vector<RegMem> registeredMems_;
    std::mutex regMemMutex_; // protects registeredMems_

    // link down async cleanup
    std::queue<std::string> linkDownQueue_;
    std::mutex linkDownQueueMutex_;
    std::condition_variable linkDownCv_;
    std::thread linkDownConsumerThread_;
    std::atomic<bool> consumerRunning_{false};

    // probe socket held bound for the store-client to reserve its session port;
    // -1 for the store-server (the config-store listener holds the port).
    int sockfd_ = -1;
};

#endif // PYTRANSFER_H
