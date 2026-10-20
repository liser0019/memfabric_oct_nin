/**
 * Copyright (c) 2025 Huawei Technologies Co., Ltd.
 * This program is free software, you can redistribute it and/or modify it under the terms and conditions of
 * CANN Open Software License Agreement Version 2.0 (the "License").
 * Please refer to the License for details. You may not use this file except in compliance with the License.
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
 * See LICENSE in the root of the software repository for the full text of the License.
 */

#include "ub_transport_lite_impl.h"
#include "binary_stream.h"
#include "ub_conn_lite.h"
#include "ub_conn_lite_mgr.h"
#include "exception_util.h"
#include "internal_exception.h"
#include "sal.h"
#include "communicator_impl_lite_manager.h"
#include "profiling_handler_lite.h"

namespace Hccl {
constexpr u32 UB_WQE_BB_SIZE       = 64;  // 一个WQE BB是64Byte
constexpr u32 UB_WQE_MAX_SIZE      = 128; // 针对WriteWithNotify类型WQE，最大是128Byte
constexpr u32 UB_INLINE_WRITE_SIZE = 4;
constexpr u32 UB_RELAX_ORDER       = 0X01; // Relax Order表示当前SQE与后续Strong Order SQE有保序要求
constexpr u32 UB_STRONG_ORDER      = 0X02; // Strong Order表示当前SQE有保序要求，该SQE不能超越前面的Relax Order SQE
constexpr u32 UB_NO_COMPLETION     = 0;    // 表示当前报文和前面报文没有completion序要求，报文对应的CQE可以乱序上报
constexpr u32 UB_COMPLETION        = 1;    // 表示当前报文和前面报文有completion序要求，报文对应的CQE需要保序上报
constexpr u8  UB_FENCE_ENABLED     = 1;    // fence使能
UbTransportLiteImpl::UbTransportLiteImpl(
    std::vector<char> &uniqueId, std::function<void(u32 streamId, u32 taskId, const TaskParam &taskParam)> callback)
{
    callback_ = callback;
    // [header...][notifyUniqueId...][rmtNotifyUniqueId...][rmtBufferUniqueIds...]
    BinaryStream binaryStream(uniqueId);
    u32          theType;
    binaryStream >> theType;
    binaryStream >> notifyNum;
    binaryStream >> bufferNum;
    binaryStream >> rmtbufferNum;
    binaryStream >> connNum;

    std::vector<char> notifyUniqueIds;
    binaryStream >> notifyUniqueIds;
    ParseLocNotifyVec(notifyUniqueIds);

    std::vector<char> rmtNotifyUniqueIds;
    binaryStream >> rmtNotifyUniqueIds;
    ParseRmtBufferVec(rmtNotifyUniqueIds, RmaUbBufType::NOTIFY);

    std::vector<char> rmtBufferUniqueIds;
    binaryStream >> rmtBufferUniqueIds;
    ParseRmtBufferVec(rmtBufferUniqueIds, RmaUbBufType::BUFFER);

    std::vector<char> connUniqueIds;
    binaryStream >> connUniqueIds;
    ParseConnVec(connUniqueIds);
}
UbTransportLiteImpl::UbTransportLiteImpl(std::vector<char> &uniqueId)
{
    Init(uniqueId);
}

void UbTransportLiteImpl::Init(std::vector<char> &uniqueId)
{
    BinaryStream binaryStream(uniqueId);
    u32          theType;
    binaryStream >> theType;
    binaryStream >> notifyNum;
    binaryStream >> bufferNum;
    binaryStream >> rmtbufferNum;
    binaryStream >> connNum;
 
    std::vector<char> notifyUniqueIds;
    binaryStream >> notifyUniqueIds;
    ParseLocNotifyVec(notifyUniqueIds);
 
    std::vector<char> rmtNotifyUniqueIds;
    binaryStream >> rmtNotifyUniqueIds;
    ParseRmtBufferVec(rmtNotifyUniqueIds, RmaUbBufType::NOTIFY);
 
    std::vector<char> locBufferUniqueIds;
    binaryStream >> locBufferUniqueIds;
    ParseLocBufferMap(locBufferUniqueIds);

    std::vector<char> rmtBufferUniqueIds;
    binaryStream >> rmtBufferUniqueIds;
    ParseRmtBufferVec(rmtBufferUniqueIds, RmaUbBufType::BUFFER);

    std::vector<char> connUniqueIds;
    binaryStream >> connUniqueIds;
    ParseConnVec(connUniqueIds);
}

UbTransportLiteImpl::~UbTransportLiteImpl()
{
    for (auto &it : connUniqueIdVec) {
       DECTOR_TRY_CATCH("UbTransportLiteImpl",  UbConnLiteMgr::GetInstance().Clear(it));
    }
}

std::string UbTransportLiteImpl::Describe() const
{
    std::string desc = "UbTransportLiteImpl[";

    u32 idx = 0;
    desc += "locNotifyVec=[";
    for (auto &it : locNotifyVec) {
        desc += StringFormat("idx=%u, %s;", idx, it->Describe().c_str());
        idx++;
    }

    idx = 0;
    desc += "], rmtNotifyVec=[";
    for (auto &it : rmtNotifyVec) {
        desc += StringFormat("idx=%u, %s;", idx, it.Describe().c_str());
        idx++;
    }

    idx = 0;
    desc += "], rmtBufferVec=[";
    for (auto &it : rmtBufferVec) {
        desc += StringFormat("idx=%u, %s;", idx, it.Describe().c_str());
        idx++;
    }

    idx = 0;
    desc += "], connVec=[";
    for (auto &it : connVec) {
        desc += StringFormat("idx=%u, %s;", idx, it->Describe().c_str());
        idx++;
    }

    desc += "]]";
    return desc;
}

void UbTransportLiteImpl::ParseLocNotifyVec(std::vector<char> &data)
{
    if (notifyNum == 0) {
        HCCL_WARNING("UbTransportLiteImpl::ParseLocNotifyVec num is 0");
        return;
    }
    u32 notifySizePerDto = data.size() / notifyNum;

    for (u32 idx = 0; idx < notifyNum; idx++) {
        auto              start = data.begin() + idx * notifySizePerDto;
        auto              end   = start + notifySizePerDto;
        std::vector<char> dto(start, end);
        locNotifyVec.push_back(std::make_unique<NotifyLite>(dto));
        HCCL_INFO("locNotify idx=%u, %s", idx, locNotifyVec.back()->Describe().c_str());
    }
}

void UbTransportLiteImpl::ParseRmtBufferVec(std::vector<char> &data, RmaUbBufType rmtType)
{
    u32 num = 0;
    if (rmtType == RmaUbBufType::NOTIFY) {
        num = notifyNum;
    } else {
        num = rmtbufferNum;
    }

    if (num == 0) {
        HCCL_WARNING("UbTransportLiteImpl::ParseRmtBufferVec %s num is 0", rmtType.Describe().c_str());
        return;
    }

    u32 rmtBufferSizePerDto = data.size() / num;
    HCCL_INFO("Parse %s num=%u, sizePerDto=%u", rmtType.Describe().c_str(), num, rmtBufferSizePerDto);
    BinaryStream binaryStream(data);

    for (u32 idx = 0; idx < num; idx++) {
        RmtUbBufLite ubBufLite;
        binaryStream >> ubBufLite.addr;
        binaryStream >> ubBufLite.size;
        binaryStream >> ubBufLite.tokenId;
        binaryStream >> ubBufLite.tokenValue;
        binaryStream >> ubBufLite.notifyId;
        HCCL_INFO("idx=%u, %s %s", idx, rmtType.Describe().c_str(), ubBufLite.Describe().c_str());
        if (rmtType == RmaUbBufType::NOTIFY) {
            rmtNotifyVec.push_back(ubBufLite);
        } else {
            rmtBufferMap[static_cast<uintptr_t>(ubBufLite.addr)] = ubBufLite;
            rmtBufferVec.push_back(ubBufLite);
        }
    }
}

void UbTransportLiteImpl::ParseLocBufferMap(std::vector<char> &data)
{
    u32 num = bufferNum;
 
    if (num == 0) {
        HCCL_WARNING("UbTransportLiteImpl::ParseLocBufferMap num is 0");
        return;
    }
 
    // GetSingleRmtBufferUniqueId also writes notifyId after the four local MR fields.
    constexpr size_t baseDtoBytes = 2U * sizeof(u64) + 2U * sizeof(u32);
    const size_t rmtBufferSizePerDto = data.size() / num;
    if (data.size() % num != 0 ||
        (rmtBufferSizePerDto != baseDtoBytes && rmtBufferSizePerDto != baseDtoBytes + sizeof(u32))) {
        MACRO_THROW(InternalException,
                    StringFormat("invalid local buffer DTO layout: num=%u bytes=%zu", num, data.size()));
    }
    HCCL_INFO("ParseLocBufferMap num=%u, sizePerDto=%zu", num, rmtBufferSizePerDto);
    BinaryStream binaryStream(data);
 
    for (u32 idx = 0; idx < num; idx++) {
        LocUbBufLite ubBufLite;
        binaryStream >> ubBufLite.addr;
        binaryStream >> ubBufLite.size;
        binaryStream >> ubBufLite.tokenId;
        binaryStream >> ubBufLite.tokenValue;
        if (rmtBufferSizePerDto == baseDtoBytes + sizeof(u32)) {
            u32 ignoredNotifyId = 0;
            binaryStream >> ignoredNotifyId;
        }
        HCCL_INFO("idx=%u, LocBuffer %s", idx, ubBufLite.Describe().c_str());
        locBufferMap[static_cast<uintptr_t>(ubBufLite.addr)] = ubBufLite;
    }
}

void UbTransportLiteImpl::ParseConnVec(std::vector<char> &data)
{
    if (connNum == 0) {
        HCCL_WARNING("UbTransportLiteImpl::ParseConnVec num is 0");
        return;
    }
    u32 connSizePerDto = data.size() / connNum;
    HCCL_INFO("Parse ConnVec num=%u, connSizePerDto=%u", connNum, connSizePerDto);
    for (u32 idx = 0; idx < connNum; idx++) {
        auto              start = data.begin() + idx * connSizePerDto;
        auto              end   = start + connSizePerDto;
        std::vector<char> connUniqueId(start, end);
        connUniqueIdVec.push_back(connUniqueId);
        // connLite的复用由 ubConnLiteMgr管理
        auto lite = UbConnLiteMgr::GetInstance().Get(connUniqueId);
        connVec.push_back(lite);
        HCCL_INFO("[%s]idx=%u, %s", __func__, idx, lite->Describe().c_str());
    }
    CheckConnVec("after ParseConnVec");
}

void UbTransportLiteImpl::BuildUbDbSendTask(const StreamLite &stream, const UbJettyLiteId &jettyLiteId, u32 pi)
{
    stream.GetRtsq()->UbDbSend(jettyLiteId, pi);
}

void UbTransportLiteImpl::BuildNotifyWaitTask(const StreamLite &stream, u32 notifyId)
{
    stream.GetRtsq()->NotifyWait(notifyId);
}

Buffer UbTransportLiteImpl::GetRmtBuffer(u32 index)
{
    if (UNLIKELY(index >= rmtBufferVec.size())) {
        THROW<InternalException>(StringFormat("UbTransportLiteImpl::GetRmtBuffer out-of-bounds. index=%u, size=%u",
            index, rmtBufferVec.size()));
    }
    return Buffer(rmtBufferVec[index].addr, rmtBufferVec[index].size);
}

RmtRmaBufSliceLite UbTransportLiteImpl::GetRmtNotifySliceLite(u32 index)
{
    RmtUbBufLite &lite = rmtNotifyVec[index];
    // ub conn lite 不关心rkey , rkey 设定为0
    return RmtRmaBufSliceLite(lite.addr, lite.size, 0, lite.tokenId, lite.tokenValue, lite.notifyId);
}

RmtRmaBufSliceLite UbTransportLiteImpl::GetRmtRmaBufSliceLite(const Buffer &rmtBuf)
{
    auto it  = rmtBufferMap.upper_bound(rmtBuf.GetAddr());

    while(it != rmtBufferMap.begin()) {
        --it;
        Buffer iterBuf(it->second.addr, it->second.size);
        if (iterBuf.Contains(rmtBuf.GetAddr(), rmtBuf.GetSize())) {
            return RmtRmaBufSliceLite(rmtBuf.GetAddr(), rmtBuf.GetSize(), 0, it->second.tokenId, it->second.tokenValue,
                UINT32_MAX);
        }
    }
    MACRO_THROW(InternalException, StringFormat("%s is not in current transport", rmtBuf.Describe().c_str()));
}

RmtRmaBufSliceLite UbTransportLiteImpl::GetRmtRmaBufSliceLite(const RmaBufferLite &lite) const
{
    return RmtRmaBufSliceLite(lite.GetAddr(), lite.GetSize(), 0, lite.GetTokenId() , lite.GetTokenValue(), UINT32_MAX);
}

HcclResult UbTransportLiteImpl::BuildLocRmaBufferLite(const uintptr_t addr, const size_t size, RmaBufferLite &rmaBufferLite)
{
    HCCL_INFO("[UbTransportLiteImpl::%s] start to find addr[0x%llx], size[0x%llx] in locBufferMap, whose size is %zu. ",
        __func__, addr, size, locBufferMap.size());
    if (locBufferMap.empty()) {
        HCCL_ERROR("[UbTransportLiteImpl::%s] locBufferMap is empty.", __func__);
        return HCCL_E_INTERNAL;
    }

    bool isAddrInRange = false;
    auto it  = locBufferMap.upper_bound(addr);

    while(it != locBufferMap.begin()) {
        --it;
        Buffer iterBuf(it->second.addr, it->second.size);
        if (iterBuf.Contains(addr, size)) {
            rmaBufferLite = RmaBufferLite(addr, size, it->second.tokenId, it->second.tokenValue);
            isAddrInRange = true;
            break;
        }
    }

    if (!isAddrInRange) {
        HCCL_WARNING("[UbTransportLiteImpl::%s] addr[0x%llx], size[0x%llx] not in any range of locBufferMap, use the first in map addr[0x%llx] size[0x%llx]",
            __func__, addr, size, it->second.addr, it->second.size);
        rmaBufferLite = RmaBufferLite(addr, size, it->second.tokenId, it->second.tokenValue);
    }

    return HCCL_SUCCESS;
}

void UbTransportLiteImpl::ClearConnOut()
{
    wqeData.clear();
    wqeData.resize(UB_WQE_MAX_SIZE);
    connOut.data     = (u8 *)wqeData.data();
    connOut.dataSize = sizeof(wqeData);
}

// 检查connection不能为空
void UbTransportLiteImpl::CheckConnVec(const std::string &desc)
{
    if (UNLIKELY(connVec.size() == 0)) {
        THROW<InternalException>(StringFormat("connVec size is 0 %s", desc.c_str()));
    }

    u32 idx = 0;
    for (auto &it : connVec) {
        if (UNLIKELY(it == nullptr)) {
            THROW<InternalException>(StringFormat("connVec[%u] is null %s", idx, desc.c_str()));
        }
        idx++;
    }
}

RmaBufSliceLite UbTransportLiteImpl::GetRmaBufSlicelite(const RmaBufferLite &lite) const
{
    // ub conn lite 不关心rkey , rkey 设定为0
    return RmaBufSliceLite(lite.GetAddr(), lite.GetSize(), 0, lite.GetTokenId());
}

void UbTransportLiteImpl::Post(u32 index, const StreamLite &stream)
{
    SqeConfigLite cfg;
    if (index == 1) { // PostFin场景
        cfg.cqeEn     = true;
        cfg.placeOdr  = UB_STRONG_ORDER;
        cfg.compOrder = UB_COMPLETION;
    }
    u32           inlineData = 1;

    auto taskId = stream.GetRtsq()->GetTaskId();
    // 当前使用1个connection，下标为0 构建sqe
    auto rmtBuffSliceLite = GetRmtNotifySliceLite(index);
    connVec[0]->InlineWrite(reinterpret_cast<u8 *>(&inlineData), UB_INLINE_WRITE_SIZE, rmtBuffSliceLite,
                            cfg, stream, connOut);
    // 构建rts 的 sqe
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi);

