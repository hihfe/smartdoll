# 测试规范（Arrange-Act-Assert 三段式）

> `tests/` 下**所有** `TEST_CASE` 一律三段式：Arrange（造夹具）→ Act（驱动被测实现）→ Assert（断言），且**每段由一个具名函数承载**——函数名必须点出该段的实际用途，`build_` / `run_` / `expect_` 取向即三段次序，不另写段标记注释。换实现只改 Act，读用例先看三行调用。

## 范式模板

三个具名函数依次承载三段（Arrange = `build_*`、Act = `run_*`、Assert = `expect_*`），段的性质由名字承载，**不写 `// Arrange` 之类段标记注释**；模板保持无注释骨架，语义说明见下方规则：

```cpp
TEST_CASE("用例名", "[tag]") {
    struct Fx { /* 输入与输出集中一处 */ };

    auto build_xxx = [] {
        auto fx = std::make_unique<Fx>();
        fx->输入 = …;
        return fx;
    };
    auto run_xxx = [](Fx& fx) {
        fx.输出 = 被测实现(fx.输入);
    };
    auto expect_xxx = [](const Fx& fx) {
        REQUIRE(fx.输出 == …);
    };

    auto fx = build_xxx();
    run_xxx(*fx);
    expect_xxx(*fx);
}
```

## 规则

1. **每段恰好一个具名函数（用例内 lambda），用例体 = 三段定义 + 三行调用**，不夹带其他语句。
2. **名字必须表现实际用途并承载段的性质**，不用 `arrange` / `act` / `assert_results` 这类泛化名——例如 `build_b_rejecting_chain`、`run_with_three_probes`、`expect_outer_to_inner_order`。命名取向：Arrange 段 `build_/prepare_/make_`，Act 段动词点名被测行为，Assert 段 `expect_/verify_`；名字即段标记，不另写 `// Arrange` 注释。
3. **Arrange 只造不测**：夹具构造、数据配置、订阅注册都在这里；不得出现 `REQUIRE`——唯一例外是**夹具自身的系统调用自检**（`eventfd_create` / `timerfd_settime` 失败 = 造不出夹具，不是被测行为，就地 `REQUIRE` 快速失败，不必记进夹具再断言）。
4. **Act 是唯一允许调用被测实现的段**：返回值、异常、事件盒计数等一切可观察结果写回夹具（`fx->…`）；段内不得出现 `REQUIRE` / `CHECK`。**每个要在 Assert 读的观察，Act 必须真的采样一次**——漏记会让字段停在哨兵初值（哨兵按「期望相反」设计时漏记当场报错，初值与期望同向的会静默通过，写完自查：expect 里 `fx.` 的每个读点在 build/act 都有写点）。多步时序用例（暂停/恢复、迟到拒绝等）把「步骤 + 每步观察」整体放 Act，中间观察记进夹具，Assert 一次性断言。
5. **Assert 只断言，零行为**：不得再调被测实现。允许带期限等待（`wait_type` / `wait_until`）——等待是为了观察 Act 的结果，等待本身不是行为。
6. **夹具统一 `std::make_unique` 出来，构造后不再移动**：夹具可能含捕获 `this` 的对象（`LoopFixture` / `EventBox` 的回调注册）、或探针指向自身成员（`Probe::log` / `Probe::exit_count`），按值返回会随移动悬垂/错指；堆上构造一次到位，三个函数按引用收 `Fx&` / `const Fx&`。
7. **异常路径不写 `REQUIRE_THROWS`**（它要触发被测行为，属 Act）：Act 用 `try/catch` 把「是否抛」记进夹具（`fx.threw = true`），Assert 断言它。
8. **编译期断言（`static_assert`）与运行期 `REQUIRE` 同属 Assert 段**，写在 `expect_*` 里；被审查的 `constexpr` 表放**用例作用域的 `constexpr` 局部变量**（`struct Fx` 上方）——lambda 内 `static_assert` 引用它是非 odr 使用，无需捕获（局部类禁有 `static` 数据成员，别往 Fx 里塞 `static constexpr`）。
9. **数据只经夹具流转**，段与段之间不共享用例级局部变量（也就没有捕获顺序问题）。
10. **纯结构重构不改语义**：测试名、标签、断言内容与数量一条不减；改写前后测试数与通过数一致。
11. **注释只留语义**：解释 why 与原理的留（哨兵初值为何反置、断言值的来历、时序/白盒步骤、原测试的业务注释、编译器规则的取舍依据）；与函数名、代码重复的说明删——名字和结构说得清的不写。

## 改写示例（改前 → 改后）

```cpp
// 改前：三段挤在用例体里，行为与断言混排
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

// 改后：三段各有具名函数，夹具承载全部数据流
TEST_CASE("with：拒绝——后续切面与本体不进，已进切面照常 exit", "[with]") {
    struct Fx {
        std::vector<std::string> log;
        Probe p1{&log}, p2{&log}, p3{&log};
        bool body_ran = false;
    };

    auto build_b_rejecting_chain = [] {
        auto fx = std::make_unique<Fx>();
        fx->p2.enter_result = Outcome::kReject;
        return fx;
    };
    auto run_with_rejecting_b = [](Fx& fx) {
        with(probe(fx.p1, "a"), probe(fx.p2, "b"), probe(fx.p3, "c"),
             [&fx] { fx.body_ran = true; });
    };
    auto expect_body_skipped_a_exited = [](const Fx& fx) {
        REQUIRE_FALSE(fx.body_ran);
        REQUIRE(fx.log == std::vector<std::string>{"enter:a", "enter:b", "exit:a"});
    };

    auto fx = build_b_rejecting_chain();
    run_with_rejecting_b(*fx);
    expect_body_skipped_a_exited(*fx);
}
```
