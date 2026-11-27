#pragma once

// ===== with.h：机械性切面（纯头文件：没有实现、没有进程、不占模块号） =====
// lambda 模拟 AOP around：业务住 lambda（本体），机械住切面；切面 = 统一 Aspect 数据形，
// with（切面…, 本体）一次平铺、不嵌套。详见 doc/with.md。
// 顺序条款由结构保证（exit 恒执行且恰好一次——完成 / 抛异常几条收尾路收敛到同一 exit）：
//   · enter 按列出顺序（外 → 内）；kReject → 后续切面与本体都不进，
//     已进切面照常 exit（拒绝处置住该切面的 enter 本身）。
//   · exit 逆序（内 → 外），只对 enter 放行的切面执行。
//   · 本体返回值原样透传；拒绝返回值初始化（void 本体拒绝即返回）。
//
// 与设计稿（doc/with.md）的两处落地偏移（用法一致、形微调）：
//   · Aspect 增补第 5 字段 subject：设计稿示例本体带形参（如 [&](RequestEntry& e)），
//     需要一处实参来源——subject 指向本体实参对象，取最内层非空；本体要收实参时
//     形参类型须明写（泛型 lambda 的 auto 收不到实参）。
//   · HandlerRow.fn 用 std::function：分流表以花括号列捕获 lambda（各模块 inbox 用法），
//     裸函数指针收不了捕获。
// 已落地的工厂（P1，签名微调已注在定义处）：live / ticket / settle / backpressure / rearm。
// 待随模块落地：defer（01 代跑，03/05/06 用）、intercept（错误拦截）、block（02 数据面块）、
// heap（04 动堆）、rpc（03/06 桩壳）——签名见 doc/with.md，状态/enter/exit 语义住各模块文档。

#include <cassert>
#include <concepts>
#include <exception>
#include <functional>
#include <initializer_list>
#include <tuple>
#include <type_traits>
#include <utility>

#include "disp.h"   // 分流形 with(ev, …) 需要 Event 完整形（01 契约）；backpressure/rearm 用 01 门面
#include "tmr.h"   // ticket/settle 的超时挂撤（04 契约）

enum class Outcome { kEnter, kReject };      // enter 的裁决：进本体 / 拒绝

struct Aspect {
    void*        state;          // 机械状态：账本指针 / 句柄 / 策略行指针（policy.h）；业务数据禁入
    Outcome    (*enter)(void*);  // 前置机械，按列出顺序（外 → 内）；kReject → 后续切面与本体都不进
    void       (*exit)(void*);   // 后置机械，逆序（内 → 外），恒执行且恰好一次；无收尾 = nullptr
    const char*  name;           // 观测/审查用
    void*        subject = nullptr;  // 本体实参对象（可空）：本体写明形参类型即按址收它（取最内层非空）
};

struct HandlerRow {             // 分流表行（with 第二形）：按 type 查表分派，查无 → 丢弃并计数
    EventType type;
    std::function<void(const Event&)> fn;
};

