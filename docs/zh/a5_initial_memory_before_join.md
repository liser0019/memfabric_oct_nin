# A5 DEVICE_URMA 初始内存注册候选

## 基线与目的

基于 `memfabric_oct_nin/main` 的 `3a6dfcc0`，其官方基线为 1.3.0 的 `bbd730b2`，
包含此前已合并的 A5 纯 DEVICE_URMA Host 注册放行补丁。
保留官方 `offload.Scene.SHARED`、VMM 分配和映射；没有加入内存类型、DDR/P2P_DDR 标签或借用 ACL 映射的实验修改。

这次只验证一个软件缺口：延迟连接先 Join、后重放数据 MR，会使通道创建时没有完整的初始内存表。
服务器对 HCOMM 9.1 的分析和日志提示已建 AICPU 对象不会随 Host 侧内存更新自动刷新。
本候选尚未在 A5 上验证，不能宣称它已解决 SHARED 超时的全部原因。

## 修改后的顺序

```text
TransferEngine.register_memory：记录地址和容量
    ↓ 首次访问该对端
锁定注册快照
    ↓
smem_trans_create_with_memories
    Initialize：创建本地实体、endpoint、标志区和初始通知资源
    PrepareLocalMemories：本地注册与导出初始数据区
    Join：将初始切片放入 externalInfo
    对端导入切片 → 创建设备通道
    ↓
发布可用连接，解锁注册快照
    ↓
执行原有读写
```

原 C API 仍保留，转调初始区域为零的新接口。Python 方法签名不变。
注册、反注册和延迟创建遵循同一注册锁；连接发布在释放注册锁之前完成。
因此并发注册要么进入本次初始快照，要么在连接发布后得到明确结果，不会在中间窗口丢失。
缓存按地址、容量去重，重连重新使用当前快照。
初始注册或 Join 失败时不发布连接，由条目析构清理导出描述和 HYBM 实体；下次调用可以重试。

## 通知区与运行期限制

`DeviceUrmaTransportManager::OpenDevice()` 已通过 `InitNotifyPoolLocked()` 初始化一个通知槽，
并在创建通道前调用 HCOMM 注册它。标志区也在 endpoint 初始化阶段注册，因此无需再造一套通知区。

本候选没有实现 HCOMM 的设备内存表热更新，也没有修改官方通知池扩容实现：

- Python 延迟连接建立后新增注册区域会明确返回错误；缓存内的同地址、同容量重复注册可复用。
- 原 C 动态注册接口及直接句柄路径保持原发布流程，不能把返回成功当作设备表已刷新。
- 初始通知池仍为一个槽，耗尽后仍按官方流程扩容。新增槽是否被已有 AICPU 表采用不在本轮保证范围内。
  HCOMM 9.1 首轮验收应先按每个实体单个在途操作测试，日志记录实际通知地址，确认命中初始化时的条目。
- 连接销毁后重新建立会重新收集初始注册内存。仅调用旧的 `smem_trans_deregister_mem` 并不真正注销 MR，
  数据内存必须保持存活直到其连接销毁。并发 destroy / transfer 不属于本候选新增的保证。

后续支持运行期新增 MR 或通知扩容，需要独立实现并验证设备表更新或通道重建，不能只调用 Host 更新接口。

## 本地验证

CPU 回归覆盖：初始内存随真实 Join 序列化、旧 C 接口、非法初始数组、部分注册失败、Join 失败和重试、
重复注册去重。通知和 DEVICE_URMA 的现有 CPU 用例用于检查初始化顺序修改没有破坏原有行为。

```bash
git submodule update --init --recursive
MF_BUILD_JOBS=4 bash script/run_ut.sh --fast TransInitialMemoryTest
# 可继续运行 DeviceUrmaTransportManagerTest 与 SmemTransTest 中的相关重连用例。
```

本地结果：17 项针对性 CPU 测试通过；Python 适配器 C++ 语法检查和 `git diff --check` 通过。
本地使用 ASan；UBSan 仅通过临时抑制文件过滤第三方 mockcpp `JmpCode.cpp` 已有的跳板对齐告警。
更广的 URMA 回归在未修改的 `hybm_transport_common.h` 中触发未初始化 `hybm_role_type` 告警，
该轮未完成，不计为通过。没有执行 A5/CANN 实机或 URMA 端到端验证。

本机缺少 RDMA 头文件、`dos2unix`，且系统 `MAP_HUGE_1GB` 宏与官方局部常量重名。
CPU 测试通过本地构建参数关闭 HCOM RDMA，使用 `/tmp` 下临时工具和预包含头绕过这些环境问题，
这些辅助文件及构建缓存均未提交；服务器正常环境不需要套用它们。

## 服务器构建与隔离安装

本候选修改了 SMEM 和 Python 适配器，HYBM 保持当前 main 的实现，必须使用同一次构建的配套产物。
禁止仅替换 `libmf_hybm_core.so`，也不要预加载上一轮的旧 MF 库。

在已配置 CANN 9.1 的构建环境执行：

```bash
git clone --branch fix/a5-initial-memory-before-join --single-branch   https://github.com/liser0019/memfabric_oct_nin.git mf_initial_memory_candidate
cd mf_initial_memory_candidate
git submodule update --init --recursive
MF_BUILD_JOBS=4 bash script/build_and_pack_run.sh --build_mode RELEASE --build_python ON --xpu_type NPU
```

记录 `git rev-parse HEAD`。新建独立虚拟环境，使用该次 `output/memfabric_hybrid/wheel/` 下的 wheel 安装：

```bash
python3 -m venv --system-site-packages .venv-candidate
.venv-candidate/bin/python -m pip install --no-deps --force-reinstall output/memfabric_hybrid/wheel/*.whl
```

选择与现场原环境一致的 Python 和 C++ ABI；wrapper 默认旧 ABI，如原环境需要新 ABI，增加 `--build_open_abi ON`。
P、D 两端使用同一提交和完整配套库。在新子进程运行探针；用 `/proc/self/maps` 确认实际加载的 HYBM、SMEM、
`_pymf_transfer` 来自候选目录，避免旧的 `LD_LIBRARY_PATH` / `LD_PRELOAD` 干扰。

## 上机验收顺序

沿用服务器已经修正过注册范围和退出流程的探针，保留同一套内存类型和后端参数。

| 顺序 | 场景 | 验收条件 |
| --- | --- | --- |
| 1 | pinned，D=0、P=3，256 KiB | URMA、CPU 内容、AIV 校验全部通过 |
| 2 | 官方 SHARED，相同配置 | 同上；设备表直接找到数据区和实际通知区，无 token 回退 |
| 3 | 双 D=0、2，P=3 | 各拉取一部分，每卡校验完整数据；串行、并行均通过 |
| 4 | 4 MiB、多轮、SIMT-CG 和 MTE | 内容正确，正常退出、重新初始化后仍通过 |

首次出现失败便保留首错、设备内存表、各 rank HVA/DVA/范围和实际加载库路径，暂停后续扩大规模。
CPU 单元测试只能证明调用顺序和资源处理；HCOMM 设备表是否完整、SHARED 传输是否恢复，必须以上机结果为准。
