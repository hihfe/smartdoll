// policy.h / media.h 契约单测（P0）：编译期审查套件的运行面 + 契约常量。
#include <catch2/catch_test_macros.hpp>

#include <array>

#include "media.h"
#include "policy.h"

TEST_CASE("policy：号段互不重叠", "[policy]") {
    static_assert(segments_disjoint());
    REQUIRE(segments_disjoint());
}

TEST_CASE("policy：行不越段", "[policy]") {
    static_assert(row_in_segment({1, kDisp, Qos::kBlock}));
    static_assert(row_in_segment({150, kIpc, Qos::kBlock}));
    static_assert(row_in_segment({500, kAi, Qos::kBlock}));
    static_assert(row_in_segment({1050, kBiz, Qos::kBlock}));
    static_assert(!row_in_segment({150, kNet, Qos::kBlock}));
    static_assert(!row_in_segment({99, kIpc, Qos::kBlock}));
    static_assert(!row_in_segment({5, kBiz, Qos::kBlock}));
    REQUIRE(row_in_segment({581, kAi, Qos::kBlock}));
}

TEST_CASE("policy：id 全局唯一 + 行表整组审查", "[policy]") {
    constexpr std::array<EventDef, 3> ok{
        EventDef{1, kDisp, Qos::kBlock},
        EventDef{101, kIpc, Qos::kBlock},
        EventDef{1050, kBiz, Qos::kDropCount},
    };
    constexpr std::array<EventDef, 3> dup{
        EventDef{1, kDisp, Qos::kBlock},
        EventDef{1, kIpc, Qos::kBlock},
        EventDef{2, kDisp, Qos::kBlock},
    };
    constexpr std::array<EventDef, 2> off_segment{
        EventDef{1, kDisp, Qos::kBlock},
        EventDef{250, kTmr, Qos::kBlock},
    };

    static_assert(rows_unique(ok));
    static_assert(!rows_unique(dup));
    static_assert(rows_in_segments(ok));
    static_assert(!rows_in_segments(off_segment));
    REQUIRE(rows_unique(ok));
    REQUIRE(rows_in_segments(ok));
}

TEST_CASE("policy：每类在飞单恰一行 OpPolicy", "[policy]") {
    constexpr std::array<OpPolicy, kOpKindCount> all{
        OpPolicy{OpKind::kIpcRequest, TimeoutSrc::kCaller, 0,
                 OpTimeoutAct::kSynthReply, OpLateAct::kDropCount},
        OpPolicy{OpKind::kTxIo, TimeoutSrc::kSpec, 500'000'000,
                 OpTimeoutAct::kKillDevice, OpLateAct::kDropCount},
        OpPolicy{OpKind::kEventIo, TimeoutSrc::kNone, 0,
                 OpTimeoutAct::kNone, OpLateAct::kDropCount},
        OpPolicy{OpKind::kNetConnect, TimeoutSrc::kCaller, 0,
                 OpTimeoutAct::kBackoffNext, OpLateAct::kDropCount},
        OpPolicy{OpKind::kLlmRequest, TimeoutSrc::kCaller, 0,
                 OpTimeoutAct::kCloseCase, OpLateAct::kDropCount},
        OpPolicy{OpKind::kAsrTts, TimeoutSrc::kNone, 0,
                 OpTimeoutAct::kNone, OpLateAct::kDropCount},
        OpPolicy{OpKind::kTimerfdRead, TimeoutSrc::kNone, 0,
                 OpTimeoutAct::kSelfHeal, OpLateAct::kNone},
        OpPolicy{OpKind::kUringIo, TimeoutSrc::kNone, 0,
                 OpTimeoutAct::kNone, OpLateAct::kDropCount},
    };
    constexpr std::array<OpPolicy, kOpKindCount - 1> missing{
        OpPolicy{OpKind::kIpcRequest, TimeoutSrc::kCaller, 0,
                 OpTimeoutAct::kSynthReply, OpLateAct::kDropCount},
        OpPolicy{OpKind::kTxIo, TimeoutSrc::kSpec, 500'000'000,
                 OpTimeoutAct::kKillDevice, OpLateAct::kDropCount},
        OpPolicy{OpKind::kEventIo, TimeoutSrc::kNone, 0,
                 OpTimeoutAct::kNone, OpLateAct::kDropCount},
        OpPolicy{OpKind::kNetConnect, TimeoutSrc::kCaller, 0,
                 OpTimeoutAct::kBackoffNext, OpLateAct::kDropCount},
        OpPolicy{OpKind::kLlmRequest, TimeoutSrc::kCaller, 0,
                 OpTimeoutAct::kCloseCase, OpLateAct::kDropCount},
        OpPolicy{OpKind::kAsrTts, TimeoutSrc::kNone, 0,
                 OpTimeoutAct::kNone, OpLateAct::kDropCount},
        OpPolicy{OpKind::kTimerfdRead, TimeoutSrc::kNone, 0,
                 OpTimeoutAct::kSelfHeal, OpLateAct::kNone},
    };

    static_assert(op_rows_complete(all));
    static_assert(!op_rows_complete(missing));
    REQUIRE(op_rows_complete(all));
}

TEST_CASE("policy：EventDef 用作 EventType（声明即行）", "[policy]") {
    constexpr EventDef row{7, kIpc, Qos::kBlock};
    EventType t = row;
    REQUIRE(t == 7u);
    REQUIRE(row.owner == kIpc);
}

TEST_CASE("media：契约常量与结构", "[media]") {
    REQUIRE(kKindVoicePcm == 1u);
    REQUIRE(kQosCritical == 1);
    REQUIRE(kQosRealtime == 2);

    constexpr AudioFormat f{16000, 1, 16, 320};
    REQUIRE(f.sample_rate == 16000);
    REQUIRE(f.period_frames == 320u);

    constexpr VoiceMeta m{16000, 1, 16, 0, 1};
    REQUIRE(m.sequence == 0u);
    REQUIRE(m.is_final == 1);
}
