// ===== disp.cc：01 事件分发核心实现 =====
// 资源聚合体 + 库薄壳（liburing / moodycamel / BS::thread_pool）住本文件；
// 内脏自由函数住 disp 命名空间（不跨模块直调）；机械动作走 with 切面。
#include <moodycamel/concurrentqueue.h>   // 先于 liburing：linux/fs.h 的 BLOCK_SIZE 宏会污染其成员名
#include <BS_thread_pool.hpp>
#include <liburing.h>
#undef BLOCK_SIZE                        // 内核 uapi 宏（1<<10），本翻译单元不用，解除污染

#include <sys/eventfd.h>
#include <unistd.h>

#include <any>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "disp.h"
#include "disp_internal.h"
#include "with.h"

// ===== 库薄壳（RAII + 不可再拆的库封装，不出本文件） =====

namespace {

class Ring {                 // liburing 薄壳
    io_uring ring_{};
    bool ok_ = false;

public:
    bool init(unsigned entries) {
        ok_ = io_uring_queue_init(entries, &ring_, 0) >= 0;
        return ok_;
    }
    io_uring_sqe* get_sqe() { return ok_ ? io_uring_get_sqe(&ring_) : nullptr; }

    template <class F>
    unsigned for_each_cqe(F&& f) {
        unsigned head = 0, n = 0;
        struct io_uring_cqe* cqe;
        io_uring_for_each_cqe(&ring_, head, cqe) {
            f(*cqe);
            ++n;
        }
        return n;
    }
    void advance(unsigned n) { io_uring_cq_advance(&ring_, n); }
    void submit_and_wait(unsigned n) { io_uring_submit_and_wait(&ring_, n); }
    ~Ring() {
        if (ok_) io_uring_queue_exit(&ring_);
    }
};

class MpmcQueue {            // moodycamel 薄壳
    moodycamel::ConcurrentQueue<Event> q_;

public:
    bool enqueue(Event&& e) { return q_.enqueue(std::move(e)); }
    bool try_dequeue(Event& e) { return q_.try_dequeue(e); }
    size_t size_approx() const { return q_.size_approx(); }
};

class ThreadPool {           // BS::thread_pool 薄壳
    BS::thread_pool pool_;

public:
    void push(std::function<void()> fn) { pool_.push_task(std::move(fn)); }
};

}  // namespace

// ===== 内部类型（实现文件） =====

namespace {

struct Pipe {                // 管道：单消费者队列，消费串行（drain 形）
    PipeId      id = 0;
    Consumer    fn;
    std::mutex  mu;
    std::condition_variable cv;
    std::vector<Event> q;
    bool scheduled = false;  // 已排上消费任务（同管道同时只跑一个消费批）
    bool draining = false;
    bool closed = false;
};

}  // namespace

struct OpTable {             // 01 票根账本（Ledger 协议，见 with.h）
    using entry_type = OpCtx;
    using id_type = uint64_t;

    std::mutex mu;
    std::unordered_map<OpCtx*, std::unique_ptr<OpCtx>> by_ptr;   // 表持有，条目地址稳定
    uint64_t next_id = 1;
    size_t late_drops = 0;

    uint64_t alloc_id() {
        std::lock_guard lk(mu);
        return next_id++;
    }
    OpCtx& insert(OpCtx e) {
        auto p = std::make_unique<OpCtx>(std::move(e));
        OpCtx* raw = p.get();
        std::lock_guard lk(mu);
        by_ptr.emplace(raw, std::move(p));
        return *raw;
    }
    OpCtx* find(const OpCtx* key) {
        std::lock_guard lk(mu);
        auto it = by_ptr.find(const_cast<OpCtx*>(key));
        return it == by_ptr.end() ? nullptr : it->second.get();
    }
    void erase(OpCtx& e) {
        std::lock_guard lk(mu);
        by_ptr.erase(&e);
    }
};

class Dispatcher {
public:
    Ring       ring;
    MpmcQueue  queue;
    ThreadPool pool;
    OpTable    ops;
    int        wake_fd = -1;
    uint64_t   wake_buf = 0;
    OpCtx      wake_op;      // 原型：内部唤醒常驻读单
    std::atomic<bool> running{true};
    std::thread::id loop_id{};
    std::mutex mu;           // 管道表 + 丢弃计数
    std::unordered_map<PipeId, std::shared_ptr<Pipe>> pipes;
    PipeId     next_pipe = 1;
    std::atomic<size_t> dropped{0};

    bool stopped() const { return !running.load(); }   // live 切面查
    ~Dispatcher() {
        if (wake_fd >= 0) ::close(wake_fd);
    }
};

// ===== 内脏自由函数 =====

