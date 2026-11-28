// with.h 切面语义单测（P0）：
// enter 外→内、exit 内→外且恒恰好一次、拒绝路、返回值透传、本体实参、分流形、live；
// enter/exit 可空；ticket/settle 账本协议语义（假账本，无超时形）。
// 全部用例为 Arrange-Act-Assert 三段式，每段一个具名函数（规范见 doc/testing.md）。
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "disp.h"
#include "with.h"

namespace {

// 记录切面动作的机械探针（业务数据不进切面 state 的规则用它反着验）
// 指针指向夹具成员，夹具堆上构造后不移动（doc/testing.md 规则 6）
struct Probe {
    std::vector<std::string>* log = nullptr;
    const char* name = "?";
    Outcome enter_result = Outcome::kEnter;
    int* exit_count = nullptr;
    bool throw_on_exit = false;
};

Outcome probe_enter(void* s) {
    auto* p = static_cast<Probe*>(s);
    p->log->push_back(std::string("enter:") + p->name);
    return p->enter_result;
}

void probe_exit(void* s) {
    auto* p = static_cast<Probe*>(s);
    p->log->push_back(std::string("exit:") + p->name);
    if (p->exit_count) ++*p->exit_count;
    if (p->throw_on_exit) throw std::runtime_error("exit boom");
}

Aspect probe(Probe& p, const char* name) {
    p.name = name;
    return Aspect{&p, probe_enter, probe_exit, name};
}

Outcome always_enter(void*) { return Outcome::kEnter; }

struct Subject { int x; };

}  // namespace

TEST_CASE("with：enter 外→内、本体、exit 内→外", "[with]") {
    struct Fx {
        std::vector<std::string> log;
        Probe p1{&log}, p2{&log}, p3{&log};
    };

    auto build_three_probe_chain = [] {
        return std::make_unique<Fx>();
    };
    auto run_body_behind_three_probes = [](Fx& fx) {
        with(probe(fx.p1, "a"), probe(fx.p2, "b"), probe(fx.p3, "c"),
             [&fx] { fx.log.push_back("body"); });
    };
    auto expect_outer_to_inner_order = [](const Fx& fx) {
        REQUIRE(fx.log == std::vector<std::string>{
            "enter:a", "enter:b", "enter:c", "body", "exit:c", "exit:b", "exit:a"});
    };

    auto fx = build_three_probe_chain();
    run_body_behind_three_probes(*fx);
    expect_outer_to_inner_order(*fx);
}

TEST_CASE("with：拒绝——后续切面与本体不进，已进切面照常 exit", "[with]") {
    struct Fx {
        std::vector<std::string> log;
        Probe p1{&log}, p2{&log}, p3{&log};
        bool body_ran = false;
    };

    auto build_chain_with_b_rejecting = [] {
        auto fx = std::make_unique<Fx>();
        fx->p2.enter_result = Outcome::kReject;
        return fx;
    };
    auto run_with_rejecting_b = [](Fx& fx) {
        with(probe(fx.p1, "a"), probe(fx.p2, "b"), probe(fx.p3, "c"),
             [&fx] { fx.body_ran = true; });
    };
    auto expect_body_skipped_and_a_exited = [](const Fx& fx) {
        REQUIRE_FALSE(fx.body_ran);
        REQUIRE(fx.log == std::vector<std::string>{"enter:a", "enter:b", "exit:a"});
    };

    auto fx = build_chain_with_b_rejecting();
    run_with_rejecting_b(*fx);
    expect_body_skipped_and_a_exited(*fx);
}

TEST_CASE("with：本体返回值原样透传", "[with]") {
    struct Fx {
        std::vector<std::string> log;
        Probe p1{&log};
        int got = 0;
    };

    auto build_single_probe_around_body = [] {
        return std::make_unique<Fx>();
    };
    auto run_returning_body = [](Fx& fx) {
        fx.got = with(probe(fx.p1, "a"), [] { return 42; });
    };
    auto expect_value_passed_through = [](const Fx& fx) {
        REQUIRE(fx.got == 42);
    };

    auto fx = build_single_probe_around_body();
    run_returning_body(*fx);
    expect_value_passed_through(*fx);
}