    HCCL_INFO("UbTransportLiteImpl::Post notifyId[0x%llx], pi=%u", rmtBuffSliceLite.GetAddr(), connOut.pi);

    if (!IsReportTask()) {
        return;
    }

    TaskParam taskParam{};
    taskParam.taskType                 = TaskParamType::TASK_UB_INLINE_WRITE;
    taskParam.beginTime                = ProfGetCurCpuTimestamp();
    taskParam.taskPara.DMA.dst         = reinterpret_cast<void*>(rmtBuffSliceLite.GetAddr());
    taskParam.taskPara.DMA.size        = rmtBuffSliceLite.GetSize();
    taskParam.taskPara.DMA.notifyID    = rmtBuffSliceLite.GetNotifyId();
    taskParam.taskPara.DMA.notifyValue = 1;
    taskParam.taskPara.DMA.linkType    = DfxLinkType::UB;
    taskParam.taskPara.DMA.dmaOp       = DmaOp::HCCL_DMA_WRITE;
    taskParam.taskPara.DMA.locEid      = GetLocEid();
    taskParam.taskPara.DMA.rmtEid      = GetRmtEid();

    HCCL_INFO("[UbTransportLiteImpl::%s] locEid[%s], rmtEid[%s]", __func__, GetLocEid().Describe().c_str(), GetRmtEid().Describe().c_str());

