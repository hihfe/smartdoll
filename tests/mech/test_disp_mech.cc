// 01 机制测：管道投递与消费串行、代跑回流、申请单完成项翻事件、迟到丢弃、背压计数、stop 拒绝。
// 全部用例为 Arrange-Act-Assert 三段式，每段一个具名函数（规范见 doc/testing.md）。
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "common/fixture.h"
#include "disp_internal.h"

using namespace doll::test;

TEST_CASE("01：post 任意线程到管道，按 pipe 投递", "[01]") {
    struct Fx {
        LoopFixture f;
        EventBox box{*f.d};
    };

    auto build_loop_with_event_box = [] {
        return std::make_unique<Fx>();
    };
    auto run_post_two_events_to_pipe = [](Fx& fx) {
        disp::post(*fx.f.d, mk_event(200, 1, fx.box.pipe()));
        disp::post(*fx.f.d, mk_event(201, 2, fx.box.pipe()));
    };
    auto expect_events_arrive_in_order = [](Fx& fx) {
        REQUIRE(fx.box.wait_count(2, ms(1000)));
        auto evs = fx.box.all();
        REQUIRE(evs[0].type == 200);
        REQUIRE(evs[1].type == 201);
    };

    auto fx = build_loop_with_event_box();
    run_post_two_events_to_pipe(*fx);
    expect_events_arrive_in_order(*fx);
}

TEST_CASE("01：同管道消费串行（单线程裁决的结构保证）", "[01]") {
    struct Fx {
        int kThreads = 4;        // 投递线程数
        int kPerThread = 25;     // 每线程投递件数
        LoopFixture f;
        std::atomic<int> active{0}, max_active{0}, seen{0};
        PipeId pipe = 0;
    };

    auto build_loop_with_concurrency_tracking_pipe = [] {
        auto fx = std::make_unique<Fx>();
        fx->pipe = disp::open_pipe(*fx->f.d, [p = fx.get()](const Event&) {
            int now = ++p->active;
            int prev = p->max_active.load();
            while (now > prev && !p->max_active.compare_exchange_weak(prev, now)) {}
            std::this_thread::sleep_for(ms(1));
            ++p->seen;
            --p->active;
        });
        return fx;
    };
    auto run_post_100_events_from_four_threads = [](Fx& fx) {
        std::vector<std::thread> ts;
        for (int t = 0; t < fx.kThreads; ++t)
            ts.emplace_back([&fx, t] {
                for (int i = 0; i < fx.kPerThread; ++i)
                    disp::post(*fx.f.d, mk_event(1, SourceId(t), fx.pipe));
            });
        for (auto& t : ts) t.join();
    };
    auto expect_serial_consumption_of_all_events = [](const Fx& fx) {
        REQUIRE(wait_until([&] { return fx.seen.load() == fx.kThreads * fx.kPerThread; }, ms(5000)));
        REQUIRE(fx.max_active.load() == 1);
    };

    auto fx = build_loop_with_concurrency_tracking_pipe();
    run_post_100_events_from_four_threads(*fx);
    expect_serial_consumption_of_all_events(*fx);
}

TEST_CASE("01：submit 代跑，fn 内 post 回流闭环", "[01]") {
    struct Fx {
        LoopFixture f;
        EventBox box{*f.d};
    };

    auto build_loop_with_event_box = [] {
        return std::make_unique<Fx>();
    };
    auto run_submit_task_posting_back = [](Fx& fx) {
        disp::submit(*fx.f.d, Task{[&fx] {
            disp::post(*fx.f.d, mk_event(550, 0, fx.box.pipe()));
        }});
    };
    auto expect_flowback_event_arrives = [](Fx& fx) {
        REQUIRE(fx.box.wait_type(550, ms(1000)));
    };

    auto fx = build_loop_with_event_box();
    run_submit_task_posting_back(*fx);
    expect_flowback_event_arrives(*fx);
}

