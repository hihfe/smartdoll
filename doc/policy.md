# 策略注册表（policy.h）

> 策略的唯一收容处——**纯头文件：没有实现、没有进程、不占模块号**，各模块共同可见（同 [media.md](./media.md) / [with.md](./with.md) 形态）。**策略 = 数据（行），不写分支；审查 = 编译期（`static_assert` 套件）**：每个事件、每类单据、每类错误源的策略都是注册表里的一行，行随契约声明一起出生——不声明策略的事件根本定义不出来，策略漂移在编译期就断。

## 为什么收编

- 策略散写在各模块规则的散文里：03 的背压列了 4 个分支、06 列了 9 个、05 的看门狗带"事件型不挂"这种例外、错误两层的归属在 02/03/05/06 各说一遍——散文不能执行：新增事件忘声明、改 A 漏 B、规则与代码各长各的。
- 策略分支还散写进编排代码：`if (type 是语音) 丢旧留新 else 阻塞`、`if (设备是事件型) 不挂看门狗`——每处分支都是一次复写（违反 [with.md](./with.md) 的切面准入），也是漂移源。
- 表驱动后：策略**只增行不改分支**；改行 = 改契约，过号段主人；完整性由编译器盯，不靠记性。

## 机制一：常量即行

事件常量的声明本身就是策略行——想跳过策略声明，事件就无从定义：

```cpp
// ===== policy.h：策略行形 =====
// 字段类型（EventType / ModuleId / Qos）定义见「行形数据定义」。

struct EventDef {
    EventType  id;
    ModuleId   owner;      // 归属模块 = 号段主人
    Qos        qos;        // 队列满：kBlock 关键阻塞 / kDropCount 丢弃计数 / kKeepLatest 留最新
    constexpr operator EventType() const { return id; }   // 用起来仍是 EventType
};

// 各模块契约块里按此形声明（以 02 为例）：
inline constexpr EventDef kTypeIpcRequest  {100, kIpc, Qos::kBlock};  // 收到请求（需响应）
inline constexpr EventDef kTypeIpcDataReady{103, kIpc, Qos::kBlock};  // 数据面：环空→非空，负载直达
```

原 `constexpr EventType kTypeX = n;` 形态作废：id 与策略分家即回到散文时代。数据面**块**的背压不在这里——块自带 `qos` 字段（`kQosCritical`/`kQosRealtime`，生产者声明制，见 [media.md](./media.md)），与事件行同一精神：**声明即注册，传输层按声明执行，不按类型猜**。

## 机制二：号段注册表

| 模块 | 号段 | owner | 细分（owner 内政） |
|---|---|---|---|
| 01 事件分发核心 | 0–99 | `kDisp` | — |
| 02 IPC | 100–199 | `kIpc` | — |
| 03 网络 | 200–299 | `kNet` | — |
| 04 定时器 | 300–399 | `kTmr` | — |
| 05 设备 IO | 400–499 | `kDev` | 400–409 公共 / 410–449 流式 / 450–499 事务 |
| 06 AI | 500–599 | `kAi` | 500–509 公共 / 510–549 前端·ASR / 550–579 LLM / 580–599 TTS |
| 07 仿生 | 600–699 | `kBio` | 600–609 公共 / 610–639 触摸 / 640–669 呼吸 / 670–689 体温 / 690–699 动作 |
| M2 业务层 | 1000 起 | `kBiz` | 1000–1049 对话分支 / 1050–1099 仿生行为 |

## 机制三：编译期审查套件

policy.h 顶 `static_assert`（号段互不重叠），其余审查是 constexpr 审计函数（`rows_unique` / `rows_in_segments` / `op_rows_complete`），由契约块与测试套件（test_contracts）`static_assert` 调用——漏一个都编译不过：