TEST_CASE("with：void 本体照常收尾", "[with]") {
    struct Fx {
        std::vector<std::string> log;
        Probe p1{&log};
        bool ran = false;
    };

    auto build_single_probe_around_void_body = [] {
        return std::make_unique<Fx>();
    };
    auto run_void_body = [](Fx& fx) {
        with(probe(fx.p1, "a"), [&fx] { fx.ran = true; });
    };
    auto expect_void_body_closed_normally = [](const Fx& fx) {
        REQUIRE(fx.ran);
        REQUIRE(fx.log == std::vector<std::string>{"enter:a", "exit:a"});
    };

    auto fx = build_single_probe_around_void_body();
    run_void_body(*fx);
    expect_void_body_closed_normally(*fx);
}

TEST_CASE("with：拒绝返回值初始化", "[with]") {
    struct Fx {
        std::vector<std::string> log;
        Probe p1{&log};
        int got_int = 0;
        bool got_bool = true;   // 初值取 true：act 没跑到就过不了断言
    };

    auto build_rejecting_probe = [] {
        auto fx = std::make_unique<Fx>();
        fx->p1.enter_result = Outcome::kReject;
        return fx;
    };
    auto run_two_rejected_bodies = [](Fx& fx) {
        fx.got_int = with(probe(fx.p1, "a"), [] { return 42; });
        fx.got_bool = with(probe(fx.p1, "a"), [] { return true; });
    };
    auto expect_zero_value_returns = [](const Fx& fx) {
        REQUIRE(fx.got_int == 0);
        REQUIRE_FALSE(fx.got_bool);
    };

    auto fx = build_rejecting_probe();
    run_two_rejected_bodies(*fx);
    expect_zero_value_returns(*fx);
}

TEST_CASE("with：零切面直接跑本体", "[with]") {
    struct Fx {
        int got = 0;
    };

    auto build_no_aspect_case = [] {
        return std::make_unique<Fx>();
    };
    auto run_body_without_aspects = [](Fx& fx) {
        fx.got = with([] { return 7; });
    };
    auto expect_body_runs_directly = [](const Fx& fx) {
        REQUIRE(fx.got == 7);
    };

    auto fx = build_no_aspect_case();
    run_body_without_aspects(*fx);
    expect_body_runs_directly(*fx);
}

TEST_CASE("with：本体抛异常也走 exit（恰好一次）", "[with]") {
    struct Fx {
        std::vector<std::string> log;
        int exits = 0;
        Probe p1{&log, "a", Outcome::kEnter, &exits};
        bool threw = false;
    };

    auto build_probe_counting_exits = [] {
        return std::make_unique<Fx>();
    };
    auto run_body_that_throws = [](Fx& fx) {
        try {
            with(probe(fx.p1, "a"), [] { throw std::runtime_error("body"); });
        } catch (...) {
            fx.threw = true;
        }
    };
    auto expect_exit_happened_once_on_throw = [](const Fx& fx) {
        REQUIRE(fx.threw);
        REQUIRE(fx.exits == 1);
        REQUIRE(fx.log.back() == "exit:a");
    };

    auto fx = build_probe_counting_exits();
    run_body_that_throws(*fx);
    expect_exit_happened_once_on_throw(*fx);
}

TEST_CASE("with：内层 exit 抛异常，外层 exit 照常", "[with]") {
    struct Fx {
        std::vector<std::string> log;
        int exits_a = 0, exits_b = 0;
        Probe p1{&log, "a", Outcome::kEnter, &exits_a};
        Probe p2{&log, "b", Outcome::kEnter, &exits_b, /*throw_on_exit=*/true};
        bool threw = false;
    };

    auto build_chain_with_booming_inner_exit = [] {
        return std::make_unique<Fx>();
    };
    auto run_chain_with_booming_inner_exit = [](Fx& fx) {
        try {
            with(probe(fx.p1, "a"), probe(fx.p2, "b"), [] {});
        } catch (...) {
            fx.threw = true;
        }
    };
    auto expect_outer_exit_survives_inner_throw = [](const Fx& fx) {
        REQUIRE(fx.threw);
        REQUIRE(fx.exits_a == 1);
        REQUIRE(fx.exits_b == 1);
        REQUIRE(fx.log == std::vector<std::string>{
            "enter:a", "enter:b", "exit:b", "exit:a"});
    };

    auto fx = build_chain_with_booming_inner_exit();
    run_chain_with_booming_inner_exit(*fx);
    expect_outer_exit_survives_inner_throw(*fx);
}

