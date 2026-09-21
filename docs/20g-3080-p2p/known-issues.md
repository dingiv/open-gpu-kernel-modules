# 已知问题与待修复清单(20G 3080 P2P 驱动)

> 2026-09-20 | 来源:混插落地 + llama.cpp T0 系列挣扎的全程探针取证
> 维护约定:每项带 状态/证据/影响/修复方向/工作量;修复后移入文末"已关闭"并注明 commit

## D-1 · VMM VA 落洞缺陷(主案,待根因)

**状态:未修复(应用侧可绕行)**

**症状**:驱动 API `cuMemAddressReserve` 返回的某些 VA 落点上,VMM 分配
`cuMemCreate/cuMemMap/cuMemSetAccess(self)` 全部成功,但此后该缓冲的
**一切访存操作返回 CUDA_ERROR_INVALID_VALUE(1)**,且
`cuPointerGetAttribute(CU_POINTER_ATTRIBUTE_MEMORY_TYPE)` **误报为 HOST(0)**
(设备指针被当成主机指针,方向判断随之全错)。

**证据**:
- 09-17(vmm 会战):大缓冲落在 VA > 0x7xxx_0000_0000 时同款症状
  (当时以"阈值 ≥1GB 让大块落低位 VA"绕开)
- 09-20(T0h):164MB meta 缓冲落在 VA **0x1332000000**(非高位!)同款全灭
  ——同进程、同时刻的其他 VMM 分配(KV 0x1f20000000、权重等)完全正常
  ⇒ **不是"高位"问题,是 VA 空间中存在特定坏洞**;洞的位置与大小未知

**影响**:所有 cuMem/VMM 应用(llama meta/comm 缓冲、任何 VMM 化尝试);
llama 一期(阈值降 64MB)直接被它堵死。

**修复方向**:
1. 取证:写独立 prober,在进程 VA 空间批量 reserve+自检,绘制坏洞地图
   (固定区域?随 ASLR 漂移?与 UVM managed 区/保留区是否重合?)
2. 根因假设:cuMem VA 分配器与 UVM managed VA 范围/其他保留区**缺乏冲突检测**
   ——reserve 落进已被占用的语义洞,分配成功但页表语义错乱
3. 修复:placement 策略避开坏区,或补冲突检测
4. 附带:属性误报(HOST)单独复测——它是应用侧自检的最快判据

**工作量**:根因定位 1-2 天(prober + 源码),修复视根因 0.5-2 天
**临时绕行**:应用侧 malloc 后 memset 自检,失败 → unmap + VA 偏移重试(~30 行)

## D-2 · 异常退出路径的窗口预算回收未验证(转正式测试项)

**状态:待验证(回归测试缺失)**

**症状**:T0 系列旧 boot 上曾观察到 `dynBar1MappedBytes` 疑似跨进程残留
(后经干净 boot 复测否定,当时为真实胃口)——但 **ggml_abort/SIGKILL 类
硬退出是否完整回收窗口与预算计数,从未被正向验证**。

**为什么重要**:任何一个 aborted 的 P2P 进程若泄漏窗口,会把预算墙"变相收紧"
毒化后续所有进程(且无进程持有可查,极难排查)。

**修复方向**:
1. 回归测试:进程 A 建满 190MB 窗口 → abort -9 → 进程 B 应能全额再建
   (`journal P3` 对账 mapped 归零)
2. 若泄漏:审计 nvGpuOpsFreeDupedHandle/_DestroyAll 与异常退出路径的
   计数回收(重点:UVM teardown 是否必然经过 FreeDupedHandle)

**工作量**:测试 0.5 天;若需修 0.5-1 天

## D-3 · 预算保留区默认值与实测驱动占用不匹配(调优项)

**状态:参数化已完成(`nv_dynbar1_reserve_mb`,钳位 [8,192]),默认值待校准**

64MB 保留是 duanyll 的保守常数;实测驱动自身 BAR1 占用 <10MB(窗口起点
0x600000~0xa00000)。收缩到 48 可为 NCCL-CUMEM 类传输腾出 16MB(预算 208MB),
T0g 账本显示这 16MB 恰是 llama NCCL-CUMEM 姿势所差的数量级(虽然 llama 最终
倒在更大的映射上)。

**修复方向**:基于 D-1 prober 的驱动占用实测,把默认值校准为"实测占用 + 合理余量"
(如 32MB),为所有小 BAR1 用户白让 ~32MB 预算;或维持 64 保持保守。