| 审什么 | 怎么审 |
|---|---|
| 号段互不重叠 | 注册表各段两两求交为空 |
| 行不越段 | 每行 `id ∈ owner` 号段 |
| id 全局唯一 | 全表按 id 排序后相邻不等 |
| 策略值合法 | `Qos`/处置枚举是 `enum class`，非法值根本写不出 |
| 分流覆盖 | 各模块 inbox 的 `with(ev, …)` 表覆盖本模块全部内部事件（待落地：尚无该审查） |
| 单据有超时行 | 每类在飞单恰有一行 `OpPolicy`（锚点 `kOpKindCount = 8`，增枚举必同步），超时参数不出行外 |
| 块声明带 qos | 每个 `kKind*` 的生产方声明 `kQos*`（与 media.h 声明制对齐；待落地） |

## 策略行形一览

| 行形 | 一行 = | 谁查 | 编译期审什么 |
|---|---|---|---|
| `EventDef` | 一个事件号的满时策略 + 归属 | 01 `should_drop`、投递点 | 号段 / 唯一 / 分流覆盖 |
| `OpPolicy` | 一类在飞单的超时与迟到处置 | `ticket`/`settle` 切面 | 每类单恰一行 |
| `ErrPolicy` | 一类错误源的归属层与处置 | `intercept` 切面 | 处置枚举封闭 |
| `DevicePolicy` | 一类设备的机制与 IO 形 | 05 `attach` | 重载按类别，走错机制编译不过 |
| `Backoff` | 一档重连退避间隔 | 02/03 重连状态机 | — |

## 行形数据定义

五种行形的数据定义（`EventDef` 的 struct 见「机制一：常量即行」，此处不重复）。**键随契约长、处置枚举封闭**：键（`OpKind`/`ErrSource`）是开放集——新增类别 = 扩枚举 + 出行；策略值枚举（`Qos`/`OpTimeoutAct`/`OpLateAct`/`ErrAction`）是封闭集——非法值根本写不出（即机制三"策略值合法"一栏）。行的取值文档形：`OpPolicy`/`ErrPolicy`/`Backoff` 见下三节，`DevicePolicy` 见 [modules/05-device-io.md](modules/05-device-io.md)「设备分类判据」表。