    AddTaskCallback(stream, taskId, taskParam);
}

void UbTransportLiteImpl::Wait(u32 index, const StreamLite &stream)
{
    WaitWithTimeout(index, stream, CommunicatorImplLiteMgr::GetInstance().GetEnvConfig().hcclExecTimeout);
}

void UbTransportLiteImpl::WaitWithTimeout(u32 index, const StreamLite &stream, u32 timeout)
{
    auto taskId   = stream.GetRtsq()->GetTaskId();
    auto notifyId = locNotifyVec[index]->GetId();
    stream.GetRtsq()->NotifyWait(notifyId, timeout);

    if (!IsReportTask()) {
        return;
    }

    TaskParam taskParam{};
    taskParam.taskType                 = TaskParamType::TASK_NOTIFY_WAIT;
    taskParam.beginTime                = ProfGetCurCpuTimestamp();
    taskParam.taskPara.Notify.notifyID = notifyId;
    taskParam.taskPara.Notify.value    = 1;

    AddTaskCallback(stream, taskId, taskParam);
}

void UbTransportLiteImpl::ProfilingProcess(void *src, void *dst, u64 size, const StreamLite &stream,
                                           DmaOp dmaOp, u32 taskId)
{
    if (!IsReportTask()) {
        return;
    }

    TaskParam taskParam{};
    taskParam.taskType = TaskParamType::TASK_UB;
    taskParam.beginTime = ProfGetCurCpuTimestamp();
    FillTaskParamDmaPub(taskParam, dst, size, dmaOp);
    taskParam.taskPara.DMA.src = src;

    AddTaskCallback(stream, taskId, taskParam);
}

void UbTransportLiteImpl::ReduceProfilingProcess(void *src, void *dst, u64 size,
                                                 const ReduceIn &reduceIn, const StreamLite &stream, u32 taskId)
{
    if (!IsReportTask()) {
        return;
    }

    TaskParam taskParam {};
    taskParam.taskType = TaskParamType::TASK_UB_REDUCE_INLINE;
    taskParam.beginTime = ProfGetCurCpuTimestamp();
    taskParam.taskPara.Reduce.src = src;
    taskParam.taskPara.Reduce.dst = dst;
    taskParam.taskPara.Reduce.size = size;
    taskParam.taskPara.Reduce.notifyID = INVALID_VALUE_NOTIFYID;
    taskParam.taskPara.Reduce.notifyValue = 1;
    taskParam.taskPara.Reduce.linkType = DfxLinkType::UB;
    taskParam.taskPara.Reduce.reduceOp = ConvertReduceOpToHcclReduceOp(reduceIn.reduceOp);
    taskParam.taskPara.Reduce.dataType = DataTypeToHcclDataType(reduceIn.dataType);
    taskParam.taskPara.Reduce.locEid   = GetLocEid();
    taskParam.taskPara.Reduce.rmtEid   = GetRmtEid();

    AddTaskCallback(stream, taskId, taskParam);
}

void UbTransportLiteImpl::WriteWithNotifyProfilingProcess(void *src, void *dst, u64 size, const StreamLite &stream,
                                           u32 taskId, u64 notifyId)
{
    if (!IsReportTask()) {
        return;
    }

    TaskParam taskParam{};
    taskParam.taskType              = TaskParamType::TASK_WRITE_WITH_NOTIFY;
    taskParam.beginTime             = ProfGetCurCpuTimestamp();
    FillTaskParamDmaPub(taskParam, dst, size, DmaOp::HCCL_DMA_WRITE);
    taskParam.taskPara.DMA.src = src;
    taskParam.taskPara.DMA.notifyID = notifyId;
    taskParam.taskPara.DMA.notifyValue = 1;

    AddTaskCallback(stream, taskId, taskParam);
}

