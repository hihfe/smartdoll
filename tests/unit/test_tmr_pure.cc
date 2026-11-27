// 04 纯函数单测（白盒：tmr_internal.h）：续期、跳积压、模板交付、内部火警事件定形。
#include <catch2/catch_test_macros.hpp>

#include <any>

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
    REQUIRE(tmr::next_due(every_spec(50), 100) == 150);
    REQUIRE(tmr::next_due(every_spec(50), 123) == 173);   // 漂移不影响：按计划时刻加
}

TEST_CASE("04：should_skip 积压判定", "[04]") {
    // 一次性永不跳
    REQUIRE_FALSE(tmr::should_skip(at_spec(100), 100, 999, 1));
    // 周期：迟到不足一拍照常到（不跳）
    REQUIRE_FALSE(tmr::should_skip(every_spec(50), 100, 149, 1));
    // 落后超一拍 → 跳
    REQUIRE(tmr::should_skip(every_spec(50), 100, 150, 1));
    // 过期计数 > 1 = 积压信号 → 跳
    REQUIRE(tmr::should_skip(every_spec(50), 100, 120, 2));
}

TEST_CASE("04：skip_beats 并到最近未来拍", "[04]") {
    REQUIRE(tmr::skip_beats(every_spec(50), 100, 230) == 250);   // 150/200 两拍欠
    REQUIRE(tmr::skip_beats(every_spec(50), 100, 150) == 200);   // 严格未来拍
    REQUIRE(tmr::skip_beats(every_spec(50), 100, 50) == 100);    // 未到期不动
    REQUIRE(tmr::skip_beats(at_spec(100), 100, 999) == 100);     // 一次性不跳
}

TEST_CASE("04：deliver_template 原样拷贝只刷 ts_ns", "[04]") {
    Event tpl{};
    tpl.type = 777;
    tpl.source = 5;
    tpl.pipe = 42;
    tpl.ts_ns = 1;
    tpl.payload = 123;

    Event out = tmr::deliver_template(tpl, 999);

    REQUIRE(out.type == 777);
    REQUIRE(out.source == 5);
    REQUIRE(out.pipe == 42);
    REQUIRE(out.ts_ns == 999);
    REQUIRE(std::any_cast<int>(out.payload) == 123);
    REQUIRE(tpl.ts_ns == 1);   // 模板本体不动
}

TEST_CASE("04：make_fire_event 内部火警定形（跨进程预留的同一处理点）", "[04]") {
    TimerFire f{.handle = 8, .seq = 4, .due_ns = 500, .actual_ns = 505};

    Event ev = tmr::make_fire_event(f, 600);

    REQUIRE(ev.type == EventType(kTypeTimerFire));
    REQUIRE(ev.def == &kTypeTimerFire);
    REQUIRE(ev.ts_ns == 600);
    auto got = std::any_cast<TimerFire>(ev.payload);
    REQUIRE(got.handle == 8);
    REQUIRE(got.seq == 4);
    REQUIRE(got.due_ns == 500);
    REQUIRE(got.actual_ns == 505);
}
