// 04 机制测：到期模板原样投递、周期续期、取消/迟到、暂停/恢复、组操作、跳积压占号、01 集成重挂。
// 全部用例为 Arrange-Act-Assert 三段式，每段一个具名函数（规范见 doc/testing.md）。
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
    struct Fx {
        LoopFixture f;
        TimerPtr t = tmr::create(*f.d);   // TimerPtr 在全局作用域（tmr.h），门面函数才归 tmr::
        EventBox box{*f.d};
        TimerHandle h = 0;
    };

    auto build_loop_with_timer_and_pipe = [] {
        return std::make_unique<Fx>();
    };
    auto run_arm_after_timer_20ms = [](Fx& fx) {
        fx.h = tmr::after(*fx.t, 20'000'000, tpl_to(fx.box));   // 20ms
    };
    auto expect_template_delivered_verbatim = [](Fx& fx) {
        REQUIRE(fx.h != 0);
        REQUIRE(fx.box.wait_type(777, ms(2000)));
        auto evs = fx.box.all();
        REQUIRE(evs.size() == 1);
        REQUIRE(std::any_cast<int>(evs[0].payload) == 42);   // 模板原样
        REQUIRE(evs[0].ts_ns > 0);                           // 只刷 ts_ns
    };

    auto fx = build_loop_with_timer_and_pipe();
    run_arm_after_timer_20ms(*fx);
    expect_template_delivered_verbatim(*fx);
}

TEST_CASE("04：at 绝对时刻到期", "[04]") {
    struct Fx {
        LoopFixture f;
        TimerPtr t = tmr::create(*f.d);
        EventBox box{*f.d};
    };

    auto build_loop_with_timer_and_pipe = [] {
        return std::make_unique<Fx>();
    };
    auto run_arm_at_absolute_deadline = [](Fx& fx) {
        tmr::at(*fx.t, clock_ns() + 30'000'000, tpl_to(fx.box));
    };
    auto expect_at_expiry_event_arrives = [](Fx& fx) {
        REQUIRE(fx.box.wait_type(777, ms(2000)));
    };

    auto fx = build_loop_with_timer_and_pipe();
    run_arm_at_absolute_deadline(*fx);
    expect_at_expiry_event_arrives(*fx);
}

TEST_CASE("04：every 周期到期，fixed-rate 连拍（01 集成：每拍重挂读单）", "[04]") {
    struct Fx {
        LoopFixture f;
        TimerPtr t = tmr::create(*f.d);
        EventBox box{*f.d};
        TimerHandle h = 0;
    };

    auto build_loop_with_timer_and_pipe = [] {
        return std::make_unique<Fx>();
    };
    auto run_arm_every_timer_25ms = [](Fx& fx) {
        fx.h = tmr::every(*fx.t, 25'000'000, tpl_to(fx.box));   // 25ms
    };
    auto expect_three_beats_and_seq_advanced = [](Fx& fx) {
        REQUIRE(fx.box.wait_count(3, ms(2000)));
        REQUIRE(tmr::seq_of(*fx.t, fx.h) >= 3);   // 计划拍号随续期递增
    };

    auto fx = build_loop_with_timer_and_pipe();
    run_arm_every_timer_25ms(*fx);
    expect_three_beats_and_seq_advanced(*fx);
}

