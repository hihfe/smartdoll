// 01 纯函数单测（白盒：disp_internal.h）：translate 翻译定形 + 错误拦截、should_drop 背压判定。
// 全部用例为 Arrange-Act-Assert 三段式，每段一个具名函数（规范见 doc/testing.md）。
#include <catch2/catch_test_macros.hpp>

#include <any>
#include <memory>
#include <optional>

#include "disp_internal.h"

namespace {

OpCtx read_op(SourceId src = 7, PipeId pipe = 42) {
    OpCtx ctx{};
    ctx.kind = IoKind::kSocketRead;
    ctx.source = src;
    ctx.pipe = pipe;
    return ctx;
}

OpCtx kind_op(IoKind kind, SourceId src, PipeId pipe) {
    OpCtx ctx{};
    ctx.kind = kind;
    ctx.source = src;
    ctx.pipe = pipe;
    return ctx;
}

}  // namespace

TEST_CASE("translate：负 res → kTypeError，内核错误就地拦截", "[01]") {
    struct Fx {
        OpCtx ctx{};
        disp::Cqe c{};
        std::optional<Event> ev;
    };

    auto build_negative_res_read_cqe = [] {
        auto fx = std::make_unique<Fx>();
        fx->ctx = read_op();
        fx->c = disp::Cqe{.res = -5, .user_data = 0x1234};
        return fx;
    };
    auto run_translate_negative_res = [](Fx& fx) {
        fx.ev = disp::translate(fx.c, fx.ctx, 1000);
    };
    auto expect_type_error_intercepted_in_place = [](const Fx& fx) {
        REQUIRE(fx.ev.has_value());
        REQUIRE(fx.ev->type == EventType(kTypeError));
        REQUIRE(fx.ev->source == 7);
        REQUIRE(fx.ev->pipe == 42);
        REQUIRE(fx.ev->ts_ns == 1000);
        REQUIRE(fx.ev->def == &kTypeError);
        auto r = std::any_cast<IoResult>(fx.ev->payload);
        REQUIRE(r.err == 5);
        REQUIRE(r.bytes == 0);
    };

    auto fx = build_negative_res_read_cqe();
    run_translate_negative_res(*fx);
    expect_type_error_intercepted_in_place(*fx);
}

TEST_CASE("translate：内部唤醒只叫醒循环，不产生事件", "[01]") {
    struct Fx {
        OpCtx ctx{};
        disp::Cqe c{};
        std::optional<Event> ev;
    };

    auto build_internal_wake_cqe = [] {
        auto fx = std::make_unique<Fx>();
        fx->ctx = kind_op(IoKind::kEventFdWake, kInternalWakeSource, 0);
        fx->c = disp::Cqe{.res = 8, .user_data = 0};
        return fx;
    };
    auto run_translate_internal_wake = [](Fx& fx) {
        fx.ev = disp::translate(fx.c, fx.ctx, 1000);
    };
    auto expect_no_event_from_internal_wake = [](const Fx& fx) {
        REQUIRE_FALSE(fx.ev.has_value());
    };

    auto fx = build_internal_wake_cqe();
    run_translate_internal_wake(*fx);
    expect_no_event_from_internal_wake(*fx);
}

TEST_CASE("translate：对端唤醒 → kTypeWake，回执地址照抄 OpCtx", "[01]") {
    struct Fx {
        OpCtx ctx{};
        disp::Cqe c{};
        std::optional<Event> ev;
    };

    auto build_peer_wake_cqe = [] {
        auto fx = std::make_unique<Fx>();
        fx->ctx = kind_op(IoKind::kEventFdWake, 9, 77);
        fx->c = disp::Cqe{.res = 8, .user_data = 0};
        return fx;
    };
    auto run_translate_peer_wake = [](Fx& fx) {
        fx.ev = disp::translate(fx.c, fx.ctx, 1000);
    };
    auto expect_wake_event_copies_op_address = [](const Fx& fx) {
        REQUIRE(fx.ev.has_value());
        REQUIRE(fx.ev->type == EventType(kTypeWake));
        REQUIRE(fx.ev->source == 9);
        REQUIRE(fx.ev->pipe == 77);
        REQUIRE(fx.ev->def == &kTypeWake);
    };

    auto fx = build_peer_wake_cqe();
    run_translate_peer_wake(*fx);
    expect_wake_event_copies_op_address(*fx);
}