void UbTransportLiteImpl::WriteReduceWithNotifyProfilingProcess(void *src, void *dst, u64 size,
                                                 const ReduceIn &reduceIn, const StreamLite &stream, u32 taskId, u64 notifyId)
{
    if (!IsReportTask()) {
        return;
    }

    TaskParam taskParam {};
    taskParam.taskType  = TaskParamType::TASK_WRITE_REDUCE_WITH_NOTIFY;
    taskParam.beginTime = ProfGetCurCpuTimestamp();
    taskParam.taskPara.Reduce.src = src;
    taskParam.taskPara.Reduce.dst = dst;
    taskParam.taskPara.Reduce.size = size;
    taskParam.taskPara.Reduce.notifyID = notifyId;
    taskParam.taskPara.Reduce.notifyValue = 1;
    taskParam.taskPara.Reduce.linkType = DfxLinkType::UB;
    taskParam.taskPara.Reduce.reduceOp = ConvertReduceOpToHcclReduceOp(reduceIn.reduceOp);
    taskParam.taskPara.Reduce.dataType = DataTypeToHcclDataType(reduceIn.dataType);
    taskParam.taskPara.Reduce.locEid   = GetLocEid();
    taskParam.taskPara.Reduce.rmtEid   = GetRmtEid();

    AddTaskCallback(stream, taskId, taskParam);
}

void UbTransportLiteImpl::NotifyRecordProfilingProcess(void *dst, u64 size,
                                                 const StreamLite &stream, u32 taskId, u64 notifyId)
{
    if (!IsReportTask()) {
        return;
    }

    TaskParam taskParam {};
    taskParam.taskType                 = TaskParamType::TASK_UB_INLINE_WRITE;
    taskParam.beginTime                = ProfGetCurCpuTimestamp();
    FillTaskParamDmaPub(taskParam, dst, size, DmaOp::HCCL_DMA_WRITE);
    taskParam.taskPara.DMA.notifyID    = notifyId;
    taskParam.taskPara.DMA.notifyValue = 1;

    AddTaskCallback(stream, taskId, taskParam);
}

void UbTransportLiteImpl::Read(const RmaBufferLite &loc, const Buffer &rmt, const StreamLite &stream)
{
    SqeConfigLite cfg;
    SetFenceConfig(cfg);
    auto taskId = stream.GetRtsq()->GetTaskId();

    // 当前使用1个connection,下标为0
    auto locRmaBufSlicelite = GetRmaBufSlicelite(loc);
    auto rmtRmaBufSlicelite = GetRmtRmaBufSliceLite(rmt);
    connVec[0]->Read(locRmaBufSlicelite, rmtRmaBufSlicelite, cfg, stream, connOut);
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi);

    ProfilingProcess(reinterpret_cast<void *>(locRmaBufSlicelite.GetAddr()),
                     reinterpret_cast<void *>(rmtRmaBufSlicelite.GetAddr()),
                     locRmaBufSlicelite.GetSize(), stream, DmaOp::HCCL_DMA_READ, taskId);
}

void UbTransportLiteImpl::Write(const RmaBufferLite &loc, const Buffer &rmt, const StreamLite &stream)
{
    SqeConfigLite cfg;
    SetFenceConfig(cfg);
    auto taskId = stream.GetRtsq()->GetTaskId();

    // 当前使用1个connection，下标为0
    auto locRmaBufSlicelite = GetRmaBufSlicelite(loc);
    auto rmtRmaBufSlicelite = GetRmtRmaBufSliceLite(rmt);
    connVec[0]->Write(locRmaBufSlicelite, rmtRmaBufSlicelite, cfg, stream, connOut);
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi);

    ProfilingProcess(reinterpret_cast<void *>(locRmaBufSlicelite.GetAddr()),
                     reinterpret_cast<void *>(rmtRmaBufSlicelite.GetAddr()),
                     locRmaBufSlicelite.GetSize(), stream, DmaOp::HCCL_DMA_WRITE, taskId);
}

void UbTransportLiteImpl::ReadReduce(const RmaBufferLite &loc, const Buffer &rmt, const ReduceIn &reduceIn,
                                     const StreamLite &stream)
{
    SqeConfigLite cfg;
    SetFenceConfig(cfg);
    auto taskId = stream.GetRtsq()->GetTaskId();

    // 当前使用1个connection，下标为0
    auto locRmaBufSlicelite = GetRmaBufSlicelite(loc);
    auto rmtRmaBufSlicelite = GetRmtRmaBufSliceLite(rmt);
    connVec[0]->ReadReduce(reduceIn, locRmaBufSlicelite, rmtRmaBufSlicelite, stream, cfg, connOut);
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi);

    ReduceProfilingProcess(reinterpret_cast<void *>(locRmaBufSlicelite.GetAddr()),
                            reinterpret_cast<void *>(rmtRmaBufSlicelite.GetAddr()),
                            locRmaBufSlicelite.GetSize(), reduceIn, stream, taskId);
}

void UbTransportLiteImpl::WriteReduce(const RmaBufferLite &loc, const Buffer &rmt, const ReduceIn &reduceIn,
                                      const StreamLite &stream)
{
    SqeConfigLite cfg;
    SetFenceConfig(cfg);
    auto taskId = stream.GetRtsq()->GetTaskId();

    // 当前使用1个connection，下标为0
    auto locRmaBufSlicelite = GetRmaBufSlicelite(loc);
    auto rmtRmaBufSlicelite = GetRmtRmaBufSliceLite(rmt);
    connVec[0]->WriteReduce(reduceIn.dataType, reduceIn.reduceOp, locRmaBufSlicelite, stream,
                            rmtRmaBufSlicelite, cfg, connOut);
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi);

    ReduceProfilingProcess(reinterpret_cast<void *>(locRmaBufSlicelite.GetAddr()),
                            reinterpret_cast<void *>(rmtRmaBufSlicelite.GetAddr()),
                            locRmaBufSlicelite.GetSize(), reduceIn, stream, taskId);
}

void UbTransportLiteImpl::ExecProfiling(const std::vector<RmaBufferLite> &loc, const std::vector<Buffer> &rmt, 
                const std::vector<BaseTransportLiteImpl::TransferOp> &transferOp, const StreamLite &stream, u32 taskId)
{
    u32 insNum = loc.size();
    u64 totalSize = 0;
    for (u32 i = 0; i < insNum; i++) {
        totalSize += GetRmaBufSlicelite(loc[i]).GetSize();
    }
    if (transferOp[insNum - 1].reduceIn.reduceOp == ReduceOp::INVALID) {
        DmaOp dmaOp = DmaOp::HCCL_DMA_WRITE;
        if (transferOp[insNum - 1].transType == TransferType::READ) {
            dmaOp = DmaOp::HCCL_DMA_READ;
        }
        ProfilingProcess(reinterpret_cast<void *>(GetRmaBufSlicelite(loc[insNum - 1]).GetAddr()),
                         reinterpret_cast<void *>(GetRmtRmaBufSliceLite(rmt[insNum - 1]).GetAddr()),
                         totalSize, stream, dmaOp, taskId);
    } else {
        ReduceProfilingProcess(reinterpret_cast<void *>(GetRmaBufSlicelite(loc[insNum - 1]).GetAddr()),
                               reinterpret_cast<void *>(GetRmtRmaBufSliceLite(rmt[insNum - 1]).GetAddr()),
                               totalSize, transferOp[insNum - 1].reduceIn, stream, taskId);
    }
}

