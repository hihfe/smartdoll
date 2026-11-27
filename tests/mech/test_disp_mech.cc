// 01 机制测：管道投递与消费串行、代跑回流、申请单完成项翻事件、迟到丢弃、背压计数、stop 拒绝。
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "common/fixture.h"
#include "disp_internal.h"

using namespace doll::test;

TEST_CASE("01：post 任意线程到管道，按 pipe 投递", "[01]") {
    LoopFixture f;
    EventBox box(*f.d);

    disp::post(*f.d, mk_event(200, 1, box.pipe()));
    disp::post(*f.d, mk_event(201, 2, box.pipe()));

    REQUIRE(box.wait_count(2, ms(1000)));
    auto evs = box.all();
    REQUIRE(evs[0].type == 200);
    REQUIRE(evs[1].type == 201);
}

TEST_CASE("01：同管道消费串行（单线程裁决的结构保证）", "[01]") {
    LoopFixture f;

    std::atomic<int> active{0}, max_active{0}, seen{0};
    PipeId pipe = disp::open_pipe(*f.d, [&](const Event&) {
        int now = ++active;
        int prev = max_active.load();
        while (now > prev && !max_active.compare_exchange_weak(prev, now)) {}
        std::this_thread::sleep_for(ms(1));
        ++seen;
        --active;
    });

    constexpr int kThreads = 4, kPerThread = 25;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t)
        ts.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i)
                disp::post(*f.d, mk_event(1, SourceId(t), pipe));
        });
    for (auto& t : ts) t.join();

    REQUIRE(wait_until([&] { return seen.load() == kThreads * kPerThread; }, ms(5000)));
    REQUIRE(max_active.load() == 1);
}

TEST_CASE("01：submit 代跑，fn 内 post 回流闭环", "[01]") {
    LoopFixture f;
    EventBox box(*f.d);

    disp::submit(*f.d, Task{[&] {
        disp::post(*f.d, mk_event(550, 0, box.pipe()));
    }});

    REQUIRE(box.wait_type(550, ms(1000)));
}

TEST_CASE("01：submit_op + eventfd 唤醒 → kTypeWake 翻事件", "[01]") {
    LoopFixture f;
    EventBox box(*f.d);

    int efd = ::eventfd(0, EFD_NONBLOCK);
    REQUIRE(efd >= 0);
    uint64_t buf = 0;
    OpCtx op{};
    op.kind = IoKind::kEventFdWake;
    op.fd = efd;
    op.source = 9;
    op.pipe = box.pipe();
    op.buf = &buf;
    op.len = sizeof(buf);
    disp::submit_op(*f.d, op);   // 非循环线程：走代交

    uint64_t one = 1;
    REQUIRE(::write(efd, &one, sizeof(one)) == sizeof(one));

    REQUIRE(box.wait_type(kTypeWake, ms(2000)));
    auto evs = box.all();
    REQUIRE(evs[0].type == EventType(kTypeWake));
    REQUIRE(evs[0].source == 9);
    ::close(efd);
}

TEST_CASE("01：submit_op + timerfd 到期 → kTypeTimeout，过期计数透传", "[01]") {
    LoopFixture f;
    EventBox box(*f.d);

    int tfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    REQUIRE(tfd >= 0);
    itimerspec its{};
    its.it_value.tv_nsec = 20'000'000;   // 20ms
    REQUIRE(::timerfd_settime(tfd, 0, &its, nullptr) == 0);

    uint64_t expirations = 0;
    OpCtx op{};
    op.kind = IoKind::kTimerWait;
    op.fd = tfd;
    op.source = kInternalWakeSource;
    op.pipe = box.pipe();
    op.buf = &expirations;
    op.len = sizeof(expirations);
    disp::submit_op(*f.d, op);

    REQUIRE(box.wait_type(kTypeTimeout, ms(2000)));
    REQUIRE(box.count(kTypeTimeout) == 1);
    ::close(tfd);
}

TEST_CASE("01：作废单的迟到完成不产事件，结账照常（丢弃计数）", "[01]") {
    LoopFixture f;
    EventBox box(*f.d);

    int efd = ::eventfd(0, EFD_NONBLOCK);
    uint64_t buf = 0;
    OpCtx op{};
    op.kind = IoKind::kEventFdWake;
    op.fd = efd;
    op.source = 9;
    op.pipe = box.pipe();
    op.buf = &buf;
    op.len = sizeof(buf);
    op.cancelled = true;   // 提交即作废：迟到完成走拒绝路
    disp::submit_op(*f.d, op);

    uint64_t one = 1;
    REQUIRE(::write(efd, &one, sizeof(one)) == sizeof(one));

    REQUIRE(wait_until([&] { return disp::late_drop_count(*f.d) >= 1; }, ms(2000)));
    REQUIRE(box.all().empty());   // 不产生事件
    ::close(efd);
}

TEST_CASE("01：背压——kDropCount 满水位丢弃并计数", "[01]") {
    constexpr EventDef kDropRow{1002, kBiz, Qos::kDropCount};
    auto d = disp::create();   // 不跑 run()：队列只进不出，水位判定确定
    EventBox box(*d);

    for (size_t i = 0; i < disp::kQueueHighWater + 100; ++i)
        disp::post(*d, mk_event(1002, 0, box.pipe(), &kDropRow));

    REQUIRE(disp::drop_count(*d) >= 100);
    REQUIRE(disp::queue_len(*d) <= disp::kQueueHighWater);
    disp::stop(*d);
}

TEST_CASE("01：stop 后门面一律拒绝（live）；stop 排空照投", "[01]") {
    auto d = disp::create();
    EventBox box(*d);
    disp::post(*d, mk_event(1, 0, box.pipe()));
    disp::stop(*d);   // drain 队列：停前的事件排空照投
    REQUIRE(box.wait_count(1, ms(1000)));

    disp::post(*d, mk_event(1, 0, box.pipe()));   // 拒绝
    disp::submit(*d, Task{[] { REQUIRE(false); }});   // 拒绝
    OpCtx op{};
    op.kind = IoKind::kEventFdWake;
    op.fd = -1;
    disp::submit_op(*d, op);   // 拒绝

    REQUIRE(box.all().size() == 1);
    REQUIRE(disp::queue_len(*d) == 0);
}
