#pragma once

// ===== tmr.h：04 定时器——契约类型 + 门面（应用层唯一可见面） =====
// 时间堆与 timerfd 归本模块："01 管睡到 T，04 管何时是 T"。到期把登记时的用户事件模板
// 原样投出（仅刷 ts_ns）；业务不见堆、不见 timerfd、不见机制句柄。
// 详见 doc/modules/04-timer.md。

#include <cstdint>
#include <memory>

#include "disp.h"

// 04 号段 300–399。kTypeTimerFire 是内部事件（只进 04 自己的 inbox，业务不见）；
// 用户事件模板的 type 由业务自定（任意号段），04 原样透传、不占号。
inline constexpr EventDef kTypeTimerFire{300, kTmr, Qos::kBlock};

// 04 的单据行（ticket/settle 查行，行随契约出生）：timerfd 读单无超时，异常自愈重建 + 重挂
inline constexpr OpPolicy kOpTimerfdRead{OpKind::kTimerfdRead, TimeoutSrc::kNone, 0,
                                         OpTimeoutAct::kSelfHeal, OpLateAct::kNone};

using TimerHandle = uint64_t;   // 语义句柄：arm() 返回，cancel/pause 用；单调发号不复用，0 = 无效
using GroupId     = uint32_t;   // 定时器组编号；0 = 无组

struct TimerSpec {
    enum Kind { kAt, kEvery } kind;
    uint64_t at_ns;        // kAt：一次性绝对时刻（CLOCK_MONOTONIC）
    uint64_t every_ns;     // kEvery：周期间隔（锚点 = arm 时刻；跳积压后锚点平移至最近未来拍）
    GroupId  group = 0;    // 归属组
};

struct TimerFire {              // kTypeTimerFire 负载（内部）
    TimerHandle handle;
    uint64_t    seq;            // 该句柄的计划拍号（含跳过的拍）：相邻 seq 差 − 1 = 欠拍数，对账依据
    uint64_t    due_ns;         // 本拍计划到期时刻
    uint64_t    actual_ns;      // 实际触发时刻（漂移观测）
};

class Timer;                    // 资源聚合体（定义在实现文件）：应用层只见名字，用指针/引用持有

struct TimerDeleter {           // 删除器住实现文件：类不完整也能销毁（同 DispatcherPtr）
    void operator()(Timer*) const;
};
using TimerPtr = std::unique_ptr<Timer, TimerDeleter>;

// ===== 门面：自由函数操作集，资源引用作第一参数 =====
namespace tmr {
TimerPtr create(Dispatcher&);              // 资源出生（RAII）+ 首挂 timerfd 读申请单

TimerHandle arm(Timer&, TimerSpec, Event tpl);       // 登记账本 + 入堆，tpl.pipe 即收件管道

// 语法糖：arm 的参数适配形，无新不变量（实现进 .cc，头文件不带时钟依赖）
TimerHandle at(Timer&, uint64_t abs_ns, Event tpl);
TimerHandle after(Timer&, uint64_t delay_ns, Event tpl);
TimerHandle every(Timer&, uint64_t iv_ns, Event tpl);

bool cancel(Timer&, TimerHandle);            // 摘堆摘表，模板作废永不投出；迟到（已 fire）返回 false 并计数
bool pause(Timer&, TimerHandle);   bool resume(Timer&, TimerHandle);    // true = 生效；句柄已销 = false
bool pause_group(Timer&, GroupId); bool resume_group(Timer&, GroupId);  // 组内无存活条目 = false
bool cancel_group(Timer&, GroupId);
void stop(Timer&);                           // 资源死：清零 timerfd → 摘堆清账本 → 注销管道
}