void UbTransportLiteImpl::ExecProfilingAll(const std::vector<RmaBufferLite> &loc, const std::vector<Buffer> &rmt, 
                const std::vector<BaseTransportLiteImpl::TransferOp> &transferOp, const StreamLite &stream, u32 taskId,
                const std::vector<uint32_t> &notifyIdxs)
{
    u32 insNum = loc.size();
    u64 totalSize = 0;
    for (u32 i = 0; i < insNum; i++) {
        totalSize += GetRmaBufSlicelite(loc[i]).GetSize();
    }

    if (transferOp[insNum - 1].transType == TransferType::READ) {
        ProfilingProcess(reinterpret_cast<void *>(GetRmaBufSlicelite(loc[insNum - 1]).GetAddr()),
                         reinterpret_cast<void *>(GetRmtRmaBufSliceLite(rmt[insNum - 1]).GetAddr()),
                         totalSize, stream, DmaOp::HCCL_DMA_READ, taskId);
    } else if (transferOp[insNum - 1].transType == TransferType::WRITE) {
        ProfilingProcess(reinterpret_cast<void *>(GetRmaBufSlicelite(loc[insNum - 1]).GetAddr()),
                         reinterpret_cast<void *>(GetRmtRmaBufSliceLite(rmt[insNum - 1]).GetAddr()),
                         totalSize, stream, DmaOp::HCCL_DMA_WRITE, taskId);
    } else if (transferOp[insNum - 1].transType == TransferType::READ_REDUCE) {
        ReduceProfilingProcess(reinterpret_cast<void *>(GetRmaBufSlicelite(loc[insNum - 1]).GetAddr()),
                               reinterpret_cast<void *>(GetRmtRmaBufSliceLite(rmt[insNum - 1]).GetAddr()),
                               totalSize, transferOp[insNum - 1].reduceIn, stream, taskId);
    } else if (transferOp[insNum - 1].transType == TransferType::WRITE_REDUCE) {
        ReduceProfilingProcess(reinterpret_cast<void *>(GetRmaBufSlicelite(loc[insNum - 1]).GetAddr()),
                               reinterpret_cast<void *>(GetRmtRmaBufSliceLite(rmt[insNum - 1]).GetAddr()),
                               totalSize, transferOp[insNum - 1].reduceIn, stream, taskId);
    } else if (transferOp[insNum - 1].transType == TransferType::WRITE_WITH_NOTIFY) {
        WriteWithNotifyProfilingProcess(reinterpret_cast<void *>(GetRmaBufSlicelite(loc[insNum - 1]).GetAddr()),
                                        reinterpret_cast<void *>(GetRmtRmaBufSliceLite(rmt[insNum - 1]).GetAddr()),
                                        totalSize, stream, taskId, GetRmtNotifySliceLite(notifyIdxs[insNum - 1]).GetAddr());
    } else if (transferOp[insNum - 1].transType == TransferType::WRITE_REDUCE_WITH_NOTIFY) {
        WriteReduceWithNotifyProfilingProcess(reinterpret_cast<void *>(GetRmaBufSlicelite(loc[insNum - 1]).GetAddr()),
                                            reinterpret_cast<void *>(GetRmtRmaBufSliceLite(rmt[insNum - 1]).GetAddr()),
                                            totalSize, transferOp[insNum - 1].reduceIn, stream, taskId, GetRmtNotifySliceLite(notifyIdxs[insNum - 1]).GetAddr());
    } else if (transferOp[insNum - 1].transType == TransferType::NOTIFY_RECORD) {
        NotifyRecordProfilingProcess(reinterpret_cast<void *>(GetRmtNotifySliceLite(notifyIdxs[insNum - 1]).GetAddr()),
                                    GetRmtNotifySliceLite(notifyIdxs[insNum - 1]).GetSize(), stream, taskId, GetRmtNotifySliceLite(notifyIdxs[insNum - 1]).GetAddr());
    }
}

void UbTransportLiteImpl::BatchTransfer(const std::vector<RmaBufferLite> &loc, const std::vector<Buffer> &rmt,
    const std::vector<BaseTransportLiteImpl::TransferOp> &transferOp, const StreamLite &stream)
{
    if (UNLIKELY(loc.empty())) {
        return;
    }
    SqeConfigLite cfg;
    SetFenceConfig(cfg);
    auto taskId = stream.GetRtsq()->GetTaskId();
    u32 insNum = loc.size();
    for (u32 i = 0; i < insNum; i++) {
        cfg.cqeEn     = (i == insNum - 1) ? true : false; // 返回最后一个sqe的cqe
        cfg.placeOdr  = UB_RELAX_ORDER;
        cfg.compOrder = UB_NO_COMPLETION;

        auto localBuffer  = GetRmaBufSlicelite(loc[i]);
        auto remoteBuffer = GetRmtRmaBufSliceLite(rmt[i]);
        if (transferOp[i].transType == TransferType::WRITE) {
            connVec[0]->Write(localBuffer, remoteBuffer, cfg, stream, connOut); // 当前只有一个connection，对应一个jetty 
        } else if (transferOp[i].transType == TransferType::WRITE_REDUCE) { // write reduce
            connVec[0]->WriteReduce(transferOp[i].reduceIn.dataType, transferOp[i].reduceIn.reduceOp, localBuffer,
                        stream, remoteBuffer, cfg, connOut);
        } else if (transferOp[i].transType == TransferType::READ) {
            connVec[0]->Read(localBuffer, remoteBuffer, cfg, stream, connOut); // 当前只有一个connection，对应一个jetty
        } else if (transferOp[i].transType == TransferType::READ_REDUCE) { // read reduce
            connVec[0]->ReadReduce(transferOp[i].reduceIn, localBuffer, remoteBuffer, stream, cfg, connOut);
        }
    }
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi);

    ExecProfiling(loc, rmt, transferOp, stream, taskId);
}

// Convert hccl::HcommDataType => Hccl::DataType, hccl::HcommReduceOp => Hccl::ReduceOp
static const std::unordered_map<HcommReduceOp, Hccl::ReduceOp> mapHcommReduceOpA5 = {
    {HcommReduceOp::HCOMM_REDUCE_SUM,  Hccl::ReduceOp::SUM},
    {HcommReduceOp::HCOMM_REDUCE_PROD, Hccl::ReduceOp::PROD},
    {HcommReduceOp::HCOMM_REDUCE_MAX,  Hccl::ReduceOp::MAX},
    {HcommReduceOp::HCOMM_REDUCE_MIN,  Hccl::ReduceOp::MIN},
    {HcommReduceOp::HCOMM_REDUCE_RESERVED, Hccl::ReduceOp::INVALID}
};

