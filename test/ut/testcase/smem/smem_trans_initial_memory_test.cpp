/*
* Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
* MemFabric_Hybrid is licensed under Mulan PSL v2.
* You can use this software according to the terms and conditions of the Mulan PSL v2.
* You may obtain a copy of Mulan PSL v2 at:
*          http://license.coscl.org.cn/MulanPSL2
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
* See the Mulan PSL v2 for more details.
*/
#include <gtest/gtest.h>
#include <mockcpp/mockcpp.hpp>
#include <cstring>
#include <future>
#include <unordered_map>
#include <vector>

#include "smem_trans.h"
#include "hybm_big_mem.h"
#include "smem_store_factory.h"
#include "smem_tcp_config_store.h"
#define private public
#include "smem_trans/smem_trans_entry.h"
#include "smem_trans/smem_trans_entry_manager.h"
#undef private

extern bool g_smemTransInited;

using namespace ock::smem;
#define MOCKER_CPP(api, TT) MOCKCPP_NS::mockAPI(#api, reinterpret_cast<TT>(api))

namespace {
constexpr uintptr_t INITIAL_ADDRESS = 0x200000UL;
constexpr uintptr_t SECOND_ADDRESS = 0x800000UL;
constexpr size_t INITIAL_BYTES = 2UL * 1024UL * 1024UL;
constexpr uint32_t EXPORT_BYTES = 16U;
constexpr uint16_t TEST_PORT = 5321U;
constexpr uint32_t TEST_RANK = 0U;
SmemTransEntry *g_initialEntry = nullptr;
std::unordered_map<uintptr_t, SmemTransEntryPtr> g_initialEntries;
uint32_t g_registerCalls = 0U;
uint32_t g_destroyCalls = 0U;
uint32_t g_joinCalls = 0U;
uint32_t g_failRegistration = 0U;
int g_joinResult = SM_OK;
RankFullInfo g_initialJoinInfo;

int32_t CreateInitialEntry(SmemTransEntryManager *, const std::string &name, const std::string &,
                          const smem_trans_config_t &config, SmemTransEntryPtr &entry)
{
    StoreBackendPtr backend;
    StorePtr store = new TcpConfigStore(backend, "127.0.0.1", TEST_PORT, CSM_CLIENT, true);
    std::string localName = name;
    entry = SmMakeRef<SmemTransEntry>(config, localName, TEST_RANK, TEST_RANK, store);
    g_initialEntry = entry.Get();
    g_initialEntries.emplace(reinterpret_cast<uintptr_t>(entry.Get()), entry);
    return SM_OK;
}

int32_t RemoveInitialEntry(SmemTransEntryManager *, uintptr_t handle)
{
    g_initialEntries.erase(handle);
    return SM_OK;
}

int32_t InitializeInitialEntry(SmemTransEntry *entry)
{
    entry->entity_ = reinterpret_cast<hybm_entity_t>(INITIAL_ADDRESS);
    entry->entityInfo_ = {};
    return SM_OK;
}

hybm_mem_slice_t RegisterInitialRegion(hybm_entity_t, const void *address, uint64_t, uint32_t)
{
    ++g_registerCalls;
    if (g_registerCalls == g_failRegistration) {
        return nullptr;
    }
    return const_cast<void *>(address);
}

void *InitialSliceAddress(hybm_entity_t, hybm_mem_slice_t slice)
{
    return slice;
}

uint64_t InitialSliceSize(hybm_entity_t, hybm_mem_slice_t)
{
    return INITIAL_BYTES;
}

int32_t ExportInitialRegion(hybm_entity_t, hybm_mem_slice_t, uint32_t, hybm_exchange_info *info)
{
    info->descLen = EXPORT_BYTES;
    info->desc = new uint8_t[EXPORT_BYTES]{};
    return SM_OK;
}

void DestroyInitialEntity(hybm_entity_t, uint32_t)
{
    ++g_destroyCalls;
}

int JoinInitialGroup(TcpConfigStore *, const RankFullInfo &info)
{
    ++g_joinCalls;
    g_initialJoinInfo = info;
    if (g_joinResult == SM_OK) {
        g_initialEntry->joinComplete_->set_value();
    }
    return g_joinResult;
}

class TransInitialMemoryTest : public testing::Test {
protected:
    void SetUp() override
    {
        wasInited_ = g_smemTransInited;
        g_smemTransInited = true;
        g_registerCalls = g_destroyCalls = g_joinCalls = g_failRegistration = 0U;
        g_joinResult = SM_OK;
        g_initialJoinInfo = {};
        ASSERT_EQ(smem_trans_config_init(&config_), SM_OK);
        InstallEntryMocks();
        InstallMemoryMocks();
    }

    void TearDown() override
    {
        g_smemTransInited = wasInited_;
        g_initialEntries.clear();
        GlobalMockObject::verify();
        GlobalMockObject::reset();
        g_initialEntry = nullptr;
    }

    void InstallEntryMocks()
    {
        MOCKER_CPP(&SmemTransEntryManager::CreateEntryByName,
                   int32_t (*)(SmemTransEntryManager *, const std::string &, const std::string &,
                              const smem_trans_config_t &, SmemTransEntryPtr &))
            .stubs().will(invoke(CreateInitialEntry));
        MOCKER_CPP(&SmemTransEntry::Initialize, int32_t (*)(SmemTransEntry *))
            .stubs().will(invoke(InitializeInitialEntry));
        MOCKER_CPP(&SmemTransEntryManager::RemoveEntryByPtr,
                   int32_t (*)(SmemTransEntryManager *, uintptr_t))
            .stubs().will(invoke(RemoveInitialEntry));
        MOCKER_CPP(&TcpConfigStore::Join, int (*)(TcpConfigStore *, const RankFullInfo &))
            .stubs().will(invoke(JoinInitialGroup));
        MOCKER_CPP(&TcpConfigStore::ExtendMemory, int (*)(TcpConfigStore *, const MultiBytes &))
            .expects(never());
    }