void DispatcherDeleter::operator()(Dispatcher* p) const { delete p; }

uint64_t clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

namespace disp {

std::optional<Event> translate(const Cqe& c, const OpCtx& ctx, uint64_t now_ns) {
    if (c.res < 0)                                        // 内核错误码就地拦截
        return Event{kTypeError, ctx.source, ctx.pipe, now_ns,
                     IoResult{-int32_t(c.res), 0}, &kTypeError};
    switch (ctx.kind) {
    case IoKind::kEventFdWake:
        if (ctx.source == kInternalWakeSource)            // 内部唤醒：只叫醒循环，丢弃
            return std::nullopt;
        return Event{kTypeWake, ctx.source, ctx.pipe, now_ns, {}, &kTypeWake};
    case IoKind::kTimerWait: {                            // 读申请单缓冲 = 8 字节 uint64 过期计数，透传不解释
        uint64_t expirations = uint64_t(c.res);
        if (c.res == int64_t(sizeof(uint64_t)) && ctx.buf)
            expirations = *static_cast<const uint64_t*>(ctx.buf);
        return Event{kTypeTimeout, ctx.source, ctx.pipe, now_ns, expirations, &kTypeTimeout};
    }
    case IoKind::kSocketRead:
    case IoKind::kDeviceIo:
        return Event{kTypeIoReadDone, ctx.source, ctx.pipe, now_ns,
                     IoResult{0, size_t(c.res)}, &kTypeIoReadDone};
    case IoKind::kSocketWrite:
        return Event{kTypeIoWriteDone, ctx.source, ctx.pipe, now_ns,
                     IoResult{0, size_t(c.res)}, &kTypeIoWriteDone};
    }
    return std::nullopt;
}

bool should_drop(const Event& ev, size_t qlen) {
    Qos qos = ev.def ? ev.def->qos : Qos::kBlock;
    switch (qos) {
    case Qos::kBlock:      return false;
    case Qos::kDropCount:  return qlen >= kQueueHighWater;
    case Qos::kKeepLatest: return qlen >= kQueueHighWater;  // 丢旧留新启用前的退化形（无行使用）
    }
    return false;
}

size_t queue_len(Dispatcher& d) { return d.queue.size_approx(); }
size_t late_drop_count(Dispatcher& d) {
    std::lock_guard lk(d.ops.mu);
    return d.ops.late_drops;
}
size_t drop_count(Dispatcher& d) { return d.dropped.load(); }

// ===== 编排函数 =====

static void drain_pipe(std::shared_ptr<Pipe> p);      // 前置声明（调度点在前）

void enqueue_event(Dispatcher& d, Event&& ev) {       // 入队点：backpressure 判定套在入队口
    bool ok = with(backpressure(d, ev), [&] {         // 业务：入队
        return d.queue.enqueue(std::move(ev));
    });
    if (!ok) d.dropped++;                             // 丢弃即计数
}

static void submit_now(Dispatcher& d, OpCtx op) {     // 上环（仅循环线程）
    const OpPolicy& row = op.row ? *op.row : kOpUringIo;
    with(ticket(d.ops, std::move(op), row, nullptr, 0, Event{}), [&](OpCtx& ctx) {
        io_uring_sqe* sqe = d.ring.get_sqe();
        if (!sqe) throw std::runtime_error("disp: SQ 满");
        io_uring_prep_read(sqe, ctx.fd, ctx.buf, ctx.len, 0);   // 读写/timerfd/eventfd 统一 read 形
        io_uring_sqe_set_data64(sqe, uint64_t(&ctx));           // 票根 = 表内条目指针
    });
}

void submit_op(Dispatcher& d, OpCtx op) {
    with(live(d), [&] {
        if (std::this_thread::get_id() == d.loop_id) submit_now(d, std::move(op));
        else post(d, Event{kTypeSubmitOp, kInternalWakeSource, kNoPipe, clock_ns(),
                           std::move(op), &kTypeSubmitOp});    // 代交：循环线程执行
    });
}

void post(Dispatcher& d, Event ev) {
    with(live(d), [&] {
        enqueue_event(d, std::move(ev));
        uint64_t one = 1;
        (void)::write(d.wake_fd, &one, sizeof(one));           // 唤醒循环
    });
}

void submit(Dispatcher& d, Task t) {
    with(live(d), [&] {
        d.pool.push(std::move(t.fn));
    });
}

PipeId open_pipe(Dispatcher& d, Consumer fn) {
    std::lock_guard lk(d.mu);
    PipeId id = d.next_pipe++;
    auto p = std::make_shared<Pipe>();
    p->id = id;
    p->fn = std::move(fn);
    d.pipes.emplace(id, std::move(p));
    return id;
}

void close_pipe(Dispatcher& d, PipeId id) {
    std::shared_ptr<Pipe> p;
    {
        std::lock_guard lk(d.mu);
        auto it = d.pipes.find(id);
        if (it == d.pipes.end()) return;
        p = it->second;
        d.pipes.erase(it);
    }
    std::unique_lock lk(p->mu);
    p->closed = true;
    p->cv.wait(lk, [&] { return !p->draining; });   // 等在跑的消费批收尾，防闭包悬垂
}

void dispatch_to_pipes(Dispatcher& d) {               // 排空队列 → 按 ev.pipe 投管道
    Event ev;
    while (d.queue.try_dequeue(ev)) {
        if (ev.type == EventType(kTypeSubmitOp)) {    // 代交：循环线程执行申请单
            submit_now(d, std::any_cast<OpCtx>(ev.payload));
            continue;
        }
        std::shared_ptr<Pipe> p;
        {
            std::lock_guard lk(d.mu);
            auto it = d.pipes.find(ev.pipe);
            if (it != d.pipes.end()) p = it->second;
        }
        if (!p) { d.dropped++; continue; }            // 无主管道：丢弃并计数
        bool schedule = false;
        {
            std::lock_guard lk(p->mu);
            if (p->closed) { d.dropped++; continue; }
            p->q.push_back(std::move(ev));
            schedule = !p->scheduled;
            if (schedule) p->scheduled = true;
        }
        if (schedule) d.pool.push([p] { drain_pipe(p); });
    }
}

static void drain_pipe(std::shared_ptr<Pipe> p) {     // 同管道消费串行：一批一任务
    for (;;) {
        std::vector<Event> batch;
        {
            std::unique_lock lk(p->mu);
            if (p->closed) { p->scheduled = false; return; }
            batch.swap(p->q);
            p->draining = true;
        }
        for (auto& ev : batch) p->fn(ev);
        {
            std::unique_lock lk(p->mu);
            p->draining = false;
            p->cv.notify_all();
            if (p->closed || p->q.empty()) { p->scheduled = false; return; }
        }
    }
}

static void harvest_into_queue(Dispatcher& d, uint64_t now_ns) {   // 批量收割 → settle → translate → 入队
    unsigned n = d.ring.for_each_cqe([&](const io_uring_cqe& cqe) {
        auto* op = reinterpret_cast<OpCtx*>(cqe.user_data);
        if (!op) return;
        Cqe c{cqe.res, cqe.user_data};
        const OpPolicy& row = op->row ? *op->row : kOpUringIo;
        if (op->kind == IoKind::kEventFdWake && op->source == kInternalWakeSource) {
            with(settle(d.ops, op, row, nullptr), rearm(d, *op), [&](OpCtx& ctx) {
                (void)translate(c, ctx, now_ns);   // 内部唤醒：只叫醒循环，结果丢弃（读完成必重挂）
            });
        } else {
            with(settle(d.ops, op, row, nullptr), [&](OpCtx& ctx) {
                if (auto ev = translate(c, ctx, now_ns)) enqueue_event(d, std::move(*ev));
            });
        }
    });
    d.ring.advance(n);
}

void run(Dispatcher& d) {
    d.loop_id = std::this_thread::get_id();
    while (d.running.load()) {
        d.ring.submit_and_wait(1);                    // ①② 提交申请单，等 ≥1 完成
        uint64_t now_ns = clock_ns();                 // 每圈取一次时钟，整批同刻
        harvest_into_queue(d, now_ns);                // ③ 批量收割
        dispatch_to_pipes(d);                         // ④ 队列 → 管道 → 线程池
    }
    dispatch_to_pipes(d);                             // 收尾排空
}

DispatcherPtr create() {
    auto d = DispatcherPtr(new Dispatcher());
    d->ring.init(256);
    d->wake_fd = ::eventfd(0, EFD_NONBLOCK);
    d->wake_op = OpCtx{.kind = IoKind::kEventFdWake, .fd = d->wake_fd,
                       .source = kInternalWakeSource, .pipe = kNoPipe,
                       .buf = &d->wake_buf, .len = sizeof(d->wake_buf)};
    submit_now(*d, d->wake_op);                       // 首挂内部唤醒读单（rearm 首挂点）
    return d;
}

void stop(Dispatcher& d) {
    if (!d.running.exchange(false)) return;           // 幂等
    uint64_t one = 1;
    (void)::write(d.wake_fd, &one, sizeof(one));      // 唤醒循环
    dispatch_to_pipes(d);                             // drain 队列（循环没跑时这里收尾）
}

}  // namespace disp
