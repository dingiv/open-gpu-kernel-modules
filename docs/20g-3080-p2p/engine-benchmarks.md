# 20G 3080 对引擎实测 —— llama.cpp 与 vLLM(P2P 红利的进程模型分水岭)

> 2026-09-19/20 | 驱动:615.71.09(aikitoria+duanyll+本地,混插扩展 + 预算参数化)
> 硬件:3080 20G ×2(01:00 / C1:00)+ 3090 Ti ×2(41:00 / 42:00),混插四对全通 13.2 GB/s
> 模型:Nex-N2.5-Mini(34.7B-A3B MoE)——llama.cpp 用 Q4_K_M GGUF(f16 KV,ctx 262k,
> tensor-split);vLLM 用 INT4 Marlin(fp8 KV,ctx 262k,TP2 双进程)
> 口径:随机词池 ~4k prompt / 256 output / 3 轮取中位 / 非流式服务端自报
> 工具:packages/llm_speedtest(统一测速);NCCL DEBUG 日志判定传输

## 〇、一句话结论

**同一驱动、同一对卡:P2P 红利归多进程引擎(vLLM,NCCL 逐 buffer 授予,
prefill +28.5%),单进程引擎(llama.cpp)结构性无缘(全部 P2P 传输味撞预算墙),
其终态 = SHM(133.6 t/s decode,internal-AR 后端)。** 根因是进程模型,不是驱动缺陷。

## 一、vLLM(多进程 NCCL)—— P2P 红利兑现者

vLLM TP2 = 每卡一个 worker 进程(`VLLM::Worker_TP0/TP1`),NCCL 走 **IPC 逐 buffer
授予**:每个 rank 只把自己的通信缓冲(几 MB)暴露给对端,窗口预算装得下。

| 姿态 | prefill t/s | decode t/s | 备注 |
|---|---|---|---|
| NCCL SHM(基线)| 5936 | 153.0 | |
| **NCCL P2P/IPC(解禁)** | **7656(+28.5%)** | **157.2(+2.4%)** | 57 请求零 Xid |

- 生产已固化:`--flavor nex_int4`(serve.py 默认开 P2P),双 3080 长稳试运行
- 注意:vLLM 须 `--disable-custom-all-reduce`(自研 AR 走 runtime enable,会踩
  限制 #1;NCCL 后端无此问题)

## 二、llama.cpp(单进程双 rank)—— 全路径实测封死

llama.cpp tensor-split = **一个进程内两个 CUDA 设备做 rank**。NCCL 对同进程 rank
启用 direct/零拷贝类传输,并对**进程内全部显存**做急切 peer 映射(runtime peer
enable 是进程级语义)——llama 的 init 后大分配(权重段/KV/meta)逐笔撞预算墙。

### 2.1 双 3080(历史,09-17)

| ALLREDUCE 后端 | prefill | decode | 备注 |
|---|---|---|---|
| nccl(SHM 三件套)| 2626 | 131.3 | |
| nccl + VMM 权重(≥1GB)| 2603 | 131.6 | |
| none(meta 通用蝶形)| 1904 | 98.8 | |

### 2.2 混插对 3080@01 + 3090@42(09-19/20,T0 系列)

| # | 姿势 | 结果 |
|---|---|---|
| T1 | `P2P=0` + nccl + `CUMEM=0` + `P2P_DISABLE=1`(**SHM**)| ✅ **2538 / 131.7** |
| T0b | P2P pop + nccl 放行 P2P + `P2P_LEVEL=SYS` | ✗ `p2p.cc:348 failed to peer with device: out of memory`(direct 大窗)|
| T0c | + `NCCL_BUFFSIZE=1MB` | ✗ 同死(胃口与通道缓冲无关)|
| T0f | + `NCCL_P2P_DIRECT_DISABLE=1` | ⚠️ 传输切到 `P2P/CUMEM`(小块增量)仍超支:`166MB 已授 + 32MB 请求 + 保留 > 256MB` |
| T0h | + 驱动 reserve 64→48MB(`nv_dynbar1_reserve_mb`) | ✗ 死点后移至 **KV 2.5GB 映射拒**(NCCL 内部 peer enable 急切化后续分配)|
| T0i | + `NCCL_CUMEM_ENABLE=0`(IPC 传输)| 通道 `via P2P/IPC` 建立 ✓;死于 **meta 162MB 急切映射 OOM** |
| — | **internal(ggml 自研 AR,pinned host 中转)** | ✅ **2308 / 133.6**(解码全场最快,免 NCCL 依赖)|
| — | none(meta 通用蝶形,混插对)| (双 3080 历史:1904 / 98.8)|

探针账本(`P3[...] window map/REJECT`)逐笔记录了每次授予/拒绝的体量与
累计位置,完整序列见 patch3 施工文档 §二。

### 2.3 机理闭环(为什么单进程无解)

```
任一 NCCL P2P 传输建立
  └► NCCL 内部调用 runtime peer enable(进程级语义)
       └► 此后进程内每笔 cudaMalloc 都被 UVM 急切 peer 映射(承诺全量可见)
            └► llama 分配画像:warmup 临时 64+102MB → meta 164/82MB → KV 2.5GB
                 └► 累计穿透 190~208MB 预算(驱动 reserve 48~64MB 均试)
```

- 映射是**小块增量**的(2MB/32MB/164MB/2.5GB 逐笔),不缺"小块模式"——
  缺的是**总量额度**,而总量由 llama 的分配结构决定,不随
  `BUFFSIZE/LOCAL_REGISTER/GRAPH_REGISTER/SYMMETRIC_SIZE/RESERVE` 任何旋钮变化
- 逐 buffer 授予的 vLLM 模式 = 多进程 IPC 显式共享;单进程 = 进程级承诺,不可选
- 附带发现(VMM VA 落洞):64MB 阈值下部分 cuMemAddressReserve 落点全访存
  INVALID_VALUE、属性误报 HOST;应用侧自检+VA 重试可绕(已列入限制 #7)

## 三、结论矩阵

| 引擎 | 进程模型 | P2P 可达 | 最优姿势 | 实测 |
|---|---|---|---|---|
| vLLM | 多进程(一卡一进程)| ✅ NCCL P2P/IPC | NCCL 后端 + 禁 custom AR | 7656 / 157.2 |
| llama.cpp | 单进程(双 rank)| ❌ 结构性封死 | **internal-AR 或 SHM** | 2308-2538 / 131.7-133.6 |

驱动侧能力(六对 13.2 GB/s)对两类引擎一视同仁;差异全部来自进程模型对
"可见性承诺粒度"的影响。**后续若出现单进程 + 小 BAR1 的强需求,解法排序:
① 应用改用显式 VMM 授予(自带小暂存);② 驱动预算拒跳过(限制 #1 patch3,
仅适用"大分配永不跨读"类);③ 多进程化。**