TEST_CASE("with：本体按址收切面实参（形参类型明写）", "[with]") {
    struct Fx {
        Subject subj{7};
        int got = 0;
    };

    auto build_aspect_anchored_on_subject = [] {
        return std::make_unique<Fx>();
    };
    auto run_body_taking_subject_ref = [](Fx& fx) {
        Aspect a{&fx.subj, always_enter, nullptr, "s", &fx.subj};
        fx.got = with(a, [](Subject& s) { return s.x * 2; });
    };
    auto expect_subject_received_by_reference = [](const Fx& fx) {
        REQUIRE(fx.got == 14);
    };

    auto fx = build_aspect_anchored_on_subject();
    run_body_taking_subject_ref(*fx);
    expect_subject_received_by_reference(*fx);
}

TEST_CASE("with：subject 取最内层非空", "[with]") {
    struct Fx {
        Subject outer{1}, inner{2};
        int got = 0;
    };

    auto build_nested_subject_aspects = [] {
        return std::make_unique<Fx>();
    };
    auto run_with_two_subject_aspects = [](Fx& fx) {
        Aspect a1{nullptr, always_enter, nullptr, "outer", &fx.outer};
        Aspect a2{nullptr, always_enter, nullptr, "inner", &fx.inner};
        fx.got = with(a1, a2, [](Subject& s) { return s.x; });
    };
    auto expect_innermost_subject_wins = [](const Fx& fx) {
        REQUIRE(fx.got == 2);
    };

    auto fx = build_nested_subject_aspects();
    run_with_two_subject_aspects(*fx);
    expect_innermost_subject_wins(*fx);
}

TEST_CASE("with 分流形：按 type 命中一行，事件原样透传", "[with]") {
    struct Fx {
        Event ev{};
        std::vector<EventType> hit;
        bool found = false;
    };

    auto build_type_200_event = [] {
        auto fx = std::make_unique<Fx>();
        fx->ev.type = 200;
        fx->ev.ts_ns = 42;
        fx->ev.payload = 7;
        return fx;
    };
    auto run_dispatch_on_type_200 = [](Fx& fx) {
        fx.found = with(fx.ev, {
            {100, [&](const Event& e) { fx.hit.push_back(e.type); }},
            {200, [&](const Event& e) {
                 fx.hit.push_back(static_cast<EventType>(std::any_cast<int>(e.payload)));
                 fx.hit.push_back(static_cast<EventType>(e.ts_ns));
             }},
        });
    };
    auto expect_row_hit_and_payload_forwarded = [](const Fx& fx) {
        REQUIRE(fx.found);
        REQUIRE(fx.hit == std::vector<EventType>{7, 42});
    };

    auto fx = build_type_200_event();
    run_dispatch_on_type_200(*fx);
    expect_row_hit_and_payload_forwarded(*fx);
}

TEST_CASE("with 分流形：查无 → 不调用并返回 false", "[with]") {
    struct Fx {
        Event ev{};
        bool called = false;
        bool found = false;
    };

    auto build_event_missing_from_table = [] {
        auto fx = std::make_unique<Fx>();
        fx->ev.type = 200;
        return fx;
    };
    auto run_dispatch_without_matching_row = [](Fx& fx) {
        fx.found = with(fx.ev, {
            {100, [&](const Event&) { fx.called = true; }},
        });
    };
    auto expect_no_row_called = [](const Fx& fx) {
        REQUIRE_FALSE(fx.found);
        REQUIRE_FALSE(fx.called);
    };

    auto fx = build_event_missing_from_table();
    run_dispatch_without_matching_row(*fx);
    expect_no_row_called(*fx);
}

