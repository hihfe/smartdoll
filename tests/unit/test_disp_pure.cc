// 01 纯函数单测（白盒：disp_internal.h）：translate 翻译定形 + 错误拦截、should_drop 背压判定。
#include <catch2/catch_test_macros.hpp>

#include <any>

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
    auto ctx = read_op();
    disp::Cqe c{.res = -5, .user_data = 0x1234};

    auto ev = disp::translate(c, ctx, 1000);

    REQUIRE(ev.has_value());
    REQUIRE(ev->type == EventType(kTypeError));
    REQUIRE(ev->source == 7);
    REQUIRE(ev->pipe == 42);
    REQUIRE(ev->ts_ns == 1000);
    REQUIRE(ev->def == &kTypeError);
    auto r = std::any_cast<IoResult>(ev->payload);
    REQUIRE(r.err == 5);
    REQUIRE(r.bytes == 0);
}

TEST_CASE("translate：内部唤醒只叫醒循环，不产生事件", "[01]") {
    auto ctx = kind_op(IoKind::kEventFdWake, kInternalWakeSource, 0);
    disp::Cqe c{.res = 8, .user_data = 0};

    auto ev = disp::translate(c, ctx, 1000);

    REQUIRE_FALSE(ev.has_value());
}

TEST_CASE("translate：对端唤醒 → kTypeWake，回执地址照抄 OpCtx", "[01]") {
    auto ctx = kind_op(IoKind::kEventFdWake, 9, 77);
    disp::Cqe c{.res = 8, .user_data = 0};

    auto ev = disp::translate(c, ctx, 1000);

    REQUIRE(ev.has_value());
    REQUIRE(ev->type == EventType(kTypeWake));
    REQUIRE(ev->source == 9);
    REQUIRE(ev->pipe == 77);
    REQUIRE(ev->def == &kTypeWake);
}

TEST_CASE("translate：timerfd 到期 → kTypeTimeout，过期计数透传不解释", "[01]") {
    uint64_t expirations = 3;   // timerfd 读回的过期计数在缓冲区（res = 字节数）
    auto ctx = kind_op(IoKind::kTimerWait, 0, 5);
    ctx.buf = &expirations;
    ctx.len = sizeof(expirations);
    disp::Cqe c{.res = 8, .user_data = 0};

    auto ev = disp::translate(c, ctx, 2000);

    REQUIRE(ev.has_value());
    REQUIRE(ev->type == EventType(kTypeTimeout));
    REQUIRE(ev->pipe == 5);
    REQUIRE(ev->ts_ns == 2000);
    REQUIRE(std::any_cast<uint64_t>(ev->payload) == 3);
    REQUIRE(ev->def == &kTypeTimeout);
}

TEST_CASE("translate：读写完成 → IoReadDone/IoWriteDone，字节数带出", "[01]") {
    disp::Cqe c{.res = 64, .user_data = 0};

    auto rd = disp::translate(c, read_op(), 1000);
    REQUIRE(rd->type == EventType(kTypeIoReadDone));
    REQUIRE(std::any_cast<IoResult>(rd->payload).bytes == 64);
    REQUIRE(rd->def == &kTypeIoReadDone);

    auto wr = disp::translate(c, kind_op(IoKind::kSocketWrite, 3, 4), 1000);
    REQUIRE(wr->type == EventType(kTypeIoWriteDone));
    REQUIRE(wr->source == 3);
    REQUIRE(wr->def == &kTypeIoWriteDone);

    auto dv = disp::translate(c, kind_op(IoKind::kDeviceIo, 8, 9), 1000);
    REQUIRE(dv->type == EventType(kTypeIoReadDone));
}

TEST_CASE("should_drop：kBlock 永不丢", "[01]") {
    Event ev{};
    ev.def = &kTypeIoReadDone;   // Qos::kBlock
    REQUIRE_FALSE(disp::should_drop(ev, 0));
    REQUIRE_FALSE(disp::should_drop(ev, disp::kQueueHighWater * 100));
}

TEST_CASE("should_drop：kDropCount 满水位丢新", "[01]") {
    constexpr EventDef kDropRow{1002, kBiz, Qos::kDropCount};   // 行形本地造，只验 Qos 判定
    Event ev{};
    ev.def = &kDropRow;
    REQUIRE_FALSE(disp::should_drop(ev, disp::kQueueHighWater - 1));
    REQUIRE(disp::should_drop(ev, disp::kQueueHighWater));
    REQUIRE(disp::should_drop(ev, disp::kQueueHighWater + 1));
}

TEST_CASE("should_drop：def 空按 kBlock", "[01]") {
    Event ev{};
    REQUIRE_FALSE(disp::should_drop(ev, disp::kQueueHighWater * 2));
}
