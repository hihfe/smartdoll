// 04 纯函数单测（白盒：tmr_internal.h）：续期、跳积压、模板交付、内部火警事件定形。
// 全部用例为 Arrange-Act-Assert 三段式，每段一个具名函数（规范见 doc/testing.md）。
#include <catch2/catch_test_macros.hpp>

#include <any>
#include <memory>

#include "tmr_internal.h"

namespace {

TimerSpec every_spec(uint64_t iv, GroupId g = 0) {
    return TimerSpec{TimerSpec::kEvery, 0, iv, g};
}
TimerSpec at_spec(uint64_t abs) {
    return TimerSpec{TimerSpec::kAt, abs, 0, 0};
}

}  // namespace

TEST_CASE("04：next_due 按计划时刻续期（不按实际时刻）", "[04]") {
    struct Fx {
        TimerSpec every50{};
        uint64_t due_plan = 0;
        uint64_t due_drifted = 0;
        uint64_t next_from_plan = 0;
        uint64_t next_from_drift = 0;
    };

    auto build_every_50_with_plan_and_drift_marks = [] {
        auto fx = std::make_unique<Fx>();
        fx->every50 = every_spec(50);
        fx->due_plan = 100;
        fx->due_drifted = 123;
        return fx;
    };
    auto run_next_due_from_plan_and_drift = [](Fx& fx) {
        fx.next_from_plan = tmr::next_due(fx.every50, fx.due_plan);
        fx.next_from_drift = tmr::next_due(fx.every50, fx.due_drifted);
    };
    auto expect_due_advanced_by_one_interval = [](const Fx& fx) {
        REQUIRE(fx.next_from_plan == 150);
        REQUIRE(fx.next_from_drift == 173);   // 漂移不影响：按计划时刻加
    };

    auto fx = build_every_50_with_plan_and_drift_marks();
    run_next_due_from_plan_and_drift(*fx);
    expect_due_advanced_by_one_interval(*fx);
}

TEST_CASE("04：should_skip 积压判定", "[04]") {
    struct Fx {
        TimerSpec once{};
        TimerSpec every50{};
        uint64_t due = 0;
        uint64_t now_long_past = 0;
        uint64_t now_within_beat = 0;
        uint64_t now_behind_beat = 0;
        uint64_t now_backlog = 0;
        uint64_t exp_normal = 0;
        uint64_t exp_backlog = 0;
        bool skip_once = false;
        bool skip_within_beat = false;
        bool skip_behind_beat = false;
        bool skip_backlog = false;
    };

    auto build_four_backlog_shapes = [] {
        auto fx = std::make_unique<Fx>();
        fx->once = at_spec(100);
        fx->every50 = every_spec(50);
        fx->due = 100;
        fx->now_long_past = 999;
        fx->now_within_beat = 149;
        fx->now_behind_beat = 150;
        fx->now_backlog = 120;
        fx->exp_normal = 1;
        fx->exp_backlog = 2;
        return fx;
    };
    auto run_should_skip_four_shapes = [](Fx& fx) {
        fx.skip_once = tmr::should_skip(fx.once, fx.due, fx.now_long_past, fx.exp_normal);
        fx.skip_within_beat = tmr::should_skip(fx.every50, fx.due, fx.now_within_beat, fx.exp_normal);
        fx.skip_behind_beat = tmr::should_skip(fx.every50, fx.due, fx.now_behind_beat, fx.exp_normal);
        fx.skip_backlog = tmr::should_skip(fx.every50, fx.due, fx.now_backlog, fx.exp_backlog);
    };
    auto expect_backlog_verdicts = [](const Fx& fx) {
        // 一次性永不跳
        REQUIRE_FALSE(fx.skip_once);
        // 周期：迟到不足一拍照常到（不跳）
        REQUIRE_FALSE(fx.skip_within_beat);
        // 落后超一拍 → 跳
        REQUIRE(fx.skip_behind_beat);
        // 过期计数 > 1 = 积压信号 → 跳
        REQUIRE(fx.skip_backlog);
    };

    auto fx = build_four_backlog_shapes();
    run_should_skip_four_shapes(*fx);
    expect_backlog_verdicts(*fx);
}