```cpp
// ===== policy.h：行形数据定义 =====

// —— 支撑类型（EventDef 的字段类型，定义在此）——
using EventType = uint32_t;    // 弱类型：enum class 封死扩展，不用；审查归 static_assert 套件
enum ModuleId : uint32_t {     // 号段主人；值 = 号段基值（模块号 × 100），行不越段 = id ∈ [基值, 基值+100)（kBiz：≥ 1000）
    kDisp = 0, kIpc = 100, kNet = 200, kTmr = 300, kDev = 400, kAi = 500, kBio = 600, kBiz = 1000,
};
enum class Qos { kBlock, kDropCount, kKeepLatest };    // 队列满处置：阻塞 / 丢弃计数 / 留最新

// —— OpPolicy：在飞单行（ticket/settle 切面查：挂多久、超时怎么办、迟到怎么办）——
enum class OpKind {                 // 单据类别（键）
    kIpcRequest,     // 02 IPC 请求
    kTxIo,           // 05 事务（ioctl / 读数 / 动作）
    kEventIo,        // 05 事件型常驻读单
    kNetConnect,     // 03 出站 connect 代跑
    kLlmRequest,     // 06 LLM 请求
    kAsrTts,         // 06 ASR 流 / TTS 单
    kTimerfdRead,    // 04 timerfd 读单
    kUringIo,        // 01 IO 申请单（02/03 常驻读单、eventfd 唤醒单、读写单）——P1 补行
    kSvcTrigger,     // 07 服务 trigger 在飞单（待 07 落码；届时 kOpKindCount 8→9）
};
enum class TimeoutSrc { kNone, kCaller, kSpec };   // 超时参数来源：无超时 / 调用方给（timeout_ns 形参）/ 行内默认
enum class OpTimeoutAct {          // 超时处置
    kNone,           // 无超时即无处置
    kSynthReply,     // 合成 kTimeout 响应（与响应同形）投回执管道
    kKillDevice,     // 置死设备 + kTypeDeviceDead + 拒新单
    kBackoffNext,    // 下一档退避重试
    kCloseCase,      // kTypeLlmDone(kTimeout) 结案
    kSelfHeal,       // kTypeError 自愈重建 + 重挂（04：机制本身是定时，异常即自愈）
};
enum class OpLateAct { kNone, kDropCount };        // 迟到处置：— / 查表落空丢弃并计数

struct OpPolicy {
    OpKind       kind;         // 单据类别（键）
    TimeoutSrc   timeout_src;  // 超时参数来源
    uint64_t     timeout_ns;   // 默认超时；kNone/kCaller = 0，kSpec = 行内默认（05 事务 500ms，DeviceSpec.watchdog_ns 可按设备覆写）
    OpTimeoutAct on_timeout;   // 超时处置
    OpLateAct    on_late;      // 迟到处置
};

// 各模块契约块里按此形出行（以 05 事务单为例）：
inline constexpr OpPolicy kOpTxIo{OpKind::kTxIo, TimeoutSrc::kSpec, 500'000'000,
                                  OpTimeoutAct::kKillDevice, OpLateAct::kDropCount};

// —— ErrPolicy：错误源行（intercept 切面查：归哪层、怎么处置）——
enum class ErrSource {           // 错误源（键）：同源不同处置按模块分值（kIpcOversize 回填 / kNetOversize 断连）
    kKernelErrno, kBadFrame, kBadJson,
    kIpcOversize, kNetOversize, kIpcReject, kIpcVersion, kIpcBroken, kNetBroken,
    kAlsaXrun, kAlsaFatal, kPeriphOpFail, kPeriphDead,
    kAiTaskFail, kAiEngineDead,
};
enum class ErrLayer { kTransport, kProtocol, kState, kDevice, kTask };   // 归属层
enum class ErrAction {           // 处置
    kTranslate,      // 翻 kTypeError 不抛上层
    kDisconnect,     // 断连
    kDropCount,      // 丢该帧 + 计数
    kReply,          // 回填错误码（kTooLarge / kRejected / kUnsupported…，码随源定）
    kBrokenReport,   // 未决单 kBroken 回填 + 断连事件上报
    kRecover,        // 就地 recover + 计数
    kKill,           // 置死 + kTypeDeviceDead + 拒新单
    kFillStatus,     // 翻进结果结构（ActuatorResult.status / AiErr）
    kKillComponent,  // 分件置死 kTypeAiDead + 拒新单
};

struct ErrPolicy {
    ErrSource source;   // 错误源（键）
    ErrLayer  layer;    // 归属层
    ErrAction action;   // 处置
};

// —— DevicePolicy：设备行（05 attach 查：走什么机制、什么 IO 形）——
enum class DevMech { kStreamThread, kDeferRun, kResidentRead, kWatch };
    // 专用线程 poll 就绪直读写 / 01 代跑 + 完成回流 / 常驻读单直入收货口 / watch 轮询（事件型按总线出两行）
enum class DevIo { kStreamSpec, kDeviceSpec };   // IO 形 = attach 重载

struct DevicePolicy {
    uint8_t kind;   // 设备类别（dev.h：kStream / kTransaction / kEvent）
    DevMech mech;   // 机制
    DevIo   io;     // IO 形：stio::attach(StreamSpec) / txio::attach(DeviceSpec)——流式想代跑、事务想专线程，编译不过
};

// —— Backoff：重连退避行（02/03 重连状态机查）——
struct Backoff {
    uint32_t step;          // 档位（0 起）
    uint64_t interval_ns;   // 该档等待间隔；末行 = 封顶行，此后恒取本行、不再翻倍
};
```

## 超时与看门狗（OpPolicy 行）

