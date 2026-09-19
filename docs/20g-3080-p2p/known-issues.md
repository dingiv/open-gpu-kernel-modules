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

## 已关闭

| 项 | 关闭方式 |
|---|---|
| GSP 窗口绑定死区(律 v3 之前) | align3 `0398d313`,2MB placement floor |
| setAccess205(cuMemSetAccess 恒拒) | 混插扩展 `e0609f7d` 顺带治愈(全局 walk 毒化消除),09-19 定谳 |
| 混插对 enFAIL | 同上,四对 13.2 GB/s 全绿 |
| 610 线全部遗留 | 615 迁移后整体废弃,归档于 p2p-build-archive |