TEST_CASE("01：submit_op + eventfd 唤醒 → kTypeWake 翻事件", "[01]") {
    struct Fx {
        LoopFixture f;
        EventBox box{*f.d};
        int efd = -1;
        uint64_t buf = 0;
        OpCtx op{};
        ssize_t write_bytes = -1;

        ~Fx() { if (efd >= 0) ::close(efd); }   // fd 随夹具收口
    };

    auto build_loop_with_eventfd_wake_op = [] {
        auto fx = std::make_unique<Fx>();
        fx->efd = ::eventfd(0, EFD_NONBLOCK);
        REQUIRE(fx->efd >= 0);
        fx->op.kind = IoKind::kEventFdWake;
        fx->op.fd = fx->efd;
        fx->op.source = 9;
        fx->op.pipe = fx->box.pipe();
        fx->op.buf = &fx->buf;
        fx->op.len = sizeof(fx->buf);
        return fx;
    };
    auto run_submit_wake_op_then_signal_eventfd = [](Fx& fx) {
        disp::submit_op(*fx.f.d, fx.op);
        uint64_t one = 1;
        fx.write_bytes = ::write(fx.efd, &one, sizeof(one));
    };
    auto expect_wake_event_from_source_9 = [](Fx& fx) {
        REQUIRE(fx.write_bytes == ssize_t(sizeof(uint64_t)));
        REQUIRE(fx.box.wait_type(kTypeWake, ms(2000)));
        auto evs = fx.box.all();
        REQUIRE(evs[0].type == EventType(kTypeWake));
        REQUIRE(evs[0].source == 9);
    };

    auto fx = build_loop_with_eventfd_wake_op();
    run_submit_wake_op_then_signal_eventfd(*fx);
    expect_wake_event_from_source_9(*fx);
}

TEST_CASE("01：submit_op + timerfd 到期 → kTypeTimeout，过期计数透传", "[01]") {
    struct Fx {
        LoopFixture f;
        EventBox box{*f.d};
        int tfd = -1;
        uint64_t expirations = 0;
        OpCtx op{};

        ~Fx() { if (tfd >= 0) ::close(tfd); }   // fd 随夹具收口
    };

    auto build_loop_with_armed_timerfd_wait_op = [] {
        auto fx = std::make_unique<Fx>();
        fx->tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        REQUIRE(fx->tfd >= 0);
        itimerspec its{};
        its.it_value.tv_nsec = 20'000'000;   // 20ms
        REQUIRE(::timerfd_settime(fx->tfd, 0, &its, nullptr) == 0);
        fx->op.kind = IoKind::kTimerWait;
        fx->op.fd = fx->tfd;
        fx->op.source = kInternalWakeSource;
        fx->op.pipe = fx->box.pipe();
        fx->op.buf = &fx->expirations;
        fx->op.len = sizeof(fx->expirations);
        return fx;
    };
    auto run_submit_timerfd_wait_op = [](Fx& fx) {
        disp::submit_op(*fx.f.d, fx.op);
    };
    auto expect_single_timeout_event = [](Fx& fx) {
        REQUIRE(fx.box.wait_type(kTypeTimeout, ms(2000)));
        REQUIRE(fx.box.count(kTypeTimeout) == 1);
    };

    auto fx = build_loop_with_armed_timerfd_wait_op();
    run_submit_timerfd_wait_op(*fx);
    expect_single_timeout_event(*fx);
}