namespace with_detail {

struct Guard {                  // exit 恰好一次的结构保证（含异常展开路）
    void* state;
    void (*exit_fn)(void*);
    explicit Guard(void* s, void (*e)(void*)) : state(s), exit_fn(e) {}
    ~Guard() noexcept(false) { if (exit_fn) exit_fn(state); }
        // exit 的异常非展开途中外传（外层 exit 在展开途中照常跑）；
        // 异常展开途中 exit 再抛 = 两异常并行，std::terminate——exit 本体必须稳
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

// 本体实参类型提取：写明形参类型的本体收切面实参；泛型 lambda / 无参本体按无参调用
template <class F, class Enable = void> struct arg_of { using type = void; };

template <class R, class C, class A, class... As>
struct arg_of<R (C::*)(A, As...), void>       { using type = A; };
template <class R, class C, class A, class... As>
struct arg_of<R (C::*)(A, As...) const, void> { using type = A; };
template <class R, class A, class... As>
struct arg_of<R (*)(A, As...), void>          { using type = A; };

template <class F>
struct arg_of<F, std::void_t<decltype(&F::operator())>>
    : arg_of<decltype(&F::operator()), void> {};

template <class F, bool HasArg> struct body_result_impl;
template <class F> struct body_result_impl<F, true> {
    using Arg = typename arg_of<F>::type;
    static_assert(std::is_invocable_v<F&, std::remove_reference_t<Arg>&>,
                  "with: 本体按址收切面实参时形参类型须明写（不能用 auto）");
    using type = std::invoke_result_t<F&, std::remove_reference_t<Arg>&>;
};
template <class F> struct body_result_impl<F, false> {
    static_assert(std::is_invocable_v<F&>, "with: 本体不可无参调用");
    using type = std::invoke_result_t<F&>;
};
template <class F>
struct body_result : body_result_impl<F, !std::is_void_v<typename arg_of<F>::type>> {};

template <class R, class F>
R invoke_body(F& f, void* subject) {
    using Arg = typename arg_of<F>::type;
    if constexpr (std::is_void_v<Arg>) {
        if constexpr (std::is_void_v<R>) f();
        else return f();
    } else {
        using T = std::remove_reference_t<Arg>;
        assert(subject != nullptr && "with: 本体要收切面实参，但切面没给 subject");
        if constexpr (std::is_void_v<R>) f(*static_cast<T*>(subject));
        else return f(*static_cast<T*>(subject));
    }
}

template <class R>
R reject_result() {
    if constexpr (!std::is_void_v<R>) {
        static_assert(std::is_default_constructible_v<R>,
                      "with: 拒绝路径要求本体返回可值初始化的类型（或 void）");
        return R{};
    }
}

// enter 外→内、exit 内→外：每层一个 Guard，异常展开也恰好走一次；
// subject 在各层 enter 之后逐层回填（切面可在 enter 里定位条目），内层非空覆盖外层
template <class R, class F>
R chain(F& f, void** psubject) {
    return invoke_body<R>(f, *psubject);
}

template <class R, class F, class A, class... As>
R chain(F& f, void** psubject, A& a, As&... as) {
    if (a.enter && a.enter(a.state) == Outcome::kReject)   // 无 enter = 放行
        return reject_result<R>();                         // 拒绝：本体与后续切面不进
    if (a.subject) *psubject = a.subject;
    Guard g{a.state, a.exit};
    return chain<R, F, As...>(f, psubject, as...);
}

template <class Tup, std::size_t... Asp, std::size_t Body>
decltype(auto) invoke_all(Tup& t, std::index_sequence<Asp...>, std::index_sequence<Body>) {
    using FD = std::remove_reference_t<std::tuple_element_t<Body, Tup>>;
    using R  = typename body_result<FD>::type;
    void* subject = nullptr;
    return chain<R>(std::get<Body>(t), &subject, std::get<Asp>(t)...);
}

}  // namespace with_detail

// with（切面…, 本体）：末位 = 业务 lambda，其余 = Aspect 值，一次平铺列出
template <class... Ts>
decltype(auto) with(Ts&&... ts) {
    static_assert(sizeof...(Ts) >= 1, "with: 至少收一个本体（末位）");
    auto t = std::forward_as_tuple(std::forward<Ts>(ts)...);
    return with_detail::invoke_all(t,
        std::make_index_sequence<sizeof...(Ts) - 1>{},
        std::index_sequence<sizeof...(Ts) - 1>{});
}

// with（ev, 分流表）：按 Event.type 查表分派；查无 → 不调用并返回 false（丢弃计数归调用点观测位）
inline bool with(const Event& ev, std::initializer_list<HandlerRow> table) {
    for (const auto& row : table)
        if (row.type == ev.type) { row.fn(ev); return true; }
    return false;
}

// ===== 标准切面库 =====
//
// 账本协议（ticket/settle 的 Tab 实现约定，01 票根 / 02 请求表 / 06 任务表共用）：
//   typename Tab::entry_type、typename Tab::id_type
//   id_type     alloc_id()          发号（单调不复用）
//   entry_type& insert(entry_type)  登记（表持有），返回表内条目
//   entry_type* find(const Key&)    查（settle 用；Key 随表：单号 / 条目指针）
//   void        erase(entry_type&)  销档
//   size_t      late_drops          迟到丢弃计数（拒绝路递增）
// 条目协议：.id（发号回填）、.inflight（在飞计数：登记 = 1，settle 减，归零才销毁）、
//          .cancelled（可选，作废标记）、.th（可选 TimerHandle，挂超时的表必带）。
//
// 生命周期口径（相对设计稿 ticket/settle exit 列的落地澄清）：
//   · ticket：enter 发号 + 登记 + 按行挂超时（入 .th）；exit 只做**失败收口**（本体抛异常
//     才销账撤超时）——本体正常返回时单据仍在飞，收口归 settle；否则请求在飞即被销账。
//   · settle：enter 查条目（落空/作废 = 迅到：late_drops 计数拒绝，rearm 不执行）；
//     exit = 结案销账 + 撤超时 + 归零才销毁——完成/超时/断连/取消几条收尾路汇到这同一 exit。
//   · 收口唯一化：同一单的 ticket 失败收口与 settle 收口二选一——本体内先 settle 再抛异常
//     属使用错误（失败收口会碰已销条目）；现有调用形两收口分属不同 with 语境，天然不相犯。

template <class R>
concept Stoppable = requires (const R& r) { { r.stopped() } -> std::convertible_to<bool>; };

template <class T>
concept Ledger = requires { typename T::entry_type; typename T::id_type; };

// live(x)：stop 后一律拒绝——enter 查 stopped，无 exit
template <Stoppable R>
Aspect live(R& x) {
    return Aspect{
        &x,
        [](void* s) -> Outcome {
            return static_cast<R*>(s)->stopped() ? Outcome::kReject : Outcome::kEnter;
        },
        nullptr,
        "live",
    };
}

namespace with_detail {

// 切面工厂的上下文随形体携带：工厂按值返回（保证省略构造于最终位置），state 指向自身
// 内嵌上下文，with() 以引用接住——上下文活到 with 全表达式末，无需额外分配。

template <class Entry>
concept ThField = requires (Entry& e) { e.th; };

template <class Tab>
void close_entry(Tab& tab, typename Tab::entry_type& e, Timer* t) {   // 结案销账 + 撤超时 + 归零才销毁
    using Entry = typename Tab::entry_type;
    if constexpr (ThField<Entry>)
        if (t && e.th) { tmr::cancel(*t, e.th); e.th = 0; }
    if (--e.inflight <= 0) tab.erase(e);
}

template <class Tab>
struct Ticket : Aspect {
    using Entry = typename Tab::entry_type;

