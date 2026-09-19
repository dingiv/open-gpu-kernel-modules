# RTX 3080 20G 双卡 P2P 驱动 —— 使用指南

> 适用硬件:RTX 3080 20G 魔改卡(BAR1 256MiB、无 ReBAR)双卡,以及 3090 Ti(ReBAR 32G)混插
> 驱动:615.71.09 · 分支 `615.71.09-p2p-3080-20g`(上游血统:aikitoria 消费卡 P2P → duanyll
> Method 3 动态窗 → 本地律 v3 修复 + 混插扩展 + P3 探针)
> 关联:duanyll 博客 duanyll.com/2026/7/13/4090-48G-P2P/ 及其 `docs/48g-4090-p2p/` 设计文档;
> 引擎实测见同目录 [engine-benchmarks.md](./engine-benchmarks.md)

## 一、设计原理

### 1.1 问题:static BAR1 的死教条

社区给消费卡解锁 P2P 的经典做法是把 GH100 的 **static BAR1 identity 映射**移植过来:
peer PTE = `对端 BAR1 基址 + 显存物理偏移`,硬性要求 **BAR1 窗口 ≥ 全部显存**。

- 3090 Ti:ReBAR 32G ≥ 24G FB → 成立(这也是 3090 对恒绿的原因)
- 3080 20G 魔改:BAR1 仅 **256MiB** ≪ 20G FB → static 死局

### 1.2 duanyll 的 Method 3:动态 BAR1 逐分配窗口

核心思想:**只要求"同时被 peer 映射的分配总量 ≤ BAR1"**。每当 peer 映射某块分配时,
用 GPUDirect RDMA 同款机制 `kbusMapFbAperture` 把这块分配单独开窗进对端 BAR1,
GMMU 提供一层翻译使任意 FB 页可达。窗口懒创建、随分配销毁、预算守卫干净拒绝。
用户态零改动;不碰邮箱路径(规避已知硬重置雷区)。

### 1.3 本地演进①:律 v3(align3)

duanyll 的 4090 能跑是 range0 恰好 2M 对齐的运气。本机实测暴露 GSP 隐藏语义:
**aperture 按 2MB 粒度绑定,窗口头部子 2MB 残留无 PTE 表项 = 死区(写静默丢失)**。
终解(align3):≥2MB BAR1 映射的 **VA placement floor 提到 2M**,PTE 尺寸不动 64K
——placement 与 PTE 尺寸解耦。实测窗口 residue=0,64MB 双向 13.2 GB/s 零 mismatch。

### 1.4 本地演进②:混插扩展(static×dynamic 逐方向归约)

duanyll v1 只做同构节点。本机 3090(静态)×3080(动态)全为混插,解法:

- **编码按 owner 路由**:owner 静态 → 恒等编码(静态区直打);owner 动态 → 逐分配窗口
- per-direction IOMMU 守卫:该方向对端是静态卡才建映射;三组合自动归约
- `RMDynBar1P2PEnable=0` 软开关:干净拒绝含 non-static 端的对(不恢复邮箱兜底)
- 附带战果:setAccess205 案结案——`cuMemSetAccess(peer)` 的 205 是混插对
  (UVM 全局 walk)邮箱毒化的附带伤害,混插改判 BAR1 后随之消失

### 1.5 本地演进③:P3 探针框架

`METHOD3_PROBES=1` 编译期开关(默认零开销):tag 分组 MAP/PTE/PAGE/APERT/PEERQ/HOT,
运行时 `/sys/module/nvidia/parameters/nv_p3_{tags,first,every}`。
窗口账本(`journalctl -k | grep 'P3\['`)是排查预算/映射问题的第一工具。

## 二、工作原理(运行时数据流)

```
NCCL/cuMem 引擎   ──► cuMemSetAccess(peer)/UVM_MAP_EXTERNAL
                      └► GET_P2P_CAPS_V2 阶梯(混插已放行;主机白名单
                         W 恒 OK、R 需同 switch 或 RYZEN/XEON_SPR)
                      └► PCIE_BAR1 → NV503b 对象
GPU0 访问 GPU1 的显存:
  owner 静态(ReBAR 卡)→ 恒等编码:对端静态区基址 + fbOffset(无预算概念)
  owner 动态(小 BAR1) → 逐分配窗口:kbusMapFbAperture 开窗 +
                         窗口 IOMMU 映射 + PTE = 窗口基址 + 偏移
torch 裸 runtime   ──► cudaDeviceEnablePeerAccess + cudaMalloc
                      └► enable 后单笔分配急切预建窗口 ⚠️ 见限制 #1
```