TEST_CASE("translate：timerfd 到期 → kTypeTimeout，过期计数透传不解释", "[01]") {
    struct Fx {
        uint64_t expirations = 0;
        OpCtx ctx{};
        disp::Cqe c{};
        std::optional<Event> ev;
    };

    auto build_timerfd_with_expirations_buffer = [] {
        auto fx = std::make_unique<Fx>();
        fx->expirations = 3;
        fx->ctx = kind_op(IoKind::kTimerWait, 0, 5);
        fx->ctx.buf = &fx->expirations;
        fx->ctx.len = sizeof(fx->expirations);
        fx->c = disp::Cqe{.res = 8, .user_data = 0};
        return fx;
    };
    auto run_translate_timerfd_expirations = [](Fx& fx) {
        fx.ev = disp::translate(fx.c, fx.ctx, 2000);
    };
    auto expect_expirations_forwarded_verbatim = [](const Fx& fx) {
        REQUIRE(fx.ev.has_value());
        REQUIRE(fx.ev->type == EventType(kTypeTimeout));
        REQUIRE(fx.ev->pipe == 5);
        REQUIRE(fx.ev->ts_ns == 2000);
        REQUIRE(std::any_cast<uint64_t>(fx.ev->payload) == 3);
        REQUIRE(fx.ev->def == &kTypeTimeout);
    };

    auto fx = build_timerfd_with_expirations_buffer();
    run_translate_timerfd_expirations(*fx);
    expect_expirations_forwarded_verbatim(*fx);
}

TEST_CASE("translate：timerfd 缓冲不可用退化，过期计数 = res 本身", "[01]") {
    struct Fx {
        uint64_t expirations = 0;
        OpCtx ctx_no_buf{};
        OpCtx ctx_with_buf{};
        disp::Cqe c_buf_empty{};
        disp::Cqe c_res_ne_8{};
        std::optional<Event> ev_buf_empty;
        std::optional<Event> ev_res_ne_8;
    };

    // 退化路：buf 为空、或 res ≠ 8（缓冲不可信）时不动缓冲，透传 res 当过期计数
    auto build_timerfd_without_trustworthy_buffer = [] {
        auto fx = std::make_unique<Fx>();
        fx->expirations = 3;
        fx->ctx_no_buf = kind_op(IoKind::kTimerWait, 0, 5);
        fx->ctx_with_buf = kind_op(IoKind::kTimerWait, 0, 5);
        fx->ctx_with_buf.buf = &fx->expirations;
        fx->ctx_with_buf.len = sizeof(fx->expirations);
        fx->c_buf_empty = disp::Cqe{.res = 8, .user_data = 0};
        fx->c_res_ne_8 = disp::Cqe{.res = 4, .user_data = 0};
        return fx;
    };
    auto run_translate_both_degrade_cases = [](Fx& fx) {
        fx.ev_buf_empty = disp::translate(fx.c_buf_empty, fx.ctx_no_buf, 1000);
        fx.ev_res_ne_8 = disp::translate(fx.c_res_ne_8, fx.ctx_with_buf, 1000);
    };
    auto expect_expirations_taken_from_res = [](const Fx& fx) {
        REQUIRE(fx.ev_buf_empty.has_value());
        REQUIRE(fx.ev_buf_empty->type == EventType(kTypeTimeout));
        REQUIRE(std::any_cast<uint64_t>(fx.ev_buf_empty->payload) == 8u);   // buf 空：取 res
        REQUIRE(fx.ev_res_ne_8.has_value());
        REQUIRE(std::any_cast<uint64_t>(fx.ev_res_ne_8->payload) == 4u);   // res ≠ 8：buf 不读，仍取 res
    };

    auto fx = build_timerfd_without_trustworthy_buffer();
    run_translate_both_degrade_cases(*fx);
    expect_expirations_taken_from_res(*fx);
}

TEST_CASE("translate：读写完成 → IoReadDone/IoWriteDone，字节数带出", "[01]") {
    struct Fx {
        disp::Cqe c{};
        OpCtx rd_op{};
        OpCtx wr_op{};
        OpCtx dv_op{};
        std::optional<Event> rd_ev;
        std::optional<Event> wr_ev;
        std::optional<Event> dv_ev;
    };

    auto build_three_ops_on_one_cqe = [] {
        auto fx = std::make_unique<Fx>();
        fx->c = disp::Cqe{.res = 64, .user_data = 0};
        fx->rd_op = read_op();
        fx->wr_op = kind_op(IoKind::kSocketWrite, 3, 4);
        fx->dv_op = kind_op(IoKind::kDeviceIo, 8, 9);
        return fx;
    };
    auto run_translate_read_write_and_device = [](Fx& fx) {
        fx.rd_ev = disp::translate(fx.c, fx.rd_op, 1000);
        fx.wr_ev = disp::translate(fx.c, fx.wr_op, 1000);
        fx.dv_ev = disp::translate(fx.c, fx.dv_op, 1000);
    };
    auto expect_read_write_device_done_events = [](const Fx& fx) {
        REQUIRE(fx.rd_ev->type == EventType(kTypeIoReadDone));
        REQUIRE(std::any_cast<IoResult>(fx.rd_ev->payload).bytes == 64);
        REQUIRE(fx.rd_ev->def == &kTypeIoReadDone);

        REQUIRE(fx.wr_ev->type == EventType(kTypeIoWriteDone));
        REQUIRE(fx.wr_ev->source == 3);
        REQUIRE(fx.wr_ev->def == &kTypeIoWriteDone);

        REQUIRE(fx.dv_ev->type == EventType(kTypeIoReadDone));
    };

    auto fx = build_three_ops_on_one_cqe();
    run_translate_read_write_and_device(*fx);
    expect_read_write_device_done_events(*fx);
}

