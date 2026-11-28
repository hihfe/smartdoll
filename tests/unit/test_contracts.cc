// policy.h / media.h 契约单测（P0）：编译期审查套件的运行面 + 契约常量。
// 全部用例为 Arrange-Act-Assert 三段式，每段一个具名函数（规范见 doc/testing.md）。
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>

#include "media.h"
#include "policy.h"

namespace {

// 被审 constexpr 表：static_assert 与 Act 两段都要同域可见、且不靠捕获。
// 夹具 Fx 是局部类，不允许有 static 数据成员（[class.local]：A local class cannot have static data members）；
// 用例作用域的 constexpr 局部变量被按 const& 实参传给审查谓词时是 odr-使用（[basic.def.odr]/5，
// 绑定引用不属"立即 lvalue-to-rvalue"），非捕获 lambda 引用不到、捕获后又进不了常量表达式——
// 故按 doc/testing.md 规则 8 的退路，表置于本文件匿名命名空间（仍是编译期常量，三段同域可见、无需捕获）。
constexpr std::array<EventDef, 3> kOkRows{
    EventDef{1, kDisp, Qos::kBlock},
    EventDef{101, kIpc, Qos::kBlock},
    EventDef{1050, kBiz, Qos::kDropCount},
};
constexpr std::array<EventDef, 3> kDupRows{
    EventDef{1, kDisp, Qos::kBlock},
    EventDef{1, kIpc, Qos::kBlock},
    EventDef{2, kDisp, Qos::kBlock},
};
constexpr std::array<EventDef, 2> kOffSegmentRows{
    EventDef{1, kDisp, Qos::kBlock},
    EventDef{250, kTmr, Qos::kBlock},
};
constexpr std::array<OpPolicy, kOpKindCount> kAllOpRows{
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
constexpr std::array<OpPolicy, kOpKindCount - 1> kMissingOpRows{
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

}  // namespace

TEST_CASE("policy：号段互不重叠", "[policy]") {
    struct Fx {
        bool disjoint = false;   // 初值 false：Act 没跑到就过不了断言
    };

    auto build_segment_table_case = [] {
        return std::make_unique<Fx>();
    };
    auto run_segments_disjoint_check = [](Fx& fx) {
        fx.disjoint = segments_disjoint();
    };
    auto expect_disjoint_at_compile_and_runtime = [](const Fx& fx) {
        static_assert(segments_disjoint());
        REQUIRE(fx.disjoint);
    };

    auto fx = build_segment_table_case();
    run_segments_disjoint_check(*fx);
    expect_disjoint_at_compile_and_runtime(*fx);
}

TEST_CASE("policy：行不越段", "[policy]") {
    struct Fx {
        bool row_581_in_segment = false;   // 初值 false：Act 没跑到就过不了断言
    };

    auto build_row_segment_case = [] {
        return std::make_unique<Fx>();
    };
    auto run_row_581_segment_check = [](Fx& fx) {
        fx.row_581_in_segment = row_in_segment({581, kAi, Qos::kBlock});
    };
    auto expect_in_and_out_of_segment_rows = [](const Fx& fx) {
        static_assert(row_in_segment({1, kDisp, Qos::kBlock}));
        static_assert(row_in_segment({150, kIpc, Qos::kBlock}));
        static_assert(row_in_segment({500, kAi, Qos::kBlock}));
        static_assert(row_in_segment({1050, kBiz, Qos::kBlock}));
        static_assert(!row_in_segment({150, kNet, Qos::kBlock}));
        static_assert(!row_in_segment({99, kIpc, Qos::kBlock}));
        static_assert(!row_in_segment({5, kBiz, Qos::kBlock}));
        REQUIRE(fx.row_581_in_segment);
    };

    auto fx = build_row_segment_case();
    run_row_581_segment_check(*fx);
    expect_in_and_out_of_segment_rows(*fx);
}

TEST_CASE("policy：id 全局唯一 + 行表整组审查", "[policy]") {
    struct Fx {
        bool ok_rows_unique = false;         // 初值 false：Act 没跑到就过不了断言
        bool ok_rows_in_segments = false;    // 初值 false：同上
    };

    auto build_row_table_case = [] {
        return std::make_unique<Fx>();
    };
    auto run_ok_rows_audit = [](Fx& fx) {
        fx.ok_rows_unique = rows_unique(kOkRows);
        fx.ok_rows_in_segments = rows_in_segments(kOkRows);
    };
    auto expect_ok_rows_unique_and_in_segment = [](const Fx& fx) {
        static_assert(rows_unique(kOkRows));
        static_assert(!rows_unique(kDupRows));
        static_assert(rows_in_segments(kOkRows));
        static_assert(!rows_in_segments(kOffSegmentRows));
        REQUIRE(fx.ok_rows_unique);
        REQUIRE(fx.ok_rows_in_segments);
    };

    auto fx = build_row_table_case();
    run_ok_rows_audit(*fx);
    expect_ok_rows_unique_and_in_segment(*fx);
}

TEST_CASE("policy：每类在飞单恰一行 OpPolicy", "[policy]") {
    struct Fx {
        bool all_op_rows_complete = false;   // 初值 false：Act 没跑到就过不了断言
    };

    auto build_op_policy_table_case = [] {
        return std::make_unique<Fx>();
    };
    auto run_op_rows_completeness_check = [](Fx& fx) {
        fx.all_op_rows_complete = op_rows_complete(kAllOpRows);
    };
    auto expect_every_op_kind_has_exactly_one_row = [](const Fx& fx) {
        static_assert(op_rows_complete(kAllOpRows));
        static_assert(!op_rows_complete(kMissingOpRows));
        REQUIRE(fx.all_op_rows_complete);
    };

    auto fx = build_op_policy_table_case();
    run_op_rows_completeness_check(*fx);
    expect_every_op_kind_has_exactly_one_row(*fx);
}

TEST_CASE("policy：EventDef 用作 EventType（声明即行）", "[policy]") {
    struct Fx {
        EventDef row{};
        EventType as_type = 0;   // 行按 EventType 取值的结果
    };

    auto build_kipc_row = [] {
        auto fx = std::make_unique<Fx>();
        fx->row = EventDef{7, kIpc, Qos::kBlock};
        return fx;
    };
    auto run_row_to_event_type_conversion = [](Fx& fx) {
        fx.as_type = fx.row;
    };
    auto expect_conversion_yields_row_id = [](const Fx& fx) {
        REQUIRE(fx.as_type == 7u);
        REQUIRE(fx.row.owner == kIpc);
    };

    auto fx = build_kipc_row();
    run_row_to_event_type_conversion(*fx);
    expect_conversion_yields_row_id(*fx);
}

TEST_CASE("media：契约常量与结构", "[media]") {
    struct Fx {
        AudioFormat fmt{};
        VoiceMeta meta{};
        // 契约常量与结构字段的读取结果（初值 0：Act 没跑到就过不了断言）
        uint32_t kind_voice_pcm = 0;
        uint8_t  qos_critical = 0;
        uint8_t  qos_realtime = 0;
        uint32_t sample_rate = 0;
        uint32_t period_frames = 0;
        uint32_t sequence = 0;
        uint8_t  is_final = 0;
    };

    auto build_format_and_meta = [] {
        auto fx = std::make_unique<Fx>();
        fx->fmt = AudioFormat{16000, 1, 16, 320};
        fx->meta = VoiceMeta{16000, 1, 16, 0, 1};
        return fx;
    };
    auto run_contract_constant_reads = [](Fx& fx) {
        fx.kind_voice_pcm = kKindVoicePcm;
        fx.qos_critical = kQosCritical;
        fx.qos_realtime = kQosRealtime;
        fx.sample_rate = fx.fmt.sample_rate;
        fx.period_frames = fx.fmt.period_frames;
        fx.sequence = fx.meta.sequence;
        fx.is_final = fx.meta.is_final;
    };
    auto expect_contract_values_hold = [](const Fx& fx) {
        REQUIRE(fx.kind_voice_pcm == 1u);
        REQUIRE(fx.qos_critical == 1);
        REQUIRE(fx.qos_realtime == 2);
        REQUIRE(fx.sample_rate == 16000);
        REQUIRE(fx.period_frames == 320u);
        REQUIRE(fx.sequence == 0u);
        REQUIRE(fx.is_final == 1);
    };

    auto fx = build_format_and_meta();
    run_contract_constant_reads(*fx);
    expect_contract_values_hold(*fx);
}