static const std::unordered_map<HcommDataType, Hccl::DataType> mapHcommDataTypeA5 = {
#ifndef OPEN_BUILD_PROJECT
    {HcommDataType::HCOMM_DATA_TYPE_HIF8,     Hccl::DataType::HIF8},
    {HcommDataType::HCOMM_DATA_TYPE_FP8E4M3,  Hccl::DataType::FP8E4M3},
    {HcommDataType::HCOMM_DATA_TYPE_FP8E5M2,  Hccl::DataType::FP8E5M2},
    {HcommDataType::HCOMM_DATA_TYPE_FP8E8M0,  Hccl::DataType::FP8E8M0},
#endif
    {HcommDataType::HCOMM_DATA_TYPE_INT8,     Hccl::DataType::INT8},
    {HcommDataType::HCOMM_DATA_TYPE_INT16,    Hccl::DataType::INT16},
    {HcommDataType::HCOMM_DATA_TYPE_INT32,    Hccl::DataType::INT32},
    {HcommDataType::HCOMM_DATA_TYPE_INT64,    Hccl::DataType::INT64},
    {HcommDataType::HCOMM_DATA_TYPE_INT128,   Hccl::DataType::INT128},
    {HcommDataType::HCOMM_DATA_TYPE_UINT8,    Hccl::DataType::UINT8},
    {HcommDataType::HCOMM_DATA_TYPE_UINT16,   Hccl::DataType::UINT16},
    {HcommDataType::HCOMM_DATA_TYPE_UINT32,   Hccl::DataType::UINT32},
    {HcommDataType::HCOMM_DATA_TYPE_UINT64,   Hccl::DataType::UINT64},
    {HcommDataType::HCOMM_DATA_TYPE_FP16,     Hccl::DataType::FP16},
    {HcommDataType::HCOMM_DATA_TYPE_FP32,     Hccl::DataType::FP32},
    {HcommDataType::HCOMM_DATA_TYPE_FP64,     Hccl::DataType::FP64},
    {HcommDataType::HCOMM_DATA_TYPE_BFP16,    Hccl::DataType::BFP16},
    {HcommDataType::HCOMM_DATA_TYPE_RESERVED, Hccl::DataType::INVALID}
};

static HcclResult CheckReduceHcommDataTypeAndHcommReduceOp(HcommDataType dataType, HcommReduceOp reduceOp)
{
    auto dataTypeIt = mapHcommDataTypeA5.find(dataType); // reduce类型，dataType不能是RESERVED
    if (dataTypeIt == mapHcommDataTypeA5.end() || dataTypeIt->first == HcommDataType::HCOMM_DATA_TYPE_RESERVED) {
        HCCL_ERROR("[%s] type[%u] is not supported.", __func__, dataType);
        return HCCL_E_PARA;
    }

    auto reduceOpIt = mapHcommReduceOpA5.find(reduceOp); // reduce类型，reduceOp不能是RESERVED
    if (reduceOpIt == mapHcommReduceOpA5.end() || reduceOpIt->first == HcommReduceOp::HCOMM_REDUCE_RESERVED) {
        HCCL_ERROR("[%s] op[%u] is not supported.", __func__, reduceOp);
        return HCCL_E_PARA;
    }

    return HCCL_SUCCESS;
}

constexpr u32 SIZE_TABLE[HCCL_DATA_TYPE_RESERVED] = {sizeof(s8), sizeof(s16), sizeof(s32),
    2, sizeof(float), sizeof(s64), sizeof(u64), sizeof(u8), sizeof(u16), sizeof(u32),
    8, 2, 16, 2, 1, 1, 1, 1};

static HcclResult ParasReduceData(const HcommBatchTransferDesc &transferDesc, uint64_t &len, 
 	                                     HcommDataType &dataType, HcommReduceOp &reduceOp)
{
    len = transferDesc.transferInfo.reduce.count;
    dataType = transferDesc.transferInfo.reduce.dataType;
    reduceOp = transferDesc.transferInfo.reduce.reduceOp;
    auto ret = CheckReduceHcommDataTypeAndHcommReduceOp(dataType, reduceOp);
    CHK_PRT_RET(ret != HCCL_SUCCESS,
        HCCL_ERROR("FAIL at CheckReduceHcommDataTypeAndHcommReduceOp dataType[%d], reduceOp[%d].", dataType, reduceOp), ret);
    return HCCL_SUCCESS;
}

static HcclResult ParseData(const HcommBatchTransferDesc &transferDesc, void* &rmt, void* &loc,
                        uint64_t &len, Hccl::TransferType &tfType, HcommDataType &dataType, HcommReduceOp &reduceOp, uint32_t &notifyIdx)
{
    if (transferDesc.transType == HCOMM_TRANSFER_TYPE_WRITE) {
        rmt = transferDesc.transferInfo.write.dst; // write操作，dst是远端地址
        loc = transferDesc.transferInfo.write.src; // src是本端地址
        len = transferDesc.transferInfo.write.len;
        tfType = Hccl::TransferType::WRITE;
    } else if (transferDesc.transType == HCOMM_TRANSFER_TYPE_READ) {
        rmt = transferDesc.transferInfo.read.src; // read操作，src是远端地址
        loc = transferDesc.transferInfo.read.dst; // dst是本端地址
        len = transferDesc.transferInfo.read.len;
        tfType = Hccl::TransferType::READ;
    } else if (transferDesc.transType == HCOMM_TRANSFER_TYPE_WRITE_REDUCE) {
        rmt = transferDesc.transferInfo.reduce.dst;
        loc = transferDesc.transferInfo.reduce.src;
        tfType = Hccl::TransferType::WRITE_REDUCE;
        CHK_RET(ParasReduceData(transferDesc, len, dataType, reduceOp));
    } else if (transferDesc.transType == HCOMM_TRANSFER_TYPE_READ_REDUCE) {
        rmt = transferDesc.transferInfo.reduce.src;
        loc = transferDesc.transferInfo.reduce.dst;
        tfType = Hccl::TransferType::READ_REDUCE;
        CHK_RET(ParasReduceData(transferDesc, len, dataType, reduceOp));
    } else if (transferDesc.transType == HCOMM_TRANSFER_TYPE_WRITE_WITH_NOTIFY) {
        rmt = transferDesc.transferInfo.writeWithNotify.dst; // write操作，dst是远端地址
        loc = transferDesc.transferInfo.writeWithNotify.src; // src是本端地址
        len = transferDesc.transferInfo.writeWithNotify.len;
        notifyIdx = transferDesc.transferInfo.writeWithNotify.notifyIdx;
        tfType = Hccl::TransferType::WRITE_WITH_NOTIFY;
    } else if (transferDesc.transType == HCOMM_TRANSFER_TYPE_WRITE_REDUCE_WITH_NOTIFY) {
        rmt = transferDesc.transferInfo.writeReduceWithNotify.dst;
        loc = transferDesc.transferInfo.writeReduceWithNotify.src;
        len = transferDesc.transferInfo.writeReduceWithNotify.count;
        dataType = transferDesc.transferInfo.writeReduceWithNotify.dataType;
        reduceOp = transferDesc.transferInfo.writeReduceWithNotify.reduceOp;
        notifyIdx = transferDesc.transferInfo.writeReduceWithNotify.notifyIdx;
        tfType = Hccl::TransferType::WRITE_REDUCE_WITH_NOTIFY;
        CHK_RET(CheckReduceHcommDataTypeAndHcommReduceOp(dataType, reduceOp));
    } else if (transferDesc.transType == HCOMM_TRANSFER_TYPE_NOTIFY_RECORD) {
        notifyIdx = transferDesc.transferInfo.notifyRecord.notifyIdx;
        tfType = Hccl::TransferType::NOTIFY_RECORD;
    } else {
        HCCL_ERROR("[%s] unsupported transType[%d]", __func__, transferDesc.transType);
        return HCCL_E_NOT_SUPPORT;
    }   
    if (reduceOp != HcommReduceOp::HCOMM_REDUCE_RESERVED) { // 对于规约类型, size = count * sizeof(datatype)
        len = len * SIZE_TABLE[dataType];
    }
    return HCCL_SUCCESS;
}
constexpr uint32_t       NOTIFYIDX_INVALID_VALUE  = 0xFFFFFFFF; // NOTIFY idex非法值
HcclResult UbTransportLiteImpl::ExecuteBatchTransfer(StreamLite *streamLitePtr,
    const HcommBatchTransferDesc *transferDescs, uint32_t transferDescNum)
{
    std::vector<Hccl::RmaBufferLite> locSlices;
    std::vector<Hccl::Buffer> rmtSlices;
    std::vector<Hccl::BaseTransportLiteImpl::TransferOp> transferOps;
    std::vector<uint32_t> notifyIdxs;

    locSlices.reserve(transferDescNum);
    rmtSlices.reserve(transferDescNum);
    transferOps.reserve(transferDescNum);
    notifyIdxs.reserve(transferDescNum);

    for (uint32_t i = 0; i < transferDescNum; i++) {
        Hccl::RmaBufferLite locRmaBuf;
        void *rmt = nullptr;
        void *loc = nullptr;
        uint64_t len = 0;
        Hccl::TransferType tfType;
        HcommDataType dataType{HcommDataType::HCOMM_DATA_TYPE_RESERVED};
        HcommReduceOp reduceOp{HcommReduceOp::HCOMM_REDUCE_RESERVED};
        uint32_t notifyIdx = NOTIFYIDX_INVALID_VALUE;
        CHK_RET(ParseData(transferDescs[i], rmt, loc, len, tfType, dataType, reduceOp, notifyIdx));
        if (tfType != Hccl::TransferType::NOTIFY_RECORD) { // NOTIFY_RECORD时没有地址字段
            CHK_PTR_NULL(rmt);
            CHK_PTR_NULL(loc);
            HcclResult ret = BuildLocRmaBufferLite(reinterpret_cast<uintptr_t>(loc), len, locRmaBuf);
            CHK_PRT_RET(ret != HCCL_SUCCESS,
                HCCL_ERROR("[%s] FAIL at BuildLocRmaBufferLite for index %u. rmt[%p], loc[%p], len[0x%llx], tfType[%u], dataType[%d], reduceOp[%d].",
                __func__, i, rmt, loc, len, tfType, dataType, reduceOp), ret);
        }
        if (tfType == Hccl::TransferType::NOTIFY_RECORD || tfType == Hccl::TransferType::WRITE_WITH_NOTIFY
            || tfType == Hccl::TransferType::WRITE_REDUCE_WITH_NOTIFY) {
            CHK_PRT_RET(notifyIdx == NOTIFYIDX_INVALID_VALUE,
                HCCL_ERROR("[%s] FAIL at ParseData for index %u. tfType[%u], notifyIdx[%u].",
                __func__, i, tfType, notifyIdx), HCCL_E_PARA);
        }
        notifyIdxs.push_back(notifyIdx);
        locSlices.push_back(locRmaBuf);

        const Hccl::Buffer rmtBuf{reinterpret_cast<uintptr_t>(rmt), len};
        rmtSlices.push_back(rmtBuf);

        Hccl::ReduceIn reduceIn{mapHcommDataTypeA5.at(dataType), mapHcommReduceOpA5.at(reduceOp)};

        transferOps.push_back(Hccl::BaseTransportLiteImpl::TransferOp{tfType, reduceIn});

        HCCL_DEBUG("[%s] Prepared transfer op for index %u. rmt[%p], loc[%p], len[0x%llx], tfType[%u], dataType[%d], reduceOp[%d].",
            __func__, i, rmt, loc, len, tfType, dataType, reduceOp);
    }
    EXCEPTION_CATCH(BatchTransferAll(locSlices, rmtSlices, transferOps, notifyIdxs, *streamLitePtr), return HCCL_E_INTERNAL);
    return HCCL_SUCCESS;
}