TEST_CASE("04：cancel 作废不投；迟到 cancel 返 false 并计数", "[04]") {
    struct Fx {
        LoopFixture f;
        TimerPtr t = tmr::create(*f.d);
        EventBox box{*f.d};
        TimerHandle h = 0, h2 = 0;
        bool cancel_ok = false;
        bool armed_after_cancel = false;
        bool fired = false;            // h2 已 fire
        bool late_cancel_ok = false;   // 迟到 cancel 结果
        size_t late_drops = 0;
        size_t count_after_late = 0;
        size_t count_after_wait = 0;
    };

    auto build_loop_with_timer_and_pipe = [] {
        return std::make_unique<Fx>();
    };
    auto run_cancel_then_late_cancel_after_fire = [](Fx& fx) {
        fx.h = tmr::after(*fx.t, 80'000'000, tpl_to(fx.box));
        fx.cancel_ok = tmr::cancel(*fx.t, fx.h);
        fx.armed_after_cancel = tmr::armed(*fx.t, fx.h);

        fx.h2 = tmr::after(*fx.t, 20'000'000, tpl_to(fx.box));
        fx.fired = fx.box.wait_type(777, ms(2000));      // 等 h2 到期，再做迟到取消
        fx.late_cancel_ok = tmr::cancel(*fx.t, fx.h2);
        fx.late_drops = tmr::cancel_late_count(*fx.t);
        fx.count_after_late = fx.box.count(777);

        std::this_thread::sleep_for(ms(120));            // 越过 h 原定的 80ms
        fx.count_after_wait = fx.box.count(777);
    };
    auto expect_cancel_holds_and_late_cancel_rejected = [](const Fx& fx) {
        REQUIRE(fx.cancel_ok);
        REQUIRE_FALSE(fx.armed_after_cancel);
        REQUIRE(fx.fired);                    // h2 已 fire
        REQUIRE_FALSE(fx.late_cancel_ok);     // 迟到：false + 计数
        REQUIRE(fx.late_drops >= 1);
        REQUIRE(fx.count_after_late == 1);    // h 的没投出来
        REQUIRE(fx.count_after_wait == 1);
    };

    auto fx = build_loop_with_timer_and_pipe();
    run_cancel_then_late_cancel_after_fire(*fx);
    expect_cancel_holds_and_late_cancel_rejected(*fx);
}

TEST_CASE("04：pause/resume 一次性——暂停不投，恢复顺延剩余时长", "[04]") {
    struct Fx {
        LoopFixture f;
        TimerPtr t = tmr::create(*f.d);
        EventBox box{*f.d};
        TimerHandle h = 0;
        bool pause_ok = false;
        bool empty_during_pause = false;
        bool resume_ok = false;
        bool fired = false;
        bool pause_again_ok = false;
    };

    auto build_loop_with_timer_and_pipe = [] {
        return std::make_unique<Fx>();
    };
    auto run_pause_wait_resume_then_recheck = [](Fx& fx) {
        fx.h = tmr::after(*fx.t, 100'000'000, tpl_to(fx.box));   // 100ms
        std::this_thread::sleep_for(ms(20));
        fx.pause_ok = tmr::pause(*fx.t, fx.h);

        std::this_thread::sleep_for(ms(200));                // 越过原定时刻
        fx.empty_during_pause = fx.box.all().empty();        // 时刻采样：暂停窗口内无投递

        fx.resume_ok = tmr::resume(*fx.t, fx.h);
        fx.fired = fx.box.wait_type(777, ms(2000));          // 顺延后到
        fx.pause_again_ok = tmr::pause(*fx.t, fx.h);         // 已销档
    };
    auto expect_pause_blocks_and_resume_delivers = [](const Fx& fx) {
        REQUIRE(fx.pause_ok);
        REQUIRE(fx.empty_during_pause);        // 暂停期不投
        REQUIRE(fx.resume_ok);
        REQUIRE(fx.fired);                     // 顺延后到
        REQUIRE_FALSE(fx.pause_again_ok);      // 已销档 = false
    };

    auto fx = build_loop_with_timer_and_pipe();
    run_pause_wait_resume_then_recheck(*fx);
    expect_pause_blocks_and_resume_delivers(*fx);
}

TEST_CASE("04：pause/resume 周期——恢复重锚，暂停期间的拍跳过占号不补", "[04]") {
    struct Fx {
        LoopFixture f;
        TimerPtr t = tmr::create(*f.d);
        EventBox box{*f.d};
        TimerHandle h = 0;
        bool first_beat = false;
        uint64_t seq_before = 0;
        bool pause_ok = false;
        size_t count_during_pause = 0;
        bool resume_ok = false;
        bool resumed_beat = false;
        uint64_t seq_after = 0;
    };

    auto build_loop_with_timer_and_pipe = [] {
        return std::make_unique<Fx>();
    };
    auto run_pause_resume_periodic_with_seq_samples = [](Fx& fx) {
        fx.h = tmr::every(*fx.t, 40'000'000, tpl_to(fx.box));    // 40ms
        fx.first_beat = fx.box.wait_count(1, ms(2000));
        fx.seq_before = tmr::seq_of(*fx.t, fx.h);
        fx.pause_ok = tmr::pause(*fx.t, fx.h);

        std::this_thread::sleep_for(ms(200));                // ≈5 拍空窗
        fx.count_during_pause = fx.box.count(777);           // 时刻采样：空窗内无投递

        fx.resume_ok = tmr::resume(*fx.t, fx.h);
        fx.resumed_beat = fx.box.wait_count(2, ms(2000));    // 等恢复后第二件
        fx.seq_after = tmr::seq_of(*fx.t, fx.h);
    };
    auto expect_no_beats_during_pause_and_seq_advances = [](const Fx& fx) {
        REQUIRE(fx.first_beat);
        REQUIRE(fx.pause_ok);
        REQUIRE(fx.count_during_pause == 1);                 // 不补拍
        REQUIRE(fx.resume_ok);
        REQUIRE(fx.resumed_beat);                           // 恢复后继续
        REQUIRE(fx.seq_after >= fx.seq_before + 2);          // 跳过的拍占号
    };

    auto fx = build_loop_with_timer_and_pipe();
    run_pause_resume_periodic_with_seq_samples(*fx);
    expect_no_beats_during_pause_and_seq_advances(*fx);
}