| 单据类别 | 默认超时 | 超时处置 | 迟到处置 | 出处 |
|---|---|---|---|---|
| 02 IPC 请求 | 调用方给（`timeout_ns`） | 合成 `kTimeout` 响应（与响应同形）投回执管道 | 查表落空，丢弃并计数 | 02 |
| 05 事务（ioctl / 读数 / 动作） | `DeviceSpec.watchdog_ns`（默认 500ms） | 置死设备 + `kTypeDeviceDead` + 拒新单 | 丢弃并计数 | 05 |
| 05 事件型常驻读单 | 无超时 | —（等待是常态，不返回不是事故） | 丢弃并计数 | 05 |
| 03 出站 connect 代跑 | 调用方给 | 下一档退避重试 | 丢弃并计数 | 03 |
| 06 LLM 请求 | 调用方给（首增量上限） | `kTypeLlmDone(kTimeout)` 结案 | 丢弃并计数 | 06 |
| 06 ASR 流 / TTS 单 | 无超时 | —（等待是常态） | 丢弃并计数 | 06 |
| 07 服务 trigger 单（待 07 落地） | 无超时 | —（05 看门狗波及结案：`kTypeDeviceDead` → `kDead`） | 丢弃并计数 | 07 |
| 04 timerfd 读单 | 无超时（机制本身是定时） | `kTypeError` 自愈重建 + 重挂 | — | 04 |
| 01 IO 申请单（读写/唤醒/常驻读） | 无超时 | —（完成即结账） | 丢弃并计数 | 01 |

## 错误归属（ErrPolicy 行）

| 错误源 | 层 | 处置 | 出处 |
|---|---|---|---|
| 内核 errno（IO 完成负 res） | 传输层 | 01 `translate` 翻 `kTypeError`，不抛上层 | 01 |
| 帧头非法（定界不可信） | 协议层 | 断连（与 02 `kBadMagic` 同理） | 02/03 |
| 单帧 JSON 解析失败（帧界仍可信） | 协议层 | 丢该帧 + 计数 | 03 |
| 负载超 `kMaxPayload` | 协议层 | 回填 `kTooLarge` / 断连 | 02/03 |
| 对端业务拒绝 / 版本不识别 | 协议层 | 回填 `kRejected` / `kUnsupported` | 02 |
| 断连（传输层归纳） | 状态 | 未决单 `kBroken` 回填 + `kTypeIpcBroken` / `kTypeNetDown` 上报 | 02/03 |
| ALSA xrun | 设备层 | 就地 `recover` + 计数 | 05 |
| ALSA 不可恢复 | 设备层 | 置死 + `kTypeDeviceDead` | 05 |
| libperiphery 负码（事务失败） | 设备层 | 翻 `ActuatorResult.status` | 05 |
| libperiphery 负码（设备死亡） | 设备层 | 置死 + `kTypeDeviceDead` | 05 |
| 推理失败 / 报文解析失败 | 任务层 | `AiErr` 填 `LlmDone.status` / `AiErrorInfo.err` | 06 |
| 引擎不可用 | 任务层 | 分件置死 `kTypeAiDead` + 拒新单 | 06 |

## 重连退避（Backoff 行，02/03 共用）

| 档 | 间隔 | 说明 |
|---|---|---|
| 0 | 1s | 指数退避 `1s × 2ⁿ`（示例值：改期只改行） |
| 1 | 2s | |
| 2 | 4s | |
| 3 | 8s | |
| 封顶 | 30s | 不再翻倍；退避等待挂 04 `tmr::after`，到期回 `inbox` 重试 |

## 准入规则

- **策略只增行不增分支**：编排代码里长出按 type/类别选处置的 `if`（丢旧留新还是阻塞？挂不挂看门狗？断连还是丢帧？）即违规——搬进注册表成行，代码只查表。
- **行随契约出生**：新增事件/单据/错误源 = 契约块里出行（`EventDef` 常量即行），不出行就定义不出来；改行 = 改契约，过号段主人评审。
- **散文只写理由**：模块规则里保留"为什么"（如"文本流丢块即报文坏"），"是什么"（阻塞/丢弃计数/留最新）只在行里出现一次。
- **切面查表不判策略**：[with.md](./with.md) 的切面只做机械（进/出/计数），策略值一律以**行指针挂进切面的 `Aspect.state`**（`backpressure` 挂 `EventDef.qos`、`ticket`/`settle` 挂 `OpPolicy`、`intercept` 挂 `ErrPolicy`），行随签名显式传入、切面内不隐式查表——机制与数据分家，两边都可单测。