void UbTransportLiteImpl::BatchTransferAll(const std::vector<RmaBufferLite> &loc, const std::vector<Buffer> &rmt,
    const std::vector<BaseTransportLiteImpl::TransferOp> &transferOp, const std::vector<uint32_t> &notifyIdxs, const StreamLite &stream)
{
    if (UNLIKELY(loc.empty())) {
        return;
    }
    auto taskId = stream.GetRtsq()->GetTaskId();
    u64  notifyData = 1;          // 普通notify，固定1，用于writeWithNotify与writeReduceWithNotify
    SqeConfigLite cfg;
    SetFenceConfig(cfg);
    u32 insNum = loc.size();
    for (u32 i = 0; i < insNum; i++) {
        cfg.cqeEn     = (i == insNum - 1) ? true : false; // 返回最后一个sqe的cqe
        cfg.placeOdr  = (i == insNum - 1) ? UB_STRONG_ORDER : UB_RELAX_ORDER; // 最后一个要求保序
        cfg.compOrder = (i == insNum - 1) ? UB_COMPLETION : UB_NO_COMPLETION;

        if (transferOp[i].transType == TransferType::NOTIFY_RECORD) { // notifyRecord操作没有loc/rmt，因此单独处理
            if (notifyIdxs[i] == 1) { // PostFin场景
                cfg.cqeEn     = true;
                cfg.placeOdr  = UB_STRONG_ORDER;
                cfg.compOrder = UB_COMPLETION;
            }
            u32           inlineData = 1;
            // 当前使用1个connection，下标为0 构建sqe
            connVec[0]->InlineWrite(reinterpret_cast<u8 *>(&inlineData), UB_INLINE_WRITE_SIZE, GetRmtNotifySliceLite(notifyIdxs[i]),
                                    cfg, stream, connOut);
        } else {
            auto localBuffer  = GetRmaBufSlicelite(loc[i]);
            auto remoteBuffer = GetRmtRmaBufSliceLite(rmt[i]);

            if (transferOp[i].transType == TransferType::WRITE) {
                connVec[0]->Write(localBuffer, remoteBuffer, cfg, stream, connOut);
            } else if (transferOp[i].transType == TransferType::WRITE_REDUCE) {
                connVec[0]->WriteReduce(transferOp[i].reduceIn.dataType, transferOp[i].reduceIn.reduceOp, localBuffer, stream, remoteBuffer, cfg, connOut);
            } else if (transferOp[i].transType == TransferType::READ) {
                connVec[0]->Read(localBuffer, remoteBuffer, cfg, stream, connOut);
            } else if (transferOp[i].transType == TransferType::READ_REDUCE) {
                connVec[0]->ReadReduce(transferOp[i].reduceIn, localBuffer, remoteBuffer, stream, cfg, connOut);
            } else if (transferOp[i].transType == TransferType::WRITE_WITH_NOTIFY) {
                connVec[0]->WriteWithNotify(localBuffer, remoteBuffer, cfg, connOut, GetRmtNotifySliceLite(notifyIdxs[i]), stream, notifyData); // 当前使用1个connection，下标为0
            } else if (transferOp[i].transType == TransferType::WRITE_REDUCE_WITH_NOTIFY) {
                connVec[0]->WriteReduceWithNotify(transferOp[i].reduceIn.dataType, transferOp[i].reduceIn.reduceOp, localBuffer,
                                                remoteBuffer, cfg, stream, connOut, GetRmtNotifySliceLite(notifyIdxs[i]), notifyData); // 当前使用1个connection，下标为0
            }
        }
    }
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi); // 约束使用一批wqe的个数不会导致反压

    ExecProfilingAll(loc, rmt, transferOp, stream, taskId, notifyIdxs);
}