TEST_CASE("04：组操作——cancel_group 整组作废", "[04]") {
    struct Fx {
        LoopFixture f;
        TimerPtr t = tmr::create(*f.d);
        EventBox box{*f.d};
        TimerSpec s1{};
        TimerSpec s2{};
        TimerHandle h1 = 0, h2 = 0;
        bool group_cancelled = false;
        bool empty_after_cancel = false;
        bool group_cancelled_again = false;
    };

    auto build_timer_with_group_specs = [] {
        auto fx = std::make_unique<Fx>();
        fx->s1 = TimerSpec{TimerSpec::kAt, clock_ns() + 200'000'000, 0, 5};
        fx->s2 = TimerSpec{TimerSpec::kEvery, 0, 30'000'000, 5};
        return fx;
    };
    auto run_arm_group_then_cancel_group_twice = [](Fx& fx) {
        fx.h1 = tmr::arm(*fx.t, fx.s1, tpl_to(fx.box));
        fx.h2 = tmr::arm(*fx.t, fx.s2, tpl_to(fx.box, 888));

        fx.group_cancelled = tmr::cancel_group(*fx.t, 5);
        std::this_thread::sleep_for(ms(120));
        fx.empty_after_cancel = fx.box.all().empty();
        fx.group_cancelled_again = tmr::cancel_group(*fx.t, 5);
    };
    auto expect_whole_group_voided = [](const Fx& fx) {
        REQUIRE(fx.h2 != 0);
        REQUIRE(fx.group_cancelled);
        REQUIRE(fx.empty_after_cancel);                   // 整组不投
        REQUIRE_FALSE(fx.group_cancelled_again);          // 组内无存活 = false
    };

    auto fx = build_timer_with_group_specs();
    run_arm_group_then_cancel_group_twice(*fx);
    expect_whole_group_voided(*fx);
}

TEST_CASE("04：跳积压——欠拍占号不投，并到最近未来拍", "[04]") {
    struct Fx {
        LoopFixture f;
        TimerPtr t = tmr::create(*f.d);
        EventBox box{*f.d};
        TimerHandle h = 0;
        bool empty_after_skip = false;
        uint64_t seq_after_skip = 0;
        bool next_beat = false;
    };

    auto build_loop_with_timer_and_pipe = [] {
        return std::make_unique<Fx>();
    };
    auto run_skip_backlog_via_fire_due = [](Fx& fx) {
        fx.h = tmr::every(*fx.t, 50'000'000, tpl_to(fx.box));    // 50ms
        std::this_thread::sleep_for(ms(10));

        tmr::fire_due(*fx.t, clock_ns() + 500'000'000, 1);      // 白盒：模拟循环卡了 500ms
        fx.empty_after_skip = fx.box.all().empty();              // 时刻采样：欠拍窗口内无投递
        fx.seq_after_skip = tmr::seq_of(*fx.t, fx.h);

        fx.next_beat = fx.box.wait_count(1, ms(1500));           // 等最近未来拍
    };
    auto expect_backlog_skipped_and_next_beat_arrives = [](const Fx& fx) {
        REQUIRE(fx.empty_after_skip);            // 跳过的拍不投
        REQUIRE(fx.seq_after_skip >= 9);         // 占号
        REQUIRE(fx.next_beat);                   // 最近未来拍照常到
    };

    auto fx = build_loop_with_timer_and_pipe();
    run_skip_backlog_via_fire_due(*fx);
    expect_backlog_skipped_and_next_beat_arrives(*fx);
}