**工作量**:0.5 天(含回归)

## D-4 · patch3(best-effort 跳过)归档待命

**状态:降级归档(非缺陷,是备胎机制)**

对 NCCL transport 映射做预算拒跳过**不安全**(NCCL 以为成功、首包踩空,
init 失败恶化为运行期故障)。仅当出现"大分配永不跨读"的 legacy 应用需求时
复活,且需加大小阈值判别(>预算跳过 + 响亮日志)。完整设计与证伪记录见
`docs/llama.cpp/patch3-besteffort-cudaMalloc-施工文档.md` §六。

## 观察项(不计入修复,记录在案)

- 引擎级 nccl-tests 全尺寸矩阵验证(vLLM/llama 已覆盖主路径,nccl-tests
  补充性验证优先级低)
- duanyll/aikitoria 上游反馈:律 v3 + 混插扩展 + T0 账本(文档已就绪,
  待提交 issue)
- 595 判别实验:已失去必要性(修法与 GSP 行为兼容)
- 运行时 reserve=48 的持久化:当前为运行时值,重启回 64;若 T0h 后验
  48 更优,再改默认

## 外部事故定性记录(2026-09-20/21 Xid 群,非驱动缺陷)

> 同一 boot(9-20 05:22 起,模块 a8e5167d 在位)出现三起应用侧事故,
> 9-21 逐一定性。驱动内核侧全程零异常(P3 账本/无 REJECT/无 assert/无 UNMAP 异常)。

| 案 | 时间/卡 | 症状 | 定性 | 关键证据 |
|---|---|---|---|---|
| A | 9-20 18:20 · 3080@01:00 | parity_all_cases ×5 pid 连续 Xid 31 FAULT_PDE VIRT_READ @ **VA 0**,随后 Xid 13 ×128 + Xid 43(通道毒化收尾) | **应用侧**(marlin kernel/FFI 读 null;当晚正处 libmarlin.a 九对象 archive 合并 churn 窗,临时不一致构建是自然解释) | ① 单卡 cudaMalloc 测试,**零 P2P**,不打进任何补丁/探针路径;② 同一模块实例 9-21 复现 30/30 全绿、内核零异常;③ fault VA=0 = kernel 内 null 解引用 |
| B | 9-21 10:00 · 3080 对 | xinfer runner 双 rank **同刻同 host VA** segfault ×2 对(0x1ccd216410 / 0x26ad216410),死在 libcuda 用户态 SSE 拷贝循环 | **应用侧**(host 指针生命周期:异步 H2D 未完成就 unmap 源 mmap;s3-load 预取改造当日路径) | fault IP 在 libcuda 用户态,error 4 用户态读;驱动内核模块不在故障路径;双 rank 对称 = 确定性布局同一悬垂指针 |
| C | 9-21 14:32 · 3090 对(静态路)| xinfer runner 双 rank 同毫秒 Xid 31 @ **同 VA 0x1f_cb427000** | **应用侧(最可能)**:graph × mempool trim 竞争,与 mistral.rs 已定谳前科同构(Xid 31 FAULT_PDE VIRT_READ 同签名) | ① fault VA 在 UVM 常规区(peer encode 基址 0x208/0x218_00000000 远方)且 PDE 无效 = 读已拆映射的自有 VA;② 3090 对静态路无窗口可拆,全 log 无该对 DynBar1 活动;③ 时间线 = gdb attach(14:07)→ Xid(14:32)→ 47e68da 提交(14:51,自述 trim 进 runner + OOM 归因中) |

**结案口径**:三案均非驱动缺陷。案 C 若需 100% 封死,可按 mistral.rs 修法做 trim-off A/B
(引擎级,~半小时);驱动侧无待办。案 A/B 的 app 侧修复属 xinfer/marlin-ffi 卷宗自理。
另:9-21 起 `nv_p3_tags=0` 运行时静音(探针仍在模块内,`echo 0xffff` 可复开)。

## 已关闭

| 项 | 关闭方式 |
|---|---|
| GSP 窗口绑定死区(律 v3 之前) | align3 `0398d313`,2MB placement floor |
| setAccess205(cuMemSetAccess 恒拒) | 混插扩展 `e0609f7d` 顺带治愈(全局 walk 毒化消除),09-19 定谳 |
| 混插对 enFAIL | 同上,四对 13.2 GB/s 全绿 |
| 610 线全部遗留 | 615 迁移后整体废弃,归档于 p2p-build-archive |