    void InstallMemoryMocks()
    {
        MOCKER(&hybm_register_local_memory).stubs().will(invoke(RegisterInitialRegion));
        MOCKER(&hybm_get_slice_va).stubs().will(invoke(InitialSliceAddress));
        MOCKER(&hybm_get_slice_size).stubs().will(invoke(InitialSliceSize));
        MOCKER(&hybm_export).stubs().will(invoke(ExportInitialRegion));
        MOCKER(&hybm_destroy_entity).stubs().will(invoke(DestroyInitialEntity));
    }

    SmemTransEntryPtr Create(const std::vector<std::pair<const void *, size_t>> &memories)
    {
        return SmemTransEntry::Create("127.0.0.1:5321", "tcp://127.0.0.1:5321", config_, memories);
    }

    std::vector<std::pair<const void *, size_t>> OneRegion() const
    {
        return {{reinterpret_cast<void *>(INITIAL_ADDRESS), INITIAL_BYTES}};
    }

    smem_trans_config_t config_{};
    bool wasInited_ = false;
};

TEST_F(TransInitialMemoryTest, InitialRegionsArePublishedByJoin)
{
    auto entry = Create(OneRegion());
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(g_registerCalls, 1U);
    EXPECT_EQ(g_joinCalls, 1U);
    ASSERT_EQ(g_initialJoinInfo.externalInfo.size(), 1U);
    const auto &bytes = g_initialJoinInfo.externalInfo.front();
    SmemTransExchangeInfo info{};
    ASSERT_GE(bytes.size(), sizeof(info.u) + sizeof(uint32_t));
    std::memcpy(&info.u, bytes.data(), sizeof(info.u));
    EXPECT_EQ(info.u.address.address, reinterpret_cast<void *>(INITIAL_ADDRESS));
    EXPECT_EQ(info.u.address.size, INITIAL_BYTES);
    EXPECT_TRUE(entry->registedInfo_.empty());
    EXPECT_EQ(entry->exportedSliceIds_.size(), 1U);
}

TEST_F(TransInitialMemoryTest, LegacyCreateUsesEmptyInitialList)
{
    auto handle = smem_trans_create("tcp://127.0.0.1:5321", "127.0.0.1:5321", &config_);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(g_registerCalls, 0U);
    EXPECT_EQ(g_joinCalls, 1U);
    EXPECT_TRUE(g_initialJoinInfo.externalInfo.empty());
    smem_trans_destroy(handle, 0U);
    EXPECT_EQ(g_destroyCalls, 1U);
}

TEST_F(TransInitialMemoryTest, CApiCarriesInitialRegionsIntoJoin)
{
    void *addresses[] = {reinterpret_cast<void *>(INITIAL_ADDRESS)};
    size_t bytes[] = {INITIAL_BYTES};
    auto handle = smem_trans_create_with_memories("tcp://127.0.0.1:5321", "127.0.0.1:5321",
                                                   &config_, addresses, bytes, 1U);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(g_registerCalls, 1U);
    EXPECT_EQ(g_initialJoinInfo.externalInfo.size(), 1U);
    smem_trans_destroy(handle, 0U);
    EXPECT_EQ(g_destroyCalls, 1U);
}

TEST_F(TransInitialMemoryTest, InvalidInitialArrayDoesNotCreateEntry)
{
    size_t bytes[] = {INITIAL_BYTES};
    EXPECT_EQ(smem_trans_create_with_memories("tcp://127.0.0.1:5321", "127.0.0.1:5321",
                                               &config_, nullptr, bytes, 1U), nullptr);
    EXPECT_EQ(g_joinCalls, 0U);
    EXPECT_EQ(g_registerCalls, 0U);
}

TEST_F(TransInitialMemoryTest, RegistrationFailureDestroysPartialEntryWithoutJoin)
{
    g_failRegistration = 2U;
    auto regions = OneRegion();
    regions.emplace_back(reinterpret_cast<void *>(SECOND_ADDRESS), INITIAL_BYTES);
    EXPECT_EQ(Create(regions), nullptr);
    EXPECT_EQ(g_registerCalls, 2U);
    EXPECT_EQ(g_joinCalls, 0U);
    EXPECT_EQ(g_destroyCalls, 1U);
}

TEST_F(TransInitialMemoryTest, JoinFailureReleasesEntryAndCanRetry)
{
    g_joinResult = SM_ERROR;
    EXPECT_EQ(Create(OneRegion()), nullptr);
    EXPECT_EQ(g_destroyCalls, 1U);
    g_joinResult = SM_OK;
    auto entry = Create(OneRegion());
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(g_joinCalls, 2U);
    EXPECT_EQ(g_registerCalls, 2U);
    EXPECT_EQ(g_initialJoinInfo.externalInfo.size(), 1U);
}

TEST_F(TransInitialMemoryTest, DuplicateRegistrationDoesNotRepublish)
{
    auto entry = Create(OneRegion());
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->RegisterLocalMemory(reinterpret_cast<void *>(INITIAL_ADDRESS), INITIAL_BYTES, 0U), SM_OK);
    EXPECT_TRUE(entry->registedInfo_.empty());
    EXPECT_EQ(entry->exportedSliceIds_.size(), 1U);
    EXPECT_EQ(g_joinCalls, 1U);
}
} // namespace