窗口生命周期:首次 peer 映射懒创建 → 挂 duped 句柄 → 句柄释放/subdevice 拆除销毁。
对账日志:`P3[...]: window map: range0=0x600000 residue=0 delta=0 calib=0`。

## 三、模块集与安装

| 模块集 | nvidia.ko md5 | 内容 |
|---|---|---|
| modules-615-mixed-probe | `9b3372a0…` | 混插 + P3/PID/PEERQ 探针(诊断推荐)|
| modules-615-reserve-param | `a8e5167d…` | 上 + `nv_dynbar1_reserve_mb` 预算参数化 |
| modules-615-align3p | `b45a3f20…` | 早期生产锚(纯双 3080 时代)|

安装(以仓库外工作区脚本为例,亦可手动):

```bash
sudo bash install-reserve-param.sh          # 守卫 + SHA256/md5 自检 + 备份 + depmod
sudo reboot
md5sum $(modinfo -n nvidia)                 # 复核指纹
```

手动等价:五个 .ko(nvidia/nvidia-uvm/nvidia-modeset/nvidia-drm/nvidia-peermem)
→ `/lib/modules/$(uname -r)/kernel/drivers/video/` + `depmod -a` + reboot。
**换模块必须 md5 复核 + 冷启动,勿热替换;切换探针模式必须 `make clean`。**

## 四、现役限制(使用前必读)

### 限制 #1:动态端 192MB 单笔预算墙

BAR1 256MiB − 64MB 保留 ≈ **190MB 并发可窗口化预算**。peer access 生效期间,
动态端任何**单笔 > 预算余量**的分配都会被确定性拒绝(响亮失败,不半配置)。
已知踩雷:llama.cpp 单进程双卡(权重 857MB 段、KV 2.5GB 等 init 后大分配)
——详见 [engine-benchmarks.md](./engine-benchmarks.md) §二,llama 终态 = SHM。

**不受影响**:NCCL/cuMem 多进程引擎(vLLM/sglang,缓冲 MB 级);不开 peer
access 的任何用法(超限分配被拒后优雅回落)。

### 其他边界

| # | 限制 | 说明 |
|---|---|---|
| 2 | `cudaDeviceEnablePeerAccess` = 进程级承诺 | enable 后该进程**所有**显存对 peer 可见(急切映射);NCCL 任何 P2P 传输都会内部触发它 |
| 3 | NCCL 单进程 direct/CUMEM/IPC 三种 P2P 传输实测均超支 | 单进程大缓冲结构 + 预算墙,详见 benchmarks §二 |
| 4 | 混插对仅 PCIe BAR1 路 | 邮箱路保持不可达(设计)|
| 5 | 不支持 `cudaMallocManaged` 跨卡迁移、`uvm_peer_copy=virt` | duanyll v1 承继 |
| 6 | NCCL 大 buffer 注册可能超预算 | `NCCL_LOCAL_REGISTER=0 NCCL_GRAPH_REGISTER=0` |
| 7 | VMM VA 落洞缺陷(应用侧观察) | 部分 cuMemAddressReserve 落点全访存 INVALID、属性误报 HOST;应用侧自检+VA 重试可绕 |

### 预算调参

```
/sys/module/nvidia/parameters/nv_dynbar1_reserve_mb   # 默认 64,钳位 [8,192]
```
收缩(如 48)可为 NCCL-CUMEM 类传输腾出额度(预算 190→208MB);
保留区防的是驱动自身 BAR1 占用(实测 <10MB),48 仍有充分余量。
重启恢复默认;对静态对无影响。

## 五、性能口径(速览)

六对全通 **13.2 GB/s 双向零 mismatch**(3090 对 / 3080 对 / 混插四对);
零 Xid 零 assert。引擎级结论:vLLM(多进程 NCCL)= P2P 红利兑现者
(prefill +28.5%);llama.cpp 单进程 = SHM 终态(133.6 t/s decode,免 NCCL 依赖)。
**完整数据:[engine-benchmarks.md](./engine-benchmarks.md)**

## 六、排查速查

```bash
journalctl -k -b | grep 'P3\['                     # 窗口账本(探针版)
cat /sys/module/nvidia/parameters/nv_p3_{tags,first,every}
md5sum $(modinfo -n nvidia)                        # 指纹
CUDA_VISIBLE_DEVICES=<对> p2pcheck 64              # 数据校验+带宽
```

失败模式速记:`failed to peer with device ... out of memory` = NCCL 传输映射超预算
(限制 #1/#2);`caps True` ≠ 通路,必须传输+校验;出异常 reboot 再查(GSP 状态可毒化)。
