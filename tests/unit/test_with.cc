// with.h 切面语义单测（P0）：
// enter 外→内、exit 内→外且恒恰好一次、拒绝路、返回值透传、本体实参、分流形、live；
// enter/exit 可空；ticket/settle 账本协议语义（假账本，无超时形）。
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "disp.h"
#include "with.h"

namespace {

// 记录切面动作的机械探针（业务数据不进切面 state 的规则用它反着验）
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
    std::vector<std::string> log;
    Probe p1{&log}, p2{&log}, p3{&log};

    with(probe(p1, "a"), probe(p2, "b"), probe(p3, "c"),
         [&] { log.push_back("body"); });

    REQUIRE(log == std::vector<std::string>{
        "enter:a", "enter:b", "enter:c", "body", "exit:c", "exit:b", "exit:a"});
}

TEST_CASE("with：拒绝——后续切面与本体不进，已进切面照常 exit", "[with]") {
    std::vector<std::string> log;
    Probe p1{&log}, p2{&log}, p3{&log};
    p2.enter_result = Outcome::kReject;

    bool body_ran = false;
    with(probe(p1, "a"), probe(p2, "b"), probe(p3, "c"),
         [&] { body_ran = true; });

    REQUIRE_FALSE(body_ran);
    REQUIRE(log == std::vector<std::string>{"enter:a", "enter:b", "exit:a"});
}

TEST_CASE("with：本体返回值原样透传", "[with]") {
    std::vector<std::string> log;
    Probe p1{&log};

    int v = with(probe(p1, "a"), [] { return 42; });
    REQUIRE(v == 42);
}

TEST_CASE("with：void 本体照常收尾", "[with]") {
    std::vector<std::string> log;
    Probe p1{&log};

    bool ran = false;
    with(probe(p1, "a"), [&] { ran = true; });

    REQUIRE(ran);
    REQUIRE(log == std::vector<std::string>{"enter:a", "exit:a"});
}

TEST_CASE("with：拒绝返回值初始化", "[with]") {
    std::vector<std::string> log;
    Probe p1{&log};
    p1.enter_result = Outcome::kReject;

    int v = with(probe(p1, "a"), [] { return 42; });
    REQUIRE(v == 0);

    bool b = with(probe(p1, "a"), [] { return true; });
    REQUIRE_FALSE(b);
}

TEST_CASE("with：零切面直接跑本体", "[with]") {
    int v = with([] { return 7; });
    REQUIRE(v == 7);
}

TEST_CASE("with：本体抛异常也走 exit（恰好一次）", "[with]") {
    std::vector<std::string> log;
    int exits = 0;
    Probe p1{&log, "a", Outcome::kEnter, &exits};

    REQUIRE_THROWS(with(probe(p1, "a"), [] { throw std::runtime_error("body"); }));
    REQUIRE(exits == 1);
    REQUIRE(log.back() == "exit:a");
}

TEST_CASE("with：内层 exit 抛异常，外层 exit 照常", "[with]") {
    std::vector<std::string> log;
    int exits_a = 0, exits_b = 0;
    Probe p1{&log, "a", Outcome::kEnter, &exits_a};
    Probe p2{&log, "b", Outcome::kEnter, &exits_b, /*throw_on_exit=*/true};

    REQUIRE_THROWS(with(probe(p1, "a"), probe(p2, "b"), [] {}));
    REQUIRE(exits_a == 1);
    REQUIRE(exits_b == 1);
    REQUIRE(log == std::vector<std::string>{
        "enter:a", "enter:b", "exit:b", "exit:a"});
}

TEST_CASE("with：本体按址收切面实参（形参类型明写）", "[with]") {
    Subject subj{7};
    Aspect a{&subj, always_enter, nullptr, "s", &subj};

    int got = with(a, [](Subject& s) { return s.x * 2; });
    REQUIRE(got == 14);
}

TEST_CASE("with：subject 取最内层非空", "[with]") {
    Subject outer{1}, inner{2};
    Aspect a1{nullptr, always_enter, nullptr, "outer", &outer};
    Aspect a2{nullptr, always_enter, nullptr, "inner", &inner};

    int got = with(a1, a2, [](Subject& s) { return s.x; });
    REQUIRE(got == 2);
}