TEST_CASE("04：skip_beats 并到最近未来拍", "[04]") {
    struct Fx {
        TimerSpec every50{};
        TimerSpec once{};
        uint64_t due = 0;
        uint64_t now_owed = 0;
        uint64_t now_on_beat = 0;
        uint64_t now_not_due = 0;
        uint64_t now_once_past = 0;
        uint64_t skip_owed = 0;
        uint64_t skip_on_beat = 0;
        uint64_t skip_not_due = 0;
        uint64_t skip_once = 0;
    };

    auto build_due_marks_for_four_skip_cases = [] {
        auto fx = std::make_unique<Fx>();
        fx->every50 = every_spec(50);
        fx->once = at_spec(100);
        fx->due = 100;
        fx->now_owed = 230;
        fx->now_on_beat = 150;
        fx->now_not_due = 50;
        fx->now_once_past = 999;
        return fx;
    };
    auto run_skip_beats_at_four_marks = [](Fx& fx) {
        fx.skip_owed = tmr::skip_beats(fx.every50, fx.due, fx.now_owed);
        fx.skip_on_beat = tmr::skip_beats(fx.every50, fx.due, fx.now_on_beat);
        fx.skip_not_due = tmr::skip_beats(fx.every50, fx.due, fx.now_not_due);
        fx.skip_once = tmr::skip_beats(fx.once, fx.due, fx.now_once_past);
    };
    auto expect_beats_merged_to_future = [](const Fx& fx) {
        REQUIRE(fx.skip_owed == 250);      // 150/200 两拍欠
        REQUIRE(fx.skip_on_beat == 200);   // 严格未来拍
        REQUIRE(fx.skip_not_due == 100);   // 未到期不动
        REQUIRE(fx.skip_once == 100);      // 一次性不跳
    };

    auto fx = build_due_marks_for_four_skip_cases();
    run_skip_beats_at_four_marks(*fx);
    expect_beats_merged_to_future(*fx);
}

TEST_CASE("04：deliver_template 原样拷贝只刷 ts_ns", "[04]") {
    struct Fx {
        Event tpl{};
        uint64_t now_ns = 0;
        Event out{};
    };

    auto build_event_template_with_all_fields = [] {
        auto fx = std::make_unique<Fx>();
        fx->tpl.type = 777;
        fx->tpl.source = 5;
        fx->tpl.pipe = 42;
        fx->tpl.ts_ns = 1;
        fx->tpl.payload = 123;
        fx->now_ns = 999;
        return fx;
    };
    auto run_deliver_template_at_now = [](Fx& fx) {
        fx.out = tmr::deliver_template(fx.tpl, fx.now_ns);
    };
    auto expect_only_ts_ns_refreshed = [](const Fx& fx) {
        REQUIRE(fx.out.type == 777);
        REQUIRE(fx.out.source == 5);
        REQUIRE(fx.out.pipe == 42);
        REQUIRE(fx.out.ts_ns == 999);
        REQUIRE(std::any_cast<int>(fx.out.payload) == 123);
        REQUIRE(fx.tpl.ts_ns == 1);   // 模板本体不动
    };

    auto fx = build_event_template_with_all_fields();
    run_deliver_template_at_now(*fx);
    expect_only_ts_ns_refreshed(*fx);
}

TEST_CASE("04：make_fire_event 内部火警定形（跨进程预留的同一处理点）", "[04]") {
    struct Fx {
        TimerFire f{};
        uint64_t now_ns = 0;
        Event ev{};
    };

    auto build_timer_fire_input = [] {
        auto fx = std::make_unique<Fx>();
        fx->f = TimerFire{.handle = 8, .seq = 4, .due_ns = 500, .actual_ns = 505};
        fx->now_ns = 600;
        return fx;
    };
    auto run_make_fire_event_at_now = [](Fx& fx) {
        fx.ev = tmr::make_fire_event(fx.f, fx.now_ns);
    };
    auto expect_fire_event_shape_and_payload = [](const Fx& fx) {
        REQUIRE(fx.ev.type == EventType(kTypeTimerFire));
        REQUIRE(fx.ev.def == &kTypeTimerFire);
        REQUIRE(fx.ev.ts_ns == 600);
        auto got = std::any_cast<TimerFire>(fx.ev.payload);
        REQUIRE(got.handle == 8);
        REQUIRE(got.seq == 4);
        REQUIRE(got.due_ns == 500);
        REQUIRE(got.actual_ns == 505);
    };

    auto fx = build_timer_fire_input();
    run_make_fire_event_at_now(*fx);
    expect_fire_event_shape_and_payload(*fx);
}