TEST_CASE("with：enter/exit 可空——无 enter 放行、无 exit 无收尾", "[with]") {
    struct Fx {
        std::vector<std::string> log;
        Probe p1{&log};
        Aspect bare{nullptr, nullptr, nullptr, "bare"};   // 既无 enter 也无 exit
        int got = 0;
    };

    auto build_probe_plus_bare_aspect = [] {
        return std::make_unique<Fx>();
    };
    auto run_with_bare_aspect = [](Fx& fx) {
        fx.got = with(probe(fx.p1, "a"), fx.bare, [] { return 5; });
    };
    auto expect_bare_aspect_adds_nothing = [](const Fx& fx) {
        REQUIRE(fx.got == 5);
        REQUIRE(fx.log == std::vector<std::string>{"enter:a", "exit:a"});
    };

    auto fx = build_probe_plus_bare_aspect();
    run_with_bare_aspect(*fx);
    expect_bare_aspect_adds_nothing(*fx);
}

TEST_CASE("with + live：stopped 拒绝，本体不进", "[with]") {
    struct Fx {
        struct Res {
            bool dead = false;
            bool stopped() const { return dead; }
        };
        Res r;
        bool alive_out = false;
        bool stopped_body_ran = false;
        bool stopped_out = true;     // 初值取 true：act 没跑到就过不了断言
    };

    auto build_live_resource = [] {
        return std::make_unique<Fx>();
    };
    auto run_live_then_stopped = [](Fx& fx) {
        fx.alive_out = with(live(fx.r), [] { return true; });
        fx.r.dead = true;
        fx.stopped_out = with(live(fx.r), [&] {
            fx.stopped_body_ran = true;
            return true;
        });
    };
    auto expect_stopped_rejects_body = [](const Fx& fx) {
        REQUIRE(fx.alive_out);
        REQUIRE_FALSE(fx.stopped_body_ran);
        REQUIRE_FALSE(fx.stopped_out);
    };

    auto fx = build_live_resource();
    run_live_then_stopped(*fx);
    expect_stopped_rejects_body(*fx);
}

// ===== ticket / settle：账本协议语义（假账本，无超时形） =====

namespace {

struct MockEntry {
    uint64_t id = 0;
    int inflight = 0;
    bool cancelled = false;
    int value = 0;
};

struct MockTable {
    using entry_type = MockEntry;
    using id_type = uint64_t;

    std::vector<std::unique_ptr<MockEntry>> store;
    uint64_t next = 1;
    size_t late_drops = 0;

    uint64_t alloc_id() { return next++; }
    MockEntry& insert(MockEntry e) {
        auto p = std::make_unique<MockEntry>(e);
        auto* raw = p.get();
        store.push_back(std::move(p));
        return *raw;
    }
    MockEntry* find(uint64_t k) {
        for (auto& x : store)
            if (x->id == k) return x.get();
        return nullptr;
    }
    void erase(MockEntry& e) {
        std::erase_if(store, [&](auto& x) { return x.get() == &e; });
    }
    size_t size() const { return store.size(); }
};

const OpPolicy kMockRow{OpKind::kUringIo, TimeoutSrc::kNone, 0,
                        OpTimeoutAct::kNone, OpLateAct::kDropCount};

}  // namespace

TEST_CASE("ticket：发号登记，本体收登记后条目；正常返回不销账", "[with]") {
    struct Fx {
        MockTable tab;
        uint64_t seen_id = 0;
        int out = 0;
    };

    auto build_empty_ledger = [] {
        return std::make_unique<Fx>();
    };
    auto run_ticket_with_body = [](Fx& fx) {
        fx.out = with(ticket(fx.tab, MockEntry{.value = 7}, kMockRow, nullptr, 0, Event{}),
                      [&](MockEntry& e) {
                          fx.seen_id = e.id;
                          return e.value;
                      });
    };
    auto expect_entry_registered_not_settled = [](const Fx& fx) {
        REQUIRE(fx.out == 7);
        REQUIRE(fx.seen_id == 1);
        REQUIRE(fx.tab.size() == 1);
    };

    auto fx = build_empty_ledger();
    run_ticket_with_body(*fx);
    expect_entry_registered_not_settled(*fx);
}

