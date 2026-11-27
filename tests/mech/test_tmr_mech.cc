// 04 机制测：到期模板原样投递、周期续期、取消/迟到、暂停/恢复、组操作、跳积压占号、01 集成重挂。
#include <catch2/catch_test_macros.hpp>

#include <thread>

#include "common/fixture.h"
#include "tmr_internal.h"

using namespace doll::test;

namespace {

Event tpl_to(EventBox& box, EventType type = 777, int payload = 42) {
    Event ev = mk_event(type, 0, box.pipe());
    ev.payload = payload;
    return ev;
}

}  // namespace

TEST_CASE("04：after 到期把模板原样投 tpl.pipe（仅刷 ts_ns）", "[04]") {
    LoopFixture f;
    auto t = tmr::create(*f.d);
    EventBox box(*f.d);

    auto h = tmr::after(*t, 20'000'000, tpl_to(box));   // 20ms

    REQUIRE(h != 0);
    REQUIRE(box.wait_type(777, ms(2000)));
    auto evs = box.all();
    REQUIRE(evs.size() == 1);
    REQUIRE(std::any_cast<int>(evs[0].payload) == 42);   // 模板原样
    REQUIRE(evs[0].ts_ns > 0);                           // 只刷 ts_ns
}

TEST_CASE("04：at 绝对时刻到期", "[04]") {
    LoopFixture f;
    auto t = tmr::create(*f.d);
    EventBox box(*f.d);

    tmr::at(*t, clock_ns() + 30'000'000, tpl_to(box));

    REQUIRE(box.wait_type(777, ms(2000)));
}

TEST_CASE("04：every 周期到期，fixed-rate 连拍（01 集成：每拍重挂读单）", "[04]") {
    LoopFixture f;
    auto t = tmr::create(*f.d);
    EventBox box(*f.d);

    auto h = tmr::every(*t, 25'000'000, tpl_to(box));   // 25ms

    REQUIRE(box.wait_count(3, ms(2000)));
    REQUIRE(tmr::seq_of(*t, h) >= 3);   // 计划拍号随续期递增
}

TEST_CASE("04：cancel 作废不投；迟到 cancel 返 false 并计数", "[04]") {
    LoopFixture f;
    auto t = tmr::create(*f.d);
    EventBox box(*f.d);

    auto h = tmr::after(*t, 80'000'000, tpl_to(box));
    REQUIRE(tmr::cancel(*t, h));
    REQUIRE_FALSE(tmr::armed(*t, h));

    auto h2 = tmr::after(*t, 20'000'000, tpl_to(box));
    REQUIRE(box.wait_type(777, ms(2000)));               // h2 已 fire
    REQUIRE_FALSE(tmr::cancel(*t, h2));                  // 迟到：false + 计数
    REQUIRE(tmr::cancel_late_count(*t) >= 1);
    REQUIRE(box.count(777) == 1);                        // h 的没投出来
    std::this_thread::sleep_for(ms(120));
    REQUIRE(box.count(777) == 1);
}

TEST_CASE("04：pause/resume 一次性——暂停不投，恢复顺延剩余时长", "[04]") {
    LoopFixture f;
    auto t = tmr::create(*f.d);
    EventBox box(*f.d);

    auto h = tmr::after(*t, 100'000'000, tpl_to(box));   // 100ms
    std::this_thread::sleep_for(ms(20));
    REQUIRE(tmr::pause(*t, h));

    std::this_thread::sleep_for(ms(200));                // 越过原定时刻
    REQUIRE(box.all().empty());                          // 暂停期不投

    REQUIRE(tmr::resume(*t, h));
    REQUIRE(box.wait_type(777, ms(2000)));               // 顺延后到
    REQUIRE_FALSE(tmr::pause(*t, h));                    // 已销档 = false
}

TEST_CASE("04：pause/resume 周期——恢复重锚，暂停期间的拍跳过占号不补", "[04]") {
    LoopFixture f;
    auto t = tmr::create(*f.d);
    EventBox box(*f.d);

    auto h = tmr::every(*t, 40'000'000, tpl_to(box));    // 40ms
    REQUIRE(box.wait_count(1, ms(2000)));
    uint64_t seq_before = tmr::seq_of(*t, h);
    REQUIRE(tmr::pause(*t, h));

    std::this_thread::sleep_for(ms(200));                // ≈5 拍空窗
    REQUIRE(box.count(777) == 1);                        // 不补拍

    REQUIRE(tmr::resume(*t, h));
    REQUIRE(box.wait_count(2, ms(2000)));                // 恢复后继续
    uint64_t seq_after = tmr::seq_of(*t, h);
    REQUIRE(seq_after >= seq_before + 2);                // 跳过的拍占号
}

TEST_CASE("04：组操作——cancel_group 整组作废", "[04]") {
    LoopFixture f;
    auto t = tmr::create(*f.d);
    EventBox box(*f.d);

    TimerSpec s1{TimerSpec::kAt, clock_ns() + 200'000'000, 0, 5};
    TimerSpec s2{TimerSpec::kEvery, 0, 30'000'000, 5};
    auto h1 = tmr::arm(*t, s1, tpl_to(box));
    auto h2 = tmr::arm(*t, s2, tpl_to(box, 888));
    (void)h1;
    REQUIRE(h2 != 0);

    REQUIRE(tmr::cancel_group(*t, 5));
    std::this_thread::sleep_for(ms(120));
    REQUIRE(box.all().empty());                          // 整组不投
    REQUIRE_FALSE(tmr::cancel_group(*t, 5));             // 组内无存活 = false
}

TEST_CASE("04：跳积压——欠拍占号不投，并到最近未来拍", "[04]") {
    LoopFixture f;
    auto t = tmr::create(*f.d);
    EventBox box(*f.d);

    auto h = tmr::every(*t, 50'000'000, tpl_to(box));    // 50ms
    std::this_thread::sleep_for(ms(10));

    tmr::fire_due(*t, clock_ns() + 500'000'000, 1);      // 白盒：模拟循环卡了 500ms
    REQUIRE(box.all().empty());                          // 跳过的拍不投
    REQUIRE(tmr::seq_of(*t, h) >= 9);                    // 占号

    REQUIRE(box.wait_count(1, ms(1500)));                // 最近未来拍照常到
}
