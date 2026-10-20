# HCOMM 9.1.0 本地 MR 记录解析修复与隔离验证

## 范围与原因

官方 `v9.1.0` 基线为 `4ca1db912ce02a5f5075849a3fa3faaf731bdd60`。
生产代码只修改 `UbTransportLiteImpl::ParseLocBufferMap()`。
MF 保持 `6f914ebdc21b3371e7cb47d46c650e871391728a`，保留服务器已经通过的独立 HYBM 库名与加载方式。
这里没有修改 MF，也没有把服务器尚未提供的库名补丁重新实现一遍。

`UbMemTransport::GetSingleRmtBufferUniqueId()` 写入：

| 字段 | 字节数 |
| --- | ---: |
| addr | 8 |
| size | 8 |
| tokenId | 4 |
| tokenValue | 4 |
| notifyId | 4 |

每条共 28 字节；原本地解析只消耗前 24 字节，第二条起读偏。
本次兼容每条 24 和 28 字节，28 字节格式读取并忽略本地表不使用的 notifyId。
解析前检查总长可整除记录数，且记录宽度只允许 24/28。
记录数为零时保留原有行为。没有修改远端表、传输凭据、回退规则或通道初始化顺序。

Pinned 能搬运成功，不代表表正确：设备可能仍借用第一条记录的凭据。
本修复针对记录错位，不预先宣称能消除所有 SHARED 传输问题。

## CPU 回归

```bash
python3 scripts/local_mr_dto/test_parser.py
python3 scripts/local_mr_dto/test_parser.py --sanitize
```

测试现场提取真实 parser、writer、LocUbBufLite，并使用原始 BinaryStream 头文件。
只替换日志、异常等外围依赖，不走新版 UT 中的空解析桩。
覆盖 1/2/3 条、24/28 字节格式，地址、长度、两项凭据，以及 10 种异常长度和零记录。
共 17 个用例。用相同测试加载原版解析函数时，会在第二条 28 字节记录的地址检查中失败；
修复后通过。ASAN/UBSAN 也通过。这不是完整 HCOMM 编译、UT 或设备测试。

## 在匹配服务器上构建

使用服务器匹配的 **CANN/HCOMM 9.1.0** 工具链。不要用本地 9.2 beta 编译后声称兼容 9.1。
还需满足官方 `docs/zh/build/build.md` 的依赖要求（含 GCC 7.3～13.3.x）。
在独立源码目录应用提交或 `hcomm_910_local_mr_dto.patch` 后执行：

```bash
# 进入这份 HCOMM 9.1.0 源码根目录。
# 先按服务器实际安装位置设置 CANN_910，例如：
CANN_910=/usr/local/Ascend/cann
source "$CANN_910/set_env.sh"
cat "$CANN_910/share/info/hcomm/version.info"

python3 scripts/local_mr_dto/test_parser.py
bash build.sh --pkg --full --version 9.1.0 -j8 -p "$CANN_910"
```

不能只运行 `--pkg`：它默认只编译 Host，修复位于 AICPU 的 `ccl_kernel` target。
需要离线依赖时，附加 `--cann_3rd_lib_path=/实际依赖目录`。
构建脚本只生成产物，**不执行生成的 `.run` 安装器，不覆盖系统 CANN**。

主要产物：

- `build/device_build/aicpu_hcomm.tar.gz`：含 libccl_kernel.so、libccl_kernel_plf.so 和 bin_hash.cfg。
- 同一 device build 目录中生成的 `ccl_kernel.json`。
- `build_out/*.run`：本轮不安装。

查找生成配置的位置：

```bash
rg --files build/device_build | rg '(^|/)ccl_kernel.json$'
```

## 建立独立加载目录

把上一步唯一的配置文件路径赋给 KERNEL_JSON；目录应来自本次构建。

```bash
KERNEL_JSON=/本次构建输出的完整路径/ccl_kernel.json
HCOMM_VIEW=/tmp/hcomm_910_local_mr_view
python3 scripts/local_mr_dto/stage_overlay.py \
  --cann "$CANN_910" \
  --archive build/device_build/aicpu_hcomm.tar.gz \
  --kernel-json "$KERNEL_JSON" \
  --overlay "$HCOMM_VIEW"
```

脚本读取工具链版本、检查包内 SHA256、AArch64 ELF 和修复字符串；创建全新目录，不能复用已有目录。
它只在候选路径创建真实文件，其余路径链接到原安装；不会沿符号链接写入系统文件。
候选文件包括 JSON、完整 AICPU 包和 devlib/device 下的两个库，保留包内 hash 清单。
脚本不把 Host HCOMM 库替换成新编译版本，尽量只检验这个设备解析修复。

加载顺序：先启用正常 CANN/虚拟环境及原 MF overlay，**最后**加载本脚本生成的环境：

```bash
source "$HCOMM_VIEW/local_mr_dto_env.sh"
cat "$HCOMM_VIEW/local_mr_dto_manifest.json"
# 在同一环境中，用全新 P/D 进程运行现有 probe。
```

`hccl::GetKernelFilePath()` 和 `Hccl::GetKernelFilePath()` 均读取 ASCEND_HOME_PATH，
查找 `opp/built-in/op_impl/aicpu/config/ccl_kernel.json`。
同时设置 ASCEND_AICPU_PATH、ASCEND_OPP_PATH 指向私有视图。
环境变量用途可参见[官方环境变量参考](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/maintenref/envvar/envref_07_0001.html)。
不能用 Host `LD_PRELOAD=libccl_kernel.so` 代替设备包加载。
服务器需用加载日志/打开文件记录确认 JSON 和设备包路径，再以解析出的正确记录、零回退验证设备侧实际生效。
仅检查 Host `/proc/PID/maps` 或文件 SHA256 不足以证明设备加载了候选。

此隔离方式尚待服务器验证；若 Runtime 仍取系统包，应先追踪实际路径，不要误报修复无效。
自编译包可能没有厂商签名；按官方构建文档和现场已有的获准测试流程处理，脚本不会修改设备验签配置。
退出测试进程并重新启用原环境即可回退；不需要全局卸载或安装。

## 固定验收顺序

1. 新进程，pinned，D=0/P=3，256 KiB：URMA 返回成功、CPU 和 AIV 数据正确，数据区和通知区命中正确记录，`fallback_count=0`。
2. 官方 Scene.SHARED，相同配置和标准。Pinned 仍有回退时，先不要扩大测试。
3. 双 D=0/2，P=3：串行与并行分片，各卡验证完整数据。
4. 4 MiB、多轮更新、SIMT/SIMT-CG/MTE，检查退出回收。

fallback_count 来自现有 probe 的日志统计，本补丁没有新增同名 HCOMM 指标。
需要保留设备日志，确认数据区和实际通知区的地址、长度、tokenId/tokenValue 全部正确。
之后才继续生产共享池接入；当前没有 vLLM 整网或性能结论。