TEST_CASE("settle：结案销账；查无/作废 = 迟到拒绝并计数", "[with]") {
    struct Fx {
        MockTable tab;
        size_t size_after_close = 0;
        bool miss_ran = false;
        size_t late_after_miss = 0;
        bool void_ran = false;
        size_t size_after_void = 0;
        size_t late_after_void = 0;
    };

    auto build_empty_ledger = [] {
        return std::make_unique<Fx>();
    };
    auto run_close_miss_and_void_settles = [](Fx& fx) {
        with(ticket(fx.tab, MockEntry{.value = 7}, kMockRow, nullptr, 0, Event{}),
             [&](MockEntry& e) {
                 with(settle(fx.tab, e.id, kMockRow), [&](MockEntry& s) { s.value = 9; });
             });
        fx.size_after_close = fx.tab.size();

        with(settle(fx.tab, uint64_t(42), kMockRow),
             [&](MockEntry&) { fx.miss_ran = true; });
        fx.late_after_miss = fx.tab.late_drops;

        MockEntry voided{};
        voided.id = fx.tab.alloc_id();
        voided.inflight = 1;
        voided.cancelled = true;            // 作废 = 就地收口 + 拒绝
        fx.tab.insert(voided);
        with(settle(fx.tab, voided.id, kMockRow),
             [&](MockEntry&) { fx.void_ran = true; });
        fx.size_after_void = fx.tab.size();
        fx.late_after_void = fx.tab.late_drops;
    };
    auto expect_closed_and_late_drops_counted = [](const Fx& fx) {
        REQUIRE(fx.size_after_close == 0);           // settle 结案销账
        REQUIRE_FALSE(fx.miss_ran);                  // 查无 = 迟到
        REQUIRE(fx.late_after_miss == 1);
        REQUIRE_FALSE(fx.void_ran);                  // 作废 = 拒绝
        REQUIRE(fx.size_after_void == 0);
        REQUIRE(fx.late_after_void == 2);
    };

    auto fx = build_empty_ledger();
    run_close_miss_and_void_settles(*fx);
    expect_closed_and_late_drops_counted(*fx);
}

TEST_CASE("ticket：本体抛异常走失败收口（销账不悬空）", "[with]") {
    struct Fx {
        MockTable tab;
        bool threw = false;
    };

    auto build_empty_ledger = [] {
        return std::make_unique<Fx>();
    };
    auto run_ticket_with_throwing_body = [](Fx& fx) {
        try {
            with(ticket(fx.tab, MockEntry{}, kMockRow, nullptr, 0, Event{}),
                 [&](MockEntry&) -> void { throw std::runtime_error("x"); });
        } catch (...) {
            fx.threw = true;
        }
    };
    auto expect_failed_close_erases_entry = [](const Fx& fx) {
        REQUIRE(fx.threw);
        REQUIRE(fx.tab.size() == 0);
    };

    auto fx = build_empty_ledger();
    run_ticket_with_throwing_body(*fx);
    expect_failed_close_erases_entry(*fx);
}

TEST_CASE("ticket/settle：inflight 归零才销毁", "[with]") {
    struct Fx {
        MockTable tab;
        size_t size_after_first = 0;
        size_t size_after_second = 0;
    };

    auto build_empty_ledger = [] {
        return std::make_unique<Fx>();
    };
    auto run_two_settles_on_one_ticket = [](Fx& fx) {
        with(ticket(fx.tab, MockEntry{}, kMockRow, nullptr, 0, Event{}),
             [&](MockEntry& e) {
                 e.inflight = 2;
                 with(settle(fx.tab, e.id, kMockRow), [](MockEntry&) {});
                 fx.size_after_first = fx.tab.size();
                 with(settle(fx.tab, e.id, kMockRow), [](MockEntry&) {});
                 fx.size_after_second = fx.tab.size();
             });
    };
    auto expect_destroyed_only_at_zero = [](const Fx& fx) {
        REQUIRE(fx.size_after_first == 1);       // 还剩一结
        REQUIRE(fx.size_after_second == 0);      // 归零销毁
    };

    auto fx = build_empty_ledger();
    run_two_settles_on_one_ticket(*fx);
    expect_destroyed_only_at_zero(*fx);
}
