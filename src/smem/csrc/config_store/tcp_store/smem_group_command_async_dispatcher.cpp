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
#include "smem_logger.h"
#include "smem_group_command_async_dispatcher.h"

namespace ock::smem {

static const char *ControlOpName(ControlOp op)
{
    switch (op) {
        case CONTROL_ADD_TO_WHITELIST:
            return "ADD_TO_WHITELIST";
        case CONTROL_REMOVE_FROM_WHITELIST:
            return "REMOVE_FROM_WHITELIST";
        case CONTROL_ESTABLISH_CONNECTION:
            return "ESTABLISH_CONNECTION";
        case CONTROL_JOIN:
            return "JOIN";
        case CONTROL_LEAVE:
            return "LEAVE";
        case CONTROL_QUERY_LINK_STATE:
            return "QUERY_LINK_STATE";
        case CONTROL_LINK_STATE_RESPONSE:
            return "LINK_STATE_RESPONSE";
        case CONTROL_ADD_TO_WHITELIST_ACK:
            return "ADD_TO_WHITELIST_ACK";
        case CONTROL_REMOVE_FROM_WHITELIST_ACK:
            return "REMOVE_FROM_WHITELIST_ACK";
        case CONTROL_ESTABLISH_CONNECTION_ACK:
            return "ESTABLISH_CONNECTION_ACK";
        default:
            return "UNKNOWN";
    }
}

SmemGroupCommandAsyncDispatcher::SmemGroupCommandAsyncDispatcher(SmemGroupManager *groupMgr) noexcept
    : groupManager_{groupMgr}
{}

SmemGroupCommandAsyncDispatcher::~SmemGroupCommandAsyncDispatcher() noexcept
{
    Stop();
}

bool SmemGroupCommandAsyncDispatcher::Start() noexcept
{
    if (started_) {
        return true;
    }

    if (groupManager_ == nullptr) {
        SM_LOG_ERROR("groupManager is nullptr, cannot start!");
        return false;
    }

    started_ = true;
    backgroundThread_ = std::thread(&SmemGroupCommandAsyncDispatcher::BackgroundThread, this);
    SM_LOG_INFO("SmemGroupCommandAsyncDispatcher Started, localRankId=" << localRankId_);
    return true;
}

void SmemGroupCommandAsyncDispatcher::Stop() noexcept
{
    {
        std::lock_guard<std::mutex> locker{mutex_};
        started_ = false;
    }
    cond_.notify_all();
    if (backgroundThread_.joinable()) {
        SM_LOG_INFO("SmemGroupCommandAsyncDispatcher joining background thread...");
        backgroundThread_.join();
    }
    SM_LOG_INFO("SmemGroupCommandAsyncDispatcher Stopped.");
}

// ── Enqueue methods ────────────────────────────────────────────────

void SmemGroupCommandAsyncDispatcher::EnqueueAddToWhitelist(uint32_t rankId, std::vector<RankFullInfo> &&others,
                                                            uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    addWhitelistQueue_.push_back({rankId, std::move(others), reqId});
    cond_.notify_one();
}

void SmemGroupCommandAsyncDispatcher::EnqueueRemoveFromWhitelist(uint32_t rankId, std::vector<RankFullInfo> &&others,
                                                                 uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    removeWhitelistQueue_.push_back({rankId, std::move(others), reqId});
    cond_.notify_one();
}

void SmemGroupCommandAsyncDispatcher::EnqueueEstablishConnection(uint32_t rankId, std::vector<RankFullInfo> &&peers,
                                                                 uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    connectionQueue_.push_back({rankId, std::move(peers), reqId});
    cond_.notify_one();
}

void SmemGroupCommandAsyncDispatcher::EnqueueQueryLinkState(uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    if (!hasQueryLinkState_) {
        queryLinkStateReqId_ = reqId;
        hasQueryLinkState_ = true;
    }
    cond_.notify_one();
}

void SmemGroupCommandAsyncDispatcher::EnqueueAddSlices(uint32_t extendingRankId, MultiBytes &&newSlices,
                                                       uint64_t reqId) noexcept
{
    std::lock_guard locker{mutex_};
    addSlicesQueue_.push_back({extendingRankId, std::move(newSlices), reqId});
    cond_.notify_one();
}

// ── Ack sending ────────────────────────────────────────────────────

void SmemGroupCommandAsyncDispatcher::SendAck(ControlOp ackOp, const std::vector<uint32_t> &targetRankIds,
                                              const std::vector<int32_t> &results, uint64_t reqId) noexcept
{
    auto tag = AckTagName(ackOp);
    if (sendAckBatch_) {
        sendAckBatch_(ackOp, localRankId_, targetRankIds, results, reqId);
    } else {
        SM_LOG_WARN("[GM][Client][Ack] skip rank=" << localRankId_ << " type=" << tag << " no sendAckBatch_");
    }
}

// ── Background thread ──────────────────────────────────────────────

void SmemGroupCommandAsyncDispatcher::BackgroundThread() noexcept
{
    pthread_setname_np(pthread_self(), "NetAsyncGrpMgr");
    SM_LOG_INFO("SmemGroupCommandAsyncDispatcher::BackgroundThread enter.");

    while (started_) {
        DrainedJobs jobs;
        DrainQueues(jobs);
        if (!started_) {
            break;
        }
        ProcessDrainedJobs(jobs);
    }

    SM_LOG_INFO("SmemGroupCommandAsyncDispatcher::BackgroundThread exit.");
}

void SmemGroupCommandAsyncDispatcher::DrainQueues(DrainedJobs &jobs) noexcept
{
    std::unique_lock locker{mutex_};
    cond_.wait(locker, [this] {
        return !started_.load() || !addWhitelistQueue_.empty() || !removeWhitelistQueue_.empty() ||
               !connectionQueue_.empty() || hasQueryLinkState_ || !addSlicesQueue_.empty();
    });

    jobs.addJobs.swap(addWhitelistQueue_);
    jobs.rmvJobs.swap(removeWhitelistQueue_);
    jobs.connJobs.swap(connectionQueue_);
    jobs.slicesJobs.swap(addSlicesQueue_);
    if (hasQueryLinkState_) {
        jobs.doQuery = true;
        jobs.queryReqId = queryLinkStateReqId_;
        hasQueryLinkState_ = false;
    }
}

void SmemGroupCommandAsyncDispatcher::ProcessDrainedJobs(const DrainedJobs &jobs) noexcept
{
    for (auto &req : jobs.addJobs) {
        BackgroundAddWhiteList(req, req.reqId);
    }

    for (auto &req : jobs.rmvJobs) {
        BackgroundRmvWhiteList(req, req.reqId);
    }

    for (auto &req : jobs.connJobs) {
        BackgroundEstablishConnection(req, req.reqId);
    }

    if (jobs.doQuery) {
        BackgroundQueryLinkState(jobs.queryReqId);
    }

    for (auto &req : jobs.slicesJobs) {
        BackgroundAddSlices(req, req.reqId);
    }
}

// ── Background handlers ────────────────────────────────────────────

static int32_t DefaultResult(int ret)
{
    return (ret == 0) ? 0 : -1;
}

void SmemGroupCommandAsyncDispatcher::BackgroundAddWhiteList(const WhitelistRequest &req, uint64_t reqId) noexcept
{
    if (req.others.empty()) {
        SM_LOG_WARN("[GM][Client][Recv] AddWhiteList: empty others, reqId=" << reqId);
        return;
    }

    int overallRet = onAddToWhitelist_ ? onAddToWhitelist_(req.rankId, req.others, reqId) : -1;
    {
        const std::string peers = JoinRankIds(req.others);
        if (overallRet != 0) {
            // P2P 未就绪为可重试瞬时状态，根因已在 segment 层记 WARN，这里不重复打印
            if (overallRet != SMEM_GROUP_P2P_NOT_READY) {
                SM_LOG_ERROR("[GM][Client][Recv] WhiteList FAIL r=" << req.rankId << " ret=" << overallRet);
            }
        } else {
            SM_LOG_INFO("[GM][Client][Recv] WhiteList r=" << req.rankId << " peers=[" << peers << "]");
        }
    }

    std::vector<uint32_t> targets;
    std::vector<int32_t> results;
    targets.reserve(req.others.size());
    results.reserve(req.others.size());
    for (const auto &other : req.others) {
        targets.push_back(other.rankId);
        results.push_back(DefaultResult(overallRet));
    }
    SendAck(CONTROL_ADD_TO_WHITELIST_ACK, targets, results, reqId);
}

void SmemGroupCommandAsyncDispatcher::BackgroundRmvWhiteList(const WhitelistRequest &req, uint64_t reqId) noexcept
{
    if (req.others.empty()) {
        SM_LOG_WARN("[GM][Client][Recv] RmvWhiteList: empty others, reqId=" << reqId);
        return;
    }

    int overallRet = onRemoveFromWhitelist_ ? onRemoveFromWhitelist_(req.rankId, req.others, reqId) : -1;
    if (overallRet != 0) {
        SM_LOG_ERROR("[GM][Client][Recv] RmvWhiteList FAIL r=" << req.rankId << " ret=" << overallRet);
    } else {
        SM_LOG_INFO("[GM][Client][Recv] RmvWhiteList r=" << req.rankId << " peers=[" << JoinRankIds(req.others) << "]");
    }

    std::vector<uint32_t> targets;
    std::vector<int32_t> results;
    targets.reserve(req.others.size());
    results.reserve(req.others.size());
    for (const auto &other : req.others) {
        targets.push_back(other.rankId);
        results.push_back(DefaultResult(overallRet));
    }
    SendAck(CONTROL_REMOVE_FROM_WHITELIST_ACK, targets, results, reqId);
}

void SmemGroupCommandAsyncDispatcher::BackgroundEstablishConnection(const ConnEstablishRequest &req,
                                                                    uint64_t reqId) noexcept
{
    int overallRet = onEstablishConnection_ ? onEstablishConnection_(req.rankId, req.peers, reqId) : -1;
    {
        const std::string peers = JoinRankIds(req.peers);
        if (overallRet != 0) {
            SM_LOG_ERROR("[GM][Client][Recv] EstConn FAIL r=" << req.rankId << " ret=" << overallRet);
        } else {
            SM_LOG_INFO("[GM][Client][Recv] EstConn r=" << req.rankId << " peers=[" << peers << "]");
        }
    }

    std::vector<uint32_t> targetIds;
    std::vector<int32_t> results;
    targetIds.reserve(req.peers.size());
    results.reserve(req.peers.size());
    for (const auto &p : req.peers) {
        targetIds.push_back(p.rankId);
        results.push_back(DefaultResult(overallRet));
    }
    SendAck(CONTROL_ESTABLISH_CONNECTION_ACK, targetIds, results, reqId);
}

void SmemGroupCommandAsyncDispatcher::BackgroundQueryLinkState(uint64_t reqId) noexcept
{
    std::vector<LinkStateEntry> entries;
    if (onQueryLinkState_) {
        entries = onQueryLinkState_();
    }
    SM_LOG_INFO("[GM][Client][Send] QueryLinkState reqId=" << reqId << " entries=" << entries.size());
}

void SmemGroupCommandAsyncDispatcher::BackgroundAddSlices(const AddSlicesRequest &req, uint64_t reqId) noexcept
{
    if (onAddSlices_) {
        int ret = onAddSlices_(req.extendingRankId, req.newSlices, reqId);
        if (ret != 0) {
            // 仅结果追踪：瞬态（P2P 未就绪）与真实错误（如 payload 损坏）的根因均已在回调内按级别记录
            SM_LOG_WARN("[GM][Client][Recv] AddSlices FAIL ext=" << req.extendingRankId << " ret=" << ret);
        }
    }
    SM_LOG_INFO("[GM][Client][Recv] AddSlices ext=" << req.extendingRankId << " slices=" << req.newSlices.size());
}

} // namespace ock::smem
