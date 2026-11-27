// ===== tmr.cc：04 定时器实现 =====
// 时间堆（Boost.Heap d_ary_heap + handle）+ timerfd（每进程一个）+ 语义账本；
// "01 管睡到 T，04 管何时是 T"：到期把用户事件模板原样投出（仅刷 ts_ns）。
// 机械动作走 with 切面：动堆一律 with(heap(t), …)（exit = reschedule 恒调）。
#include <boost/heap/d_ary_heap.hpp>

#include <sys/timerfd.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "disp.h"
#include "tmr.h"
#include "tmr_internal.h"
#include "with.h"

// ===== 库薄壳 =====

namespace {

class TimerFd {              // timerfd 薄壳
    int fd_ = -1;

public:
    bool init() {
        fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        return fd_ >= 0;
    }
    void rebuild() {
        if (fd_ >= 0) ::close(fd_);
        init();
    }
    void set(uint64_t abs_ns) {   // TFD_TIMER_ABSTIME：绝对时刻，改期不漂移
        itimerspec its{};
        its.it_value.tv_sec = time_t(abs_ns / 1'000'000'000);
        its.it_value.tv_nsec = long(abs_ns % 1'000'000'000);
        ::timerfd_settime(fd_, TFD_TIMER_ABSTIME, &its, nullptr);
    }
    void clear() {
        itimerspec its{};
        ::timerfd_settime(fd_, 0, &its, nullptr);
    }
    int fd() const { return fd_; }
    ~TimerFd() {
        if (fd_ >= 0) ::close(fd_);
    }
};

struct HeapItem {            // 堆内形态：到期时刻 + 语义句柄
    uint64_t    due_ns;
    TimerHandle id;
};
struct HeapItemCmp {         // 最小到期在顶
    bool operator()(const HeapItem& a, const HeapItem& b) const { return a.due_ns > b.due_ns; }
};

class TimerHeap {            // Boost.Heap 薄壳（mutable：改期/删除走句柄）
public:
    using Heap = boost::heap::d_ary_heap<HeapItem, boost::heap::arity<2>,
                                        boost::heap::compare<HeapItemCmp>,
                                        boost::heap::mutable_<true>>;
    using Handle = Heap::handle_type;

    Handle push(uint64_t due_ns, TimerHandle id) { return h_.push(HeapItem{due_ns, id}); }
    void   erase(Handle hd) { h_.erase(hd); }
    void   pop() { h_.pop(); }
    const HeapItem& top() const { return h_.top(); }
    bool   empty() const { return h_.empty(); }
    uint64_t top_deadline() const { return h_.top().due_ns; }

private:
    Heap h_;
};

}  // namespace

// ===== 内部类型 =====

struct TimerEntry {          // 一档定时器的档案（账本条目）
    TimerHeap::Handle heap;
    bool       in_heap = false;
    TimerSpec  spec;
    Event      tpl;          // 用户事件模板：到期原样投出（仅刷 ts_ns）
    uint64_t   next_ns = 0;  // 下一到期时刻（绝对，CLOCK_MONOTONIC）
    uint64_t   seq = 0;      // 计划拍号：每续期/跳拍 +1（跳过的拍也占号）
    bool       paused = false;
    uint64_t   paused_at_ns = 0;
};

class Timer {
public:
    explicit Timer(Dispatcher& d_) : d(d_) {}

    PipeId      inbox = kNoPipe;
    Dispatcher& d;
    std::mutex  mu;          // 堆/表/timerfd 设时共用一把锁（多写单交付）
    TimerFd     tfd;
    TimerHeap   heap;
    std::unordered_map<TimerHandle, TimerEntry> entries;
    std::unordered_map<GroupId, std::vector<TimerHandle>> groups;
    TimerHandle next = 1;
    size_t      late_cancels = 0;
    OpCtx       wait;        // timerfd 常驻读申请单（原型；每 fire 重挂一副本）
    uint64_t    expirations_buf = 0;
    std::atomic<bool> running{true};

