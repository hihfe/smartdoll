#pragma once

// ===== disp.h：01 事件分发核心——契约类型 + 门面（应用层唯一可见面） =====
// 弱类型 + 号段约定：各模块在自己的契约里定义本号段的行（EventDef）与负载结构，无需改 01。
// 门面 = disp 自由函数操作集；实现体（资源聚合体与库薄壳）住实现文件。
// 详见 doc/modules/01-dispatcher.md。
//
// 落地偏移（用法一致、形微调，相对设计稿）：
//   · IoKind：设计稿 OpCtx.kind 注释列的是 IO 形（SocketRead/…/EventFdWake），与 policy.h 的
//     OpKind（单据类别）同名冲突——拆开：IO 形 = IoKind（本文件），单据类别 = OpKind（policy.h）。
//   · OpCtx 补 fd（申请单要有作用对象）与 row（单据行指针，ticket/settle 查行用，显式作参）；
//     inflight 用 int（账本一把锁护增删查，不必再原子），OpCtx 因此可拷贝——rearm 重挂即交副本。
//   · Event 补 def（策略行指针）：入队点的背压判定查它（should_drop），调用点填自己的行，
//     "调用点传哪个行一眼可见"；不填按 kBlock 处理。
//   · 补管道门面 open_pipe/close_pipe：设计稿用法处处是"登记消费者"，门面缺形——
//     管道 = 单消费者队列，消费串行（同管道同时只跑一个消费批）。

#include <any>
#include <cstdint>
#include <functional>
#include <memory>

#include "policy.h"

// 01 基础层保留 0–99：完成项关乎在飞结账，不许丢
inline constexpr EventDef kTypeIoReadDone {1, kDisp, Qos::kBlock};   // 读完成
inline constexpr EventDef kTypeIoWriteDone{2, kDisp, Qos::kBlock};   // 写完成
inline constexpr EventDef kTypeWake       {3, kDisp, Qos::kBlock};   // eventfd 对端唤醒（数据就绪；source 定位对端）
inline constexpr EventDef kTypeError      {4, kDisp, Qos::kBlock};   // 错误事件（本层拦截后上报）
inline constexpr EventDef kTypeTimeout    {5, kDisp, Qos::kBlock};   // 定时唤醒：timerfd 读完成，通知提交方（04）
inline constexpr EventDef kTypeSubmitOp   {6, kDisp, Qos::kBlock};   // 内部：申请单代交（非循环线程 post 形），只回 01 自己的分发口

using SourceId = uint32_t;     // 来源编号：fd / 设备 / 进程
constexpr SourceId kInternalWakeSource = 0;   // 保留：内部唤醒专用，永不分配给业务/对端

using PipeId = uint32_t;       // 管道编号（各业务队列的收件地址）
constexpr PipeId kNoPipe = 0;  // 保留：无收件管道（内部唤醒等）

struct IoResult {              // 传输层完成项负载（字节数 / 错误码；数据在缓冲区）
    int32_t err = 0;
    size_t  bytes = 0;
};

enum class IoKind {            // 申请单的 IO 形：决定怎么上环、完成项怎么翻
    kSocketRead,               // 读（socket / UDS / 事件型设备）
    kSocketWrite,              // 写
    kDeviceIo,                 // 设备 IO（事件型常驻读单）
    kTimerWait,                // timerfd 读（04）：完成项带过期计数
    kEventFdWake,              // eventfd 读（内部唤醒 / 对端数据唤醒）
};

struct Event {
    EventType type;
    SourceId  source;
    PipeId    pipe;              // 收件地址：IO 完成从 OpCtx 抄来，post() 由投递方声明
    uint64_t  ts_ns;
    std::any  payload;          // 弱类型负载：传输层放 IoResult，语义事件放模块自定义结构
    const EventDef* def = nullptr;   // 策略行指针（调用点填）：背压判定查它；空 = kBlock
};

struct Task {
    std::function<void()> fn;   // 干完活自行 post() 结果回流
    uint8_t priority = 0;
};

// 申请单：一张未完成 IO/唤醒/等待的档案。提交方填内容、声明回执地址；
// 提交时把 OpCtx* 塞进完成项票根，收割时查回。生命周期由账本持有（submit_op 收副本）。
// 01 的单据行（ticket/settle 查行，行随契约出生）：IO 申请单无超时，迟到丢弃计数
inline constexpr OpPolicy kOpUringIo{OpKind::kUringIo, TimeoutSrc::kNone, 0,
                                     OpTimeoutAct::kNone, OpLateAct::kDropCount};

struct OpCtx {
    IoKind   kind;
    int      fd = -1;           // 作用对象（fd 封在申请单里，不出模块边界的例外：账本要上环）
    SourceId source = 0;
    PipeId   pipe = 0;          // 回执地址：完成项翻成事件后投回这条管道
    void*    buf = nullptr;
    size_t   len = 0;
    const OpPolicy* row = nullptr;   // 单据行（ticket/settle 查行，显式作参）；空 = kOpUringIo
    uint64_t id = 0;            // 票根号（ticket 发号回填）
    int      inflight = 0;      // 在飞计数（ticket 维护：登记=1，settle 减，归零销档）
    bool     cancelled = false; // 取消后迟到的完成项据此丢弃
};

class Dispatcher;               // 资源聚合体（定义在实现文件）：应用层只见名字，用指针/引用持有

struct DispatcherDeleter {      // 删除器住实现文件：类不完整也能销毁（设计稿缺口的落地形）
    void operator()(Dispatcher*) const;
};
using DispatcherPtr = std::unique_ptr<Dispatcher, DispatcherDeleter>;

using Consumer = std::function<void(const Event&)>;   // 管道消费者（单消费者、消费串行）

// ===== 门面：自由函数操作集，资源引用作第一参数 =====
namespace disp {
DispatcherPtr create();                 // 资源出生（RAII）
void post(Dispatcher&, Event);          // 入队并写 eventfd 唤醒循环
void submit(Dispatcher&, Task);         // 任务进线程池消费，结果由 fn 自行 post() 回流
void submit_op(Dispatcher&, OpCtx);     // 申请单提交口：登记账本 + 上环（仅循环线程，其他线程 post() 代交）
void run(Dispatcher&);                  // 进入事件循环，阻塞到 stop
void stop(Dispatcher&);                 // 资源死：置位 → 唤醒 → drain 队列 → 等在飞结清 → 释放

PipeId open_pipe(Dispatcher&, Consumer);   // 开管道 + 登记唯一消费者（谁登记谁收货）
void   close_pipe(Dispatcher&, PipeId);    // 注销管道（残留事件丢弃）
size_t queue_len(Dispatcher&);             // 入队点观测：队列深度（背压判定用）

// 背压判定纯函数（内脏纯函数，声明在此供 with.h 的 backpressure 切面调用）：
// 查 Event.def 的 Qos 行——kBlock 永不丢 / kDropCount 满水位丢新并计数 / kKeepLatest 暂同 kDropCount
//（当前无行用 kKeepLatest，"丢旧留新"到启用时在队列口实现）；def 空按 kBlock。
bool should_drop(const Event&, size_t qlen);
inline constexpr size_t kQueueHighWater = 4096;   // 丢弃水位（行值后移 policy.h）
}

uint64_t clock_ns();             // CLOCK_MONOTONIC 纳秒（01 提供，各模块共用）