void UbTransportLiteImpl::WriteWithNotify(const RmaBufferLite &loc, const Buffer &rmt, const WithNotifyIn &withNotify,
                                          const StreamLite &stream)
{
    SqeConfigLite cfg;
    SetFenceConfig(cfg);
    u64           notifyData = 1; // 普通notify，固定1
    auto taskId = stream.GetRtsq()->GetTaskId();

    // 当前使用1个connection，下标为0
    auto locRmaBufSlicelite = GetRmaBufSlicelite(loc);
    auto rmtRmaBufSlicelite = GetRmtRmaBufSliceLite(rmt);
    auto rmtNotifySliceLite = GetRmtNotifySliceLite(withNotify.index_);
    connVec[0]->WriteWithNotify(locRmaBufSlicelite, rmtRmaBufSlicelite, cfg, connOut,
                                rmtNotifySliceLite, stream, notifyData);
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi);

    if (!IsReportTask()) {
        return;
    }

    TaskParam taskParam{};
    taskParam.taskType              = TaskParamType::TASK_WRITE_WITH_NOTIFY;
    taskParam.beginTime             = ProfGetCurCpuTimestamp();
    taskParam.taskPara.DMA.src      = reinterpret_cast<void *>(locRmaBufSlicelite.GetAddr());
    taskParam.taskPara.DMA.dst      = reinterpret_cast<void *>(rmtRmaBufSlicelite.GetAddr());
    taskParam.taskPara.DMA.size     = locRmaBufSlicelite.GetSize();
    taskParam.taskPara.DMA.notifyID = rmtNotifySliceLite.GetNotifyId();
    taskParam.taskPara.DMA.notifyValue = 1;
    taskParam.taskPara.DMA.linkType = DfxLinkType::UB;
    taskParam.taskPara.DMA.dmaOp    = DmaOp::HCCL_DMA_WRITE;
    taskParam.taskPara.DMA.locEid = GetLocEid();
    taskParam.taskPara.DMA.rmtEid = GetRmtEid();

    AddTaskCallback(stream, taskId, taskParam);
}

void UbTransportLiteImpl::WriteReduceWithNotify(const RmaBufferLite &loc, const Buffer &rmt, const ReduceIn &reduceIn,
                                                const WithNotifyIn &withNotify, const StreamLite &stream)
{
    SqeConfigLite cfg;
    SetFenceConfig(cfg);
    u64           notifyData = 1;                               // 普通notify，固定1
    auto taskId = stream.GetRtsq()->GetTaskId();

    // 当前使用1个connection，下标为0
    auto locRmaBufSlicelite = GetRmaBufSlicelite(loc);
    auto rmtRmaBufSlicelite = GetRmtRmaBufSliceLite(rmt);
    auto rmtNotifySliceLite = GetRmtNotifySliceLite(withNotify.index_);
    connVec[0]->WriteReduceWithNotify(reduceIn.dataType, reduceIn.reduceOp, locRmaBufSlicelite,
                                      rmtRmaBufSlicelite, cfg, stream, connOut, rmtNotifySliceLite,
                                      notifyData);
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi);
    if (!IsReportTask()) {
        return;
    }

    TaskParam taskParam{};
    taskParam.taskType                 = TaskParamType::TASK_WRITE_REDUCE_WITH_NOTIFY;
    taskParam.beginTime                = ProfGetCurCpuTimestamp();
    taskParam.taskPara.Reduce.src      = reinterpret_cast<void *>(locRmaBufSlicelite.GetAddr());
    taskParam.taskPara.Reduce.dst      = reinterpret_cast<void *>(rmtRmaBufSlicelite.GetAddr());
    taskParam.taskPara.Reduce.size     = locRmaBufSlicelite.GetSize();
    taskParam.taskPara.Reduce.notifyID = rmtNotifySliceLite.GetNotifyId();
    taskParam.taskPara.Reduce.notifyValue = 1;
    taskParam.taskPara.Reduce.linkType = DfxLinkType::UB;
    taskParam.taskPara.Reduce.reduceOp = ConvertReduceOpToHcclReduceOp(reduceIn.reduceOp);
    taskParam.taskPara.Reduce.dataType = DataTypeToHcclDataType(reduceIn.dataType);
    taskParam.taskPara.Reduce.locEid   = GetLocEid();
    taskParam.taskPara.Reduce.rmtEid   = GetRmtEid();

    AddTaskCallback(stream, taskId, taskParam);
}

void UbTransportLiteImpl::BatchOneSidedRead(const vector<RmaBufSliceLite> &loc, const vector<RmtRmaBufSliceLite> &rmt,
        const StreamLite &stream)
{
    SqeConfigLite cfg;
    SetFenceConfig(cfg);

    // 当前使用1个connection，下标为0
    connVec[0]->BatchOneSidedRead(loc, rmt, cfg, stream, connOut);
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi);
}

void UbTransportLiteImpl::BatchOneSidedWrite(const vector<RmaBufSliceLite> &loc, const vector<RmtRmaBufSliceLite> &rmt,
        const StreamLite &stream)
{
    SqeConfigLite cfg;
    SetFenceConfig(cfg);

    // 当前使用1个connection，下标为0
    connVec[0]->BatchOneSidedWrite(loc, rmt, cfg, stream, connOut);
    BuildUbDbSendTask(stream, connVec[0]->GetUbJettyLiteId(), connOut.pi);
}

 
Eid UbTransportLiteImpl::GetLocEid() const
{
    return connVec[0]->GetLocEid();
}

Eid UbTransportLiteImpl::GetRmtEid() const
{
    return connVec[0]->GetRmtEid();
}

HcclResult UbTransportLiteImpl::Clean()
{
    locNotifyVec.clear();
    rmtNotifyVec.clear();
    locBufferMap.clear();
    rmtBufferVec.clear();
    rmtBufferMap.clear();

    // 清理connVec，connLite由UbConnLiteMgr管理
    for (auto &it : connUniqueIdVec) {
       DECTOR_TRY_CATCH("UbTransportLiteImpl",  UbConnLiteMgr::GetInstance().Clear(it));
    }
    connUniqueIdVec.clear();
    connVec.clear();

    return HCCL_SUCCESS;
}

HcclResult UbTransportLiteImpl::Resume(std::vector<char> &uniqueId)
{
    Init(uniqueId);
    return HCCL_SUCCESS;
}

HcclResult UbTransportLiteImpl::Fence()
{
    fence_ = true;
    HCCL_INFO("[%s] SUCCESS. fence[%d]", __func__, fence_);
    return HCCL_SUCCESS;
}

void UbTransportLiteImpl::SetFenceConfig(SqeConfigLite &cfg)
{
    if (fence_) {
        cfg.fence = UB_FENCE_ENABLED;
        cfg.placeOdr  = UB_STRONG_ORDER;
        cfg.compOrder = UB_COMPLETION;
    }
    fence_ = false;
}

bool UbTransportLiteImpl::IsReportTask()
{
    return (taskExceptionEnable_ || ProfilingHandlerLite::GetInstance().GetProfL1State()) &&
           (callback_ != nullptr || newCallback_ != nullptr);
}
} // namespace Hccl