TEST_CASE("should_drop：kBlock 永不丢", "[01]") {
    struct Fx {
        Event ev{};
        bool drop_at_zero = false;
        bool drop_far_above_water = false;
    };

    auto build_block_qos_event = [] {
        auto fx = std::make_unique<Fx>();
        fx->ev.def = &kTypeIoReadDone;   // Qos::kBlock
        return fx;
    };
    auto run_should_drop_block_at_two_levels = [](Fx& fx) {
        fx.drop_at_zero = disp::should_drop(fx.ev, 0);
        fx.drop_far_above_water = disp::should_drop(fx.ev, disp::kQueueHighWater * 100);
    };
    auto expect_block_qos_never_drops = [](const Fx& fx) {
        REQUIRE_FALSE(fx.drop_at_zero);
        REQUIRE_FALSE(fx.drop_far_above_water);
    };

    auto fx = build_block_qos_event();
    run_should_drop_block_at_two_levels(*fx);
    expect_block_qos_never_drops(*fx);
}

TEST_CASE("should_drop：kDropCount 满水位丢新", "[01]") {
    struct Fx {
        EventDef row{1002, kBiz, Qos::kDropCount};   // 行形本地造，只验 Qos 判定
        Event ev{};
        bool drop_below = false;
        bool drop_at = false;
        bool drop_above = false;
    };

    auto build_drop_count_qos_event = [] {
        auto fx = std::make_unique<Fx>();
        fx->ev.def = &fx->row;
        return fx;
    };
    auto run_should_drop_count_across_water_levels = [](Fx& fx) {
        fx.drop_below = disp::should_drop(fx.ev, disp::kQueueHighWater - 1);
        fx.drop_at = disp::should_drop(fx.ev, disp::kQueueHighWater);
        fx.drop_above = disp::should_drop(fx.ev, disp::kQueueHighWater + 1);
    };
    auto expect_drop_count_drops_at_high_water = [](const Fx& fx) {
        REQUIRE_FALSE(fx.drop_below);
        REQUIRE(fx.drop_at);
        REQUIRE(fx.drop_above);
    };

    auto fx = build_drop_count_qos_event();
    run_should_drop_count_across_water_levels(*fx);
    expect_drop_count_drops_at_high_water(*fx);
}

TEST_CASE("should_drop：kKeepLatest 退化形同 kDropCount（丢旧留新启用前）", "[01]") {
    struct Fx {
        EventDef row{1003, kBiz, Qos::kKeepLatest};   // 行形本地造（当前无行使用，钉退化形）
        Event ev{};
        bool drop_below = false;
        bool drop_at = false;
    };

    auto build_keep_latest_qos_event = [] {
        auto fx = std::make_unique<Fx>();
        fx->ev.def = &fx->row;
        return fx;
    };
    auto run_should_drop_keep_latest_at_two_levels = [](Fx& fx) {
        fx.drop_below = disp::should_drop(fx.ev, disp::kQueueHighWater - 1);
        fx.drop_at = disp::should_drop(fx.ev, disp::kQueueHighWater);
    };
    auto expect_keep_latest_degrades_to_drop_count = [](const Fx& fx) {
        REQUIRE_FALSE(fx.drop_below);
        REQUIRE(fx.drop_at);
    };

    auto fx = build_keep_latest_qos_event();
    run_should_drop_keep_latest_at_two_levels(*fx);
    expect_keep_latest_degrades_to_drop_count(*fx);
}

TEST_CASE("should_drop：def 空按 kBlock", "[01]") {
    struct Fx {
        Event ev{};
        bool dropped = false;
    };

    auto build_event_without_def = [] {
        return std::make_unique<Fx>();
    };
    auto run_should_drop_on_empty_def = [](Fx& fx) {
        fx.dropped = disp::should_drop(fx.ev, disp::kQueueHighWater * 2);
    };
    auto expect_empty_def_falls_back_to_block = [](const Fx& fx) {
        REQUIRE_FALSE(fx.dropped);
    };

    auto fx = build_event_without_def();
    run_should_drop_on_empty_def(*fx);
    expect_empty_def_falls_back_to_block(*fx);
}
