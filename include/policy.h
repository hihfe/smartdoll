#pragma once

// ===== policy.h：策略注册表（纯头文件：没有实现、没有进程、不占模块号） =====
// 策略 = 数据（行），不写分支；审查 = 编译期（static_assert 套件）。
// 行随契约声明一起出生：EventDef 常量即行——不声明策略的事件根本定义不出来。
// 定义权在本文档（doc/policy.md），解释权在各号段主人；切面查表不判策略（见 with.h）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// —— 支撑类型（行形的字段类型，定义在此）——

using EventType = uint32_t;    // 弱类型：enum class 封死扩展，不用；审查归 static_assert 套件

enum ModuleId : uint32_t {     // 号段主人；值 = 号段基值（模块号 × 100），行不越段 = id ∈ [基值, 基值+100)（kBiz：≥ 1000）
    kDisp = 0, kIpc = 100, kNet = 200, kTmr = 300, kDev = 400, kAi = 500, kBio = 600, kBiz = 1000,
};

enum class Qos { kBlock, kDropCount, kKeepLatest };    // 队列满处置：阻塞 / 丢弃计数 / 留最新

// —— EventDef：事件定义 = 策略行（机制一：常量即行）——

struct EventDef {
    EventType  id;
    ModuleId   owner;      // 归属模块 = 号段主人
    Qos        qos;        // 队列满时策略
    constexpr operator EventType() const { return id; }   // 用起来仍是 EventType
};

// —— OpPolicy：在飞单行（ticket/settle 切面查：挂多久、超时怎么办、迟到怎么办）——

enum class OpKind {                 // 单据类别（键，开放集：新增类别 = 扩枚举 + 出行）
    kIpcRequest,     // 02 IPC 请求
    kTxIo,           // 05 事务（ioctl / 读数 / 动作）
    kEventIo,        // 05 事件型常驻读单
    kNetConnect,     // 03 出站 connect 代跑
    kLlmRequest,     // 06 LLM 请求
    kAsrTts,         // 06 ASR 流 / TTS 单
    kTimerfdRead,    // 04 timerfd 读单
    kUringIo,        // 01 IO 申请单（02/03 常驻读单、eventfd 唤醒单、读写单）——P1 补行
};

enum class TimeoutSrc { kNone, kCaller, kSpec };   // 超时参数来源：无超时 / 调用方给 / 行内默认

enum class OpTimeoutAct {          // 超时处置（封闭集）
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
    uint64_t     timeout_ns;   // 默认超时；kNone/kCaller = 0，kSpec = 行内默认
    OpTimeoutAct on_timeout;   // 超时处置
    OpLateAct    on_late;      // 迟到处置
};

// —— ErrPolicy：错误源行（intercept 切面查：归哪层、怎么处置）——

enum class ErrSource {           // 错误源（键）：同源不同处置按模块分值
    kKernelErrno, kBadFrame, kBadJson,
    kIpcOversize, kNetOversize, kIpcReject, kIpcVersion, kIpcBroken, kNetBroken,
    kAlsaXrun, kAlsaFatal, kPeriphOpFail, kPeriphDead,
    kAiTaskFail, kAiEngineDead,
};

enum class ErrLayer { kTransport, kProtocol, kState, kDevice, kTask };   // 归属层

enum class ErrAction {           // 处置（封闭集）
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
    // 专用线程 poll 就绪直读写 / 01 代跑 + 完成回流 / 常驻读单直入收货口 / watch 轮询

enum class DevIo { kStreamSpec, kDeviceSpec };   // IO 形 = attach 重载

struct DevicePolicy {
    uint8_t kind;   // 设备类别（dev.h：kStream / kTransaction / kEvent）
    DevMech mech;   // 机制
    DevIo   io;     // IO 形：流式想代跑、事务想专线程，编译不过
};

// —— Backoff：重连退避行（02/03 重连状态机查）——

struct Backoff {
    uint32_t step;          // 档位（0 起）
    uint64_t interval_ns;   // 该档等待间隔；末行 = 封顶行，此后恒取本行、不再翻倍
};

// ===== 机制三：编译期审查套件 =====

// 号段注册表：[lo, hi)；模块号 × 100，kBiz ≥ 1000
struct IdSegment {
    ModuleId owner;
    uint32_t lo, hi;        // 左闭右开
};

inline constexpr std::array kIdSegments{
    IdSegment{kDisp, 0,    100},
    IdSegment{kIpc,  100,  200},
    IdSegment{kNet,  200,  300},
    IdSegment{kTmr,  300,  400},
    IdSegment{kDev,  400,  500},
    IdSegment{kAi,   500,  600},
    IdSegment{kBio,  600,  700},
    IdSegment{kBiz,  1000, std::numeric_limits<uint32_t>::max()},
};

// 审：号段互不重叠
constexpr bool segments_disjoint() {
    for (std::size_t i = 0; i < kIdSegments.size(); ++i)
        for (std::size_t j = i + 1; j < kIdSegments.size(); ++j)
            if (kIdSegments[i].lo < kIdSegments[j].hi && kIdSegments[j].lo < kIdSegments[i].hi)
                return false;
    return true;
}
static_assert(segments_disjoint(), "policy: 号段互不重叠");

// 审：行不越段（id ∈ owner 号段）
constexpr bool row_in_segment(const EventDef& r) {
    for (const auto& s : kIdSegments)
        if (s.owner == r.owner) return r.id >= s.lo && r.id < s.hi;
    return false;
}

// 审：id 全局唯一（行表按 id 相邻不等的整组形）
template <std::size_t N>
constexpr bool rows_unique(const std::array<EventDef, N>& rows) {
    for (std::size_t i = 0; i < N; ++i)
        for (std::size_t j = i + 1; j < N; ++j)
            if (rows[i].id == rows[j].id) return false;
    return true;
}

// 审：行表整组不越段
template <std::size_t N>
constexpr bool rows_in_segments(const std::array<EventDef, N>& rows) {
    for (const auto& r : rows)
        if (!row_in_segment(r)) return false;
    return true;
}

// 审：每类在飞单恰有一行 OpPolicy（单据有超时行）
inline constexpr std::size_t kOpKindCount = 8;   // OpKind 成员数（增枚举必同步——审查锚点）

template <std::size_t N>
constexpr bool op_rows_complete(const std::array<OpPolicy, N>& rows) {
    if (N != kOpKindCount) return false;
    for (std::size_t k = 0; k < kOpKindCount; ++k) {
        std::size_t seen = 0;
        for (const auto& r : rows)
            if (static_cast<uint32_t>(r.kind) == k) ++seen;
        if (seen != 1) return false;
    }
    return true;
}