    struct Ctx {
        Tab*            tab;
        Entry           entry;        // 条目按值收：enter 发号后登记进表
        const OpPolicy* row;
        Timer*          timer;
        uint64_t        timeout_ns;
        Event           tpl;
        Aspect*         asp = nullptr;   // 回指形体：enter 回填 subject（登记后条目）
        Entry*          reg = nullptr;
        int             ue = 0;          // enter 时的异常展开计数（判本体是否抛出）
    } ctx;

    Ticket(Tab& tab, Entry e, const OpPolicy& r, Timer* t, uint64_t ns, Event tpl)
        : ctx{&tab, std::move(e), &r, t, ns, std::move(tpl), this} {
        state = &ctx;
        enter = &Ticket::enter_fn;
        exit  = &Ticket::exit_fn;
        name  = "ticket";
    }

    static Outcome enter_fn(void* s) {
        auto& c = *static_cast<Ctx*>(s);
        c.ue = std::uncaught_exceptions();
        c.entry.id = c.tab->alloc_id();                    // ticket：登记本单（发号）
        c.entry.inflight = 1;
        c.reg = &c.tab->insert(std::move(c.entry));
        c.asp->subject = c.reg;                            // 本体收登记后条目
        if constexpr (ThField<Entry>) {
            if (c.timer && c.row->timeout_src != TimeoutSrc::kNone) {
                uint64_t ns = c.row->timeout_src == TimeoutSrc::kSpec ? c.row->timeout_ns
                                                                     : c.timeout_ns;
                c.reg->th = tmr::after(*c.timer, ns, c.tpl);   // 按行挂超时
            }
        } else {
            assert(!c.timer || c.row->timeout_src == TimeoutSrc::kNone);   // 协议：挂超时的表必带 th
        }
        return Outcome::kEnter;
    }

    static void exit_fn(void* s) {
        auto& c = *static_cast<Ctx*>(s);
        if (std::uncaught_exceptions() > c.ue) close(c);   // 失败收口：正常路归 settle
    }