TEST_CASE("01：作废单的迟到完成不产事件，结账照常（丢弃计数）", "[01]") {
    struct Fx {
        LoopFixture f;
        EventBox box{*f.d};
        int efd = -1;
        uint64_t buf = 0;
        OpCtx op{};
        ssize_t write_bytes = -1;

        ~Fx() { if (efd >= 0) ::close(efd); }   // fd 随夹具收口
    };

    auto build_cancelled_op_on_eventfd = [] {
        auto fx = std::make_unique<Fx>();
        fx->efd = ::eventfd(0, EFD_NONBLOCK);
        fx->op.kind = IoKind::kEventFdWake;
        fx->op.fd = fx->efd;
        fx->op.source = 9;
        fx->op.pipe = fx->box.pipe();
        fx->op.buf = &fx->buf;
        fx->op.len = sizeof(fx->buf);
        fx->op.cancelled = true;   // 提交即作废：迟到完成走拒绝路
        return fx;
    };
    auto run_submit_cancelled_op_then_signal_eventfd = [](Fx& fx) {
        disp::submit_op(*fx.f.d, fx.op);
        uint64_t one = 1;
        fx.write_bytes = ::write(fx.efd, &one, sizeof(one));
    };
    auto expect_late_completion_dropped_without_event = [](Fx& fx) {
        REQUIRE(fx.write_bytes == ssize_t(sizeof(uint64_t)));
        REQUIRE(wait_until([&] { return disp::late_drop_count(*fx.f.d) >= 1; }, ms(2000)));
        REQUIRE(fx.box.all().empty());   // 不产生事件
    };

    auto fx = build_cancelled_op_on_eventfd();
    run_submit_cancelled_op_then_signal_eventfd(*fx);
    expect_late_completion_dropped_without_event(*fx);
}

TEST_CASE("01：背压——kDropCount 满水位丢弃并计数", "[01]") {
    struct Fx {
        EventDef row{1002, kBiz, Qos::kDropCount};
        DispatcherPtr d = disp::create();
        EventBox box{*d};

        ~Fx() { disp::stop(*d); }   // 不跑 run()：停掉即收口
    };

    auto build_dispatcher_without_loop = [] {
        return std::make_unique<Fx>();
    };
    auto run_post_events_past_high_water = [](Fx& fx) {
        for (size_t i = 0; i < disp::kQueueHighWater + 100; ++i)
            disp::post(*fx.d, mk_event(1002, 0, fx.box.pipe(), &fx.row));
    };
    auto expect_drops_counted_and_queue_capped = [](const Fx& fx) {
        REQUIRE(disp::drop_count(*fx.d) >= 100);
        REQUIRE(disp::queue_len(*fx.d) <= disp::kQueueHighWater);
    };

    auto fx = build_dispatcher_without_loop();
    run_post_events_past_high_water(*fx);
    expect_drops_counted_and_queue_capped(*fx);
}

TEST_CASE("01：stop 后门面一律拒绝（live）；stop 排空照投", "[01]") {
    struct Fx {
        DispatcherPtr d = disp::create();
        EventBox box{*d};
        OpCtx op{};
        bool drained = false;   // stop 前的事件排空照投
        bool task_ran = false;  // stop 后 submit 的任务：拒绝 = 不跑
    };

    auto build_dispatcher_with_event_box = [] {
        auto fx = std::make_unique<Fx>();
        fx->op.kind = IoKind::kEventFdWake;
        fx->op.fd = -1;
        return fx;
    };
    auto run_stop_then_reject_all_facades = [](Fx& fx) {
        disp::post(*fx.d, mk_event(1, 0, fx.box.pipe()));
        disp::stop(*fx.d);   // drain 队列：停前的事件排空照投
        fx.drained = fx.box.wait_count(1, ms(1000));

        disp::post(*fx.d, mk_event(1, 0, fx.box.pipe()));            // 拒绝
        disp::submit(*fx.d, Task{[&fx] { fx.task_ran = true; }});    // 拒绝：任务不进池
        disp::submit_op(*fx.d, fx.op);                               // 拒绝
    };
    auto expect_drained_then_facades_rejected = [](const Fx& fx) {
        REQUIRE(fx.drained);
        REQUIRE(fx.box.all().size() == 1);
        REQUIRE(disp::queue_len(*fx.d) == 0);
        REQUIRE_FALSE(fx.task_ran);   // submit 拒绝：任务不跑（原任务内 REQUIRE(false) 探针）
    };

    auto fx = build_dispatcher_with_event_box();
    run_stop_then_reject_all_facades(*fx);
    expect_drained_then_facades_rejected(*fx);
}