    bool stopped() const { return !running.load(); }   // live 切面查
};

// ===== 内脏自由函数 =====

namespace tmr {

// ===== 纯函数 =====

uint64_t next_due(const TimerSpec& s, uint64_t due_ns) {
    return due_ns + (s.kind == TimerSpec::kEvery ? s.every_ns : 0);
}

bool should_skip(const TimerSpec& s, uint64_t due_ns, uint64_t now_ns, uint64_t expirations) {
    if (s.kind != TimerSpec::kEvery || s.every_ns == 0) return false;
    return expirations > 1 || now_ns >= due_ns + s.every_ns;   // 落后超一拍 / 过期计数 = 积压
}

uint64_t skip_beats(const TimerSpec& s, uint64_t due_ns, uint64_t now_ns) {
    if (s.kind != TimerSpec::kEvery || s.every_ns == 0 || now_ns < due_ns) return due_ns;
    uint64_t k = (now_ns - due_ns) / s.every_ns + 1;   // 严格未来的最近拍
    return due_ns + k * s.every_ns;
}

Event deliver_template(const Event& tpl, uint64_t now_ns) {
    Event out = tpl;
    out.ts_ns = now_ns;
    return out;
}

Event make_fire_event(const TimerFire& f, uint64_t now_ns) {
    return Event{kTypeTimerFire, kInternalWakeSource, kNoPipe, now_ns, f, &kTypeTimerFire};
}

// ===== 内部（锁内形：heap 切面之下） =====

static void reschedule_locked(Timer& t) {          // 动堆切面的 exit 体：堆顶设时，堆空停表
    if (t.heap.empty()) t.tfd.clear();
    else t.tfd.set(t.heap.top_deadline());
}

static void group_remove_locked(Timer& t, GroupId g, TimerHandle h) {
    if (!g) return;
    auto it = t.groups.find(g);
    if (it == t.groups.end()) return;
    auto& v = it->second;
    for (auto i = v.begin(); i != v.end(); ++i)
        if (*i == h) { v.erase(i); break; }
    if (v.empty()) t.groups.erase(it);
}

static bool cancel_locked(Timer& t, TimerHandle h) {
    auto it = t.entries.find(h);
    if (it == t.entries.end()) { t.late_cancels++; return false; }   // 迟到（已 fire）：false 并计数
    if (it->second.in_heap) t.heap.erase(it->second.heap);
    group_remove_locked(t, it->second.spec.group, h);
    t.entries.erase(it);
    return true;
}

static bool pause_locked(Timer& t, TimerHandle h) {
    auto it = t.entries.find(h);
    if (it == t.entries.end() || it->second.paused) return false;
    if (it->second.in_heap) { t.heap.erase(it->second.heap); it->second.in_heap = false; }
    it->second.paused = true;
    it->second.paused_at_ns = clock_ns();
    return true;
}

static bool resume_locked(Timer& t, TimerHandle h) {
    auto it = t.entries.find(h);
    if (it == t.entries.end() || !it->second.paused) return false;
    TimerEntry& e = it->second;
    uint64_t now = clock_ns();
    if (e.spec.kind == TimerSpec::kAt) {
        e.next_ns += now - e.paused_at_ns;               // 一次性顺延剩余时长
    } else {
        e.seq += (now - e.paused_at_ns) / e.spec.every_ns;   // 暂停期间的拍占号不补
        e.next_ns = now + e.spec.every_ns;               // 周期：从恢复时刻重锚
    }
    e.paused = false;
    e.heap = t.heap.push(e.next_ns, h);
    e.in_heap = true;
    return true;
}

// 同一处理点：timerfd 路由在此合成 kTypeTimerFire（跨进程模式改由 02 抵达，处理同此）
static void fire_one(Timer& t, const TimerFire& f, uint64_t now_ns) {
    auto it = t.entries.find(f.handle);
    if (it == t.entries.end()) return;
    TimerEntry& e = it->second;
    disp::post(t.d, deliver_template(e.tpl, now_ns));    // 用户事件模板原样投出
    if (e.spec.kind == TimerSpec::kAt) {
        group_remove_locked(t, e.spec.group, f.handle);
        t.entries.erase(it);                             // 一次性到期即销档
    } else {
        e.next_ns = next_due(e.spec, e.next_ns);         // 按计划时刻续期（不累积漂移）
        e.heap = t.heap.push(e.next_ns, f.handle);
        e.in_heap = true;
    }
}

// ===== 编排函数 =====

static void rearm_wait(Timer& t);   // 前置：on_timer_event 的自愈路要用

Aspect heap(Timer& t) {
    return Aspect{
        &t,
        [](void* s) -> Outcome {
            static_cast<Timer*>(s)->mu.lock();
            return Outcome::kEnter;
        },
        [](void* s) {
            Timer* x = static_cast<Timer*>(s);
            std::lock_guard lk(x->mu, std::adopt_lock);   // 解锁恒执行（含异常路）
            reschedule_locked(*x);                        // 动堆必重设
        },
        "heap",
    };
}

void fire_due(Timer& t, uint64_t now_ns, uint64_t expirations) {
    with(live(t), heap(t), rearm(t.d, t.wait), [&] {
        while (!t.heap.empty() && t.heap.top().due_ns <= now_ns) {
            HeapItem top = t.heap.top();
            t.heap.pop();
            auto it = t.entries.find(top.id);
            if (it == t.entries.end()) continue;
            TimerEntry& e = it->second;
            e.in_heap = false;
            if (should_skip(e.spec, e.next_ns, now_ns, expirations)) {
                uint64_t new_due = skip_beats(e.spec, e.next_ns, now_ns);
                e.seq += (new_due - e.next_ns) / e.spec.every_ns;   // 欠拍占号
                e.next_ns = new_due;
                e.heap = t.heap.push(new_due, top.id);              // 并到最近未来拍
                e.in_heap = true;
                continue;
            }
            e.seq += 1;
            TimerFire fire{top.id, e.seq, e.next_ns, now_ns};
            fire_one(t, std::any_cast<TimerFire>(make_fire_event(fire, now_ns).payload), now_ns);
        }
    });
}

void on_timer_event(Timer& t, const Event& ev) {
    with(ev, {
        {kTypeTimeout, [&](const Event& e) {
            fire_due(t, clock_ns(), std::any_cast<uint64_t>(e.payload));
        }},
        {kTypeError, [&](const Event&) {              // 自愈重建 + 重挂（异常即自愈）
            with(heap(t), [&] {
                t.tfd.rebuild();
                t.wait.fd = t.tfd.fd();
            });
            rearm_wait(t);
        }},
    });
}

static void rearm_wait(Timer& t) {                    // 挂/重挂 timerfd 读申请单（经 01 代交）
    disp::submit_op(t.d, t.wait);
}

// ===== 门面 =====

TimerPtr create(Dispatcher& d) {
    TimerPtr t(new Timer(d));
    t->tfd.init();
    t->inbox = disp::open_pipe(d, [raw = t.get()](const Event& ev) { on_timer_event(*raw, ev); });
    OpCtx w{};
    w.kind = IoKind::kTimerWait;
    w.fd = t->tfd.fd();
    w.source = kInternalWakeSource;
    w.pipe = t->inbox;
    w.buf = &t->expirations_buf;
    w.len = sizeof(t->expirations_buf);
    w.row = &kOpTimerfdRead;
    t->wait = w;
    rearm_wait(*t);                                   // 首挂（rearm 首挂点）
    return t;
}

TimerHandle arm(Timer& t, TimerSpec spec, Event tpl) {
    return with(live(t), heap(t), [&]() -> TimerHandle {
        TimerHandle h = t.next++;                       // 发号（单调不复用）
        TimerEntry e;
        e.spec = spec;
        e.tpl = std::move(tpl);
        e.next_ns = spec.kind == TimerSpec::kAt ? spec.at_ns : clock_ns() + spec.every_ns;
        e.heap = t.heap.push(e.next_ns, h);
        e.in_heap = true;
        t.entries.emplace(h, std::move(e));
        if (spec.group) t.groups[spec.group].push_back(h);
        return h;
    });
}

bool cancel(Timer& t, TimerHandle h) {
    return with(live(t), heap(t), [&] { return cancel_locked(t, h); });
}

bool pause(Timer& t, TimerHandle h) {
    return with(live(t), heap(t), [&] { return pause_locked(t, h); });
}

bool resume(Timer& t, TimerHandle h) {
    return with(live(t), heap(t), [&] { return resume_locked(t, h); });
}

bool cancel_group(Timer& t, GroupId g) {
    return with(live(t), heap(t), [&] {
        auto it = t.groups.find(g);
        if (it == t.groups.end() || it->second.empty()) return false;
        auto handles = it->second;                      // 摘组会动表，先拷贝
        for (TimerHandle h : handles) cancel_locked(t, h);
        return true;
    });
}

bool pause_group(Timer& t, GroupId g) {
    return with(live(t), heap(t), [&] {
        auto it = t.groups.find(g);
        if (it == t.groups.end() || it->second.empty()) return false;
        for (TimerHandle h : it->second) pause_locked(t, h);
        return true;
    });
}

bool resume_group(Timer& t, GroupId g) {
    return with(live(t), heap(t), [&] {
        auto it = t.groups.find(g);
        if (it == t.groups.end() || it->second.empty()) return false;
        for (TimerHandle h : it->second) resume_locked(t, h);
        return true;
    });
}

void stop(Timer& t) {
    if (!t.running.exchange(false)) return;             // 幂等
    with(heap(t), [&] {                                 // 清零 timerfd → 摘堆清账本
        t.tfd.clear();
        while (!t.heap.empty()) t.heap.pop();
        t.entries.clear();
        t.groups.clear();
    });
    disp::close_pipe(t.d, t.inbox);                     // 注销管道
}

// ===== 观测口（测试用） =====

bool armed(Timer& t, TimerHandle h) {
    std::lock_guard lk(t.mu);
    return t.entries.find(h) != t.entries.end();
}

uint64_t seq_of(Timer& t, TimerHandle h) {
    std::lock_guard lk(t.mu);
    auto it = t.entries.find(h);
    return it == t.entries.end() ? 0 : it->second.seq;
}

size_t cancel_late_count(Timer& t) {
    std::lock_guard lk(t.mu);
    return t.late_cancels;
}

}  // namespace tmr

// ===== 语法糖（arm 的参数适配形） =====

namespace tmr {
TimerHandle at(Timer& t, uint64_t abs_ns, Event tpl) {
    return arm(t, TimerSpec{TimerSpec::kAt, abs_ns, 0, 0}, std::move(tpl));
}
TimerHandle after(Timer& t, uint64_t delay_ns, Event tpl) {
    return arm(t, TimerSpec{TimerSpec::kAt, clock_ns() + delay_ns, 0, 0}, std::move(tpl));
}
TimerHandle every(Timer& t, uint64_t iv_ns, Event tpl) {
    return arm(t, TimerSpec{TimerSpec::kEvery, 0, iv_ns, 0}, std::move(tpl));
}
}

void TimerDeleter::operator()(Timer* p) const {
    if (p) {
        tmr::stop(*p);
        delete p;
    }
}
