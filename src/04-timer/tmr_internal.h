#pragma once

// ===== tmr_internal.h：04 内脏头 =====
// 纯函数 + 编排入口：tests 白盒可见；应用层只 include tmr.h（封装靠文件边界）。
#include <cstddef>
#include <cstdint>

#include "disp.h"
#include "tmr.h"
#include "with.h"   // heap 切面的 Aspect 形

namespace tmr {

// ===== 纯函数：查询/变换，无副作用，可直接单测 =====

uint64_t next_due(const TimerSpec&, uint64_t due_ns);   // 周期续期：due + every（按计划时刻加）
bool     should_skip(const TimerSpec&, uint64_t due_ns, uint64_t now_ns, uint64_t expirations);
uint64_t skip_beats(const TimerSpec&, uint64_t due_ns, uint64_t now_ns);   // 并到最近未来拍
Event    deliver_template(const Event& tpl, uint64_t now_ns);   // 原样拷贝 + 刷 ts_ns
Event    make_fire_event(const TimerFire&, uint64_t now_ns);    // 内部火警定形（跨进程预留的同一处理点）

// ===== 编排入口（白盒测用；正常路径由 on_timer_event 驱动） =====

void fire_due(Timer&, uint64_t now_ns, uint64_t expirations);   // 弹堆到期档 → 交付 + 续期/结案

// ===== 观测口（测试用） =====

bool   armed(Timer&, TimerHandle);          // 存活（含暂停中）
uint64_t seq_of(Timer&, TimerHandle);       // 计划拍号（句柄已销 = 0）
size_t cancel_late_count(Timer&);           // 迟到 cancel 计数

// ===== 动堆切面：heap 的定义处（Aspect 契约见 with.md） =====
// with(heap(t), 本体)：enter 加锁（堆/表/timerfd 一把锁），exit = reschedule 恒调 + 解锁。
Aspect heap(Timer&);

}  // namespace tmr