    static void close(Ctx& c) { close_entry(*c.tab, *c.reg, c.timer); }
};

template <class Tab, class Key>
struct Settle : Aspect {
    using Entry = typename Tab::entry_type;

    struct Ctx {
        Tab*            tab;
        Key             key;
        const OpPolicy* row;
        Timer*          timer;
        Aspect*         asp = nullptr;
        Entry*          found = nullptr;
    } ctx;

    Settle(Tab& tab, Key key, const OpPolicy& r, Timer* t)
        : ctx{&tab, std::move(key), &r, t, this} {
        state = &ctx;
        enter = &Settle::enter_fn;
        exit  = &Settle::exit_fn;
        name  = "settle";
    }

    static Outcome enter_fn(void* s) {
        auto& c = *static_cast<Ctx*>(s);
        Entry* e = c.tab->find(c.key);                     // settle：查条目
        if (e && is_cancelled(*e)) {                       // 作废 = 迅到：就地收口（拒绝处置）
            close_entry(*c.tab, *e, c.timer);
            e = nullptr;
        }
        if (!e) {                                          // 落空 = 迅到：丢弃并计数
            c.tab->late_drops++;
            return Outcome::kReject;
        }
        c.found = e;
        c.asp->subject = e;                                // 本体收查到的条目
        return Outcome::kEnter;
    }

    static void exit_fn(void* s) {
        auto& c = *static_cast<Ctx*>(s);
        close_entry(*c.tab, *c.found, c.timer);
    }

    static bool is_cancelled(const Entry& e) {
        if constexpr (requires { e.cancelled; }) return e.cancelled;
        else return false;
    }
};

class Backpressure : public Aspect {
    struct Ctx { Dispatcher* d; const Event* ev; } ctx;
public:
    Backpressure(Dispatcher& d, const Event& ev) : ctx{&d, &ev} {
        state = &ctx;
        enter = &Backpressure::enter_fn;
        exit  = nullptr;
        name  = "backpressure";
    }
    static Outcome enter_fn(void* s) {                     // 丢弃即计数的计数落点在入队口
        auto& c = *static_cast<Ctx*>(s);
        return disp::should_drop(*c.ev, disp::queue_len(*c.d)) ? Outcome::kReject
                                                               : Outcome::kEnter;
    }
};

class Rearm : public Aspect {
    struct Ctx { Dispatcher* d; OpCtx op; } ctx;
public:
    Rearm(Dispatcher& d, const OpCtx& op) : ctx{&d, op} {
        state = &ctx;
        enter = nullptr;
        exit  = &Rearm::exit_fn;
        name  = "rearm";
    }
    static void exit_fn(void* s) {                         // 读完成必重挂（恒执行恰好一次）
        auto& c = *static_cast<Ctx*>(s);
        disp::submit_op(*c.d, c.op);
    }
};

}  // namespace with_detail

// ticket(tab, e, row, t, ns, tpl)：登记本单——发号 + 登记 + 按行挂超时（t 空 = 不挂）
template <Ledger Tab>
with_detail::Ticket<Tab> ticket(Tab& tab, typename Tab::entry_type e, const OpPolicy& row,
                               Timer* t, uint64_t ns, Event tpl) {
    return with_detail::Ticket<Tab>{tab, std::move(e), row, t, ns, std::move(tpl)};
}

// settle(tab, key, row, t = 空)：结案本单——查条目 + 收口（撤超时要 t）
template <Ledger Tab, class Key>
with_detail::Settle<Tab, std::remove_reference_t<Key>>
settle(Tab& tab, Key&& key, const OpPolicy& row, Timer* t = nullptr) {
    return with_detail::Settle<Tab, std::remove_reference_t<Key>>{
        tab, std::forward<Key>(key), row, t};
}

// backpressure(d, ev)：背压判定——查 ev.def 的 qos 行（行指针随事件，调用点可见）
inline with_detail::Backpressure backpressure(Dispatcher& d, const Event& ev) {
    return with_detail::Backpressure{d, ev};
}

// rearm(d, op)：读完成必重挂——exit 交副本重新上环（每对端/连接/设备一张在飞）
inline with_detail::Rearm rearm(Dispatcher& d, const OpCtx& op) {
    return with_detail::Rearm{d, op};
}