TEST_CASE("with 分流形：按 type 命中一行，事件原样透传", "[with]") {
    Event ev{};
    ev.type = 200;
    ev.ts_ns = 42;
    ev.payload = 7;

    std::vector<EventType> hit;
    bool found = with(ev, {
        {100, [&](const Event& e) { hit.push_back(e.type); }},
        {200, [&](const Event& e) {
             hit.push_back(static_cast<EventType>(std::any_cast<int>(e.payload)));
             hit.push_back(static_cast<EventType>(e.ts_ns));
         }},
    });

    REQUIRE(found);
    REQUIRE(hit == std::vector<EventType>{7, 42});
}

TEST_CASE("with 分流形：查无 → 不调用并返回 false", "[with]") {
    Event ev{};
    ev.type = 200;

    bool called = false;
    bool found = with(ev, {
        {100, [&](const Event&) { called = true; }},
    });

    REQUIRE_FALSE(found);
    REQUIRE_FALSE(called);
}

TEST_CASE("with：enter/exit 可空——无 enter 放行、无 exit 无收尾", "[with]") {
    std::vector<std::string> log;
    Probe p1{&log};
    Aspect bare{nullptr, nullptr, nullptr, "bare"};   // 既无 enter 也无 exit

    int v = with(probe(p1, "a"), bare, [] { return 5; });

    REQUIRE(v == 5);
    REQUIRE(log == std::vector<std::string>{"enter:a", "exit:a"});
}

TEST_CASE("with + live：stopped 拒绝，本体不进", "[with]") {
    struct Res {
        bool dead = false;
        bool stopped() const { return dead; }
    };

    Res r;
    bool ran = with(live(r), [&] { return true; });
    REQUIRE(ran);

    r.dead = true;
    bool body_ran = false;
    bool out = with(live(r), [&] { body_ran = true; return true; });
    REQUIRE_FALSE(body_ran);
    REQUIRE_FALSE(out);
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
    MockTable tab;

    uint64_t seen_id = 0;
    int out = with(ticket(tab, MockEntry{.value = 7}, kMockRow, nullptr, 0, Event{}),
                   [&](MockEntry& e) {
                       seen_id = e.id;
                       return e.value;
                   });

    REQUIRE(out == 7);
    REQUIRE(seen_id == 1);
    REQUIRE(tab.size() == 1);   // 单据仍在飞，收口归 settle
}

TEST_CASE("settle：结案销账；查无/作废 = 迟到拒绝并计数", "[with]") {
    MockTable tab;

    with(ticket(tab, MockEntry{.value = 7}, kMockRow, nullptr, 0, Event{}),
         [&](MockEntry& e) {
             with(settle(tab, e.id, kMockRow), [&](MockEntry& s) { s.value = 9; });
         });
    REQUIRE(tab.size() == 0);           // settle 结案销账

    bool ran = false;                   // 查无 = 迟到
    with(settle(tab, uint64_t(42), kMockRow), [&](MockEntry&) { ran = true; });
    REQUIRE_FALSE(ran);
    REQUIRE(tab.late_drops == 1);

    MockEntry voided{};
    voided.id = tab.alloc_id();
    voided.inflight = 1;
    voided.cancelled = true;            // 作废 = 就地收口 + 拒绝
    tab.insert(voided);
    with(settle(tab, voided.id, kMockRow), [&](MockEntry&) { ran = true; });
    REQUIRE_FALSE(ran);
    REQUIRE(tab.size() == 0);
    REQUIRE(tab.late_drops == 2);
}

TEST_CASE("ticket：本体抛异常走失败收口（销账不悬空）", "[with]") {
    MockTable tab;

    REQUIRE_THROWS(with(ticket(tab, MockEntry{}, kMockRow, nullptr, 0, Event{}),
                        [&](MockEntry&) -> void { throw std::runtime_error("x"); }));
    REQUIRE(tab.size() == 0);
}

TEST_CASE("ticket/settle：inflight 归零才销毁", "[with]") {
    MockTable tab;

    with(ticket(tab, MockEntry{}, kMockRow, nullptr, 0, Event{}), [&](MockEntry& e) {
        e.inflight = 2;   // 一单两结（订阅形）
        with(settle(tab, e.id, kMockRow), [](MockEntry&) {});
        REQUIRE(tab.size() == 1);       // 还剩一结
        with(settle(tab, e.id, kMockRow), [](MockEntry&) {});
        REQUIRE(tab.size() == 0);       // 归零销毁
    });
}
