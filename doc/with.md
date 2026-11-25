# 机械性切面（with.h）

> 与业务无关的机械动作的唯一收容处——**纯头文件：没有实现、没有进程、不占模块号**，各模块共同可见（同 [media.md](./media.md) 形态）。**lambda 模拟 AOP around**：业务住 lambda（本体），机械住切面；切面 = 统一 `Aspect` 数据形，`with（切面…, 本体）` 一次平铺、不嵌套。

## 为什么单独立库

- 机械动作（守护、登记/结案、超时挂撤、读单重挂、代跑回流、错误拦截、背压）在各模块复写，改一处漏一处；且带硬顺序（先翻译后结账、完成必撤超时、动堆必重设、用完必 release），靠规则 + 记性维持，漏一处 = 空醒、死条目、槽位泄漏。
- `with` 把顺序变成结构：机械住 enter/exit，**exit 恒执行且恰好一次**——完成 / 超时 / 断连 / 取消几条收尾路的竞态收敛到同一 exit。
- 切面成数据（`Aspect`）：可存表、可按 `name` 观测计数；策略值以行指针挂进 state（见 [policy.md](./policy.md)），机制与数据分家。

## 契约（with.h）

```cpp
// ===== with.h：切面的数据形 =====

enum class Outcome { kEnter, kReject };      // enter 的裁决：进本体 / 拒绝

struct Aspect {
    void*        state;          // 机械状态：账本指针 / 句柄 / 策略行指针（policy.h）；业务数据禁入
    Outcome    (*enter)(void*);  // 前置机械，按列出顺序（外 → 内）；kReject → 后续切面与本体都不进
    void       (*exit)(void*);   // 后置机械，逆序（内 → 外），恒执行且恰好一次
    const char*  name;           // 观测/审查用
};

// with（切面…, 本体）：末位 = 业务 lambda，其余 = Aspect 值，一次平铺列出
template <class... Ts> decltype(auto) with(Ts&&... ts);
//   enter 按序（外 → 内），exit 逆序（内 → 外）；拒绝 → 走拒绝处置，本体不跑

with(live(ip), ticket（ip.pending, e, kOpIpcRequest, ip.timer, 2s, 超时模板）, [&](auto& e) { … });

// 分流表（with 的第二形）：按 type 查表分派，查无 → 丢弃并计数
struct HandlerRow { EventType type; void (*fn)(void* ctx, const Event&); };
```

## 工厂签名

```cpp
// 全部返回 Aspect；行类别对 = 形参类型固定（错挂编译不过）；表/条目类型、句柄非零走 requires/static_assert
template <class R>  concept Stoppable = requires (const R& r) { { r.stopped() } -> std::convertible_to<bool>; };
template <class T>  concept Ledger    = requires { typename T::entry_type; typename T::id_type; };

template <Stoppable R>           Aspect live(R& x);
template <Ledger Tab>            Aspect ticket(Tab& tab, typename Tab::entry_type e, const OpPolicy& row, Timer& t, uint64_t ns, Event tpl);
template <Ledger Tab, class Key> Aspect settle(Tab& tab, Key&& key, const OpPolicy& row);   // Key = 单号，01 票根形 = 条目引用
                                 Aspect rearm(OpCtx& op);
                                 Aspect defer(Dispatcher& d);
                                 Aspect intercept(const ErrPolicy& row);
                                 Aspect backpressure(Dispatcher& d, const EventDef& row);
                                 Aspect block(Ipc&, DataBlockView&);          // 住 02
                                 Aspect heap(Timer&);                         // 住 04
                                 Aspect rpc(Ipc&, PeerId, std::string_view op, const nlohmann::json& args,
                                            PipeId ev_pipe, const OpPolicy& row, uint64_t timeout_ns);
```

- `ticket` 条目按值收、enter 发号（`e.id`、`TimerHandle`，单调不复用），本体 lambda 收登记后条目；`ns` 随行的 `TimeoutSrc` 取值（`kCaller` 实参 / `kSpec` 行解析 / `kNone` 不挂）。

## 标准切面库

全部是**返回 `Aspect` 的工厂**；state / enter / exit / 拒绝一行定死：

| 工厂 | state | enter | exit（恒一次） | 拒绝 | 收编自 |
|---|---|---|---|---|---|
| `live(x)` | 资源指针 | 查 `stopped` | — | 返回 `false`/断言 | 各门面"stop 后一律拒绝" |
| `ticket(tab, e, row, t, ns, tpl)` | 表 / 条目 / `OpPolicy` 行 / 超时句柄 | 登记 + 在飞 + 按行挂超时 | 结案销账 + 撤超时 + 归零才销毁 | 迟到/作废：丢弃并计数 | 01 票根、02 请求表、05 IoTicket、06 任务表 |
| `settle(tab, id, row)` | 表 / 单号 / `OpPolicy` 行 | 查条目 | 汇到 `ticket` 的同一 exit | 落空/作废 = 迟到：丢弃并计数 | 01 收割结案、02 销账、05 完成回流、06 摘单 |
| `rearm(op)` | 常驻读单 | — | 读完成即重挂（每对端/连接/设备一张在飞） | — | 02 UDS、03 socket、04 timerfd、05 边沿 |
| `defer(d)` | `Dispatcher` 指针 | 阻塞本体提交线程池 | 完成 `post()` 回流 | — | 03 connect、05 ioctl、06 ASR/TTS 代跑 |
| `intercept（err 行）` | `ErrPolicy` 行指针 | — | `expected` 落空 → 查行处置，不抛上层 | — | 01 translate、02/03 decode、05/06 推理 |
| `backpressure（d, ev 行）` | `Dispatcher` + `EventDef` 行指针 | 查 `qos` 行（阻塞/丢弃计数/留最新） | 丢弃即计数 | — | 01 投队列、02/03/05 环写 |
| `block(ip, view)` | `Ipc` + 块视图 | — | 语义住 [02](modules/02-ipc.md)（见定义行） | — | 03/05/06 的"用完必须 release" |
| `heap(t)` | `Timer` 指针 | 语义住 [04](modules/04-timer.md)（见定义行） | 语义住 [04](modules/04-timer.md)（见定义行） | — | 04 的"动堆必重设" |
| `rpc(ip, peer, "op", args, ev_pipe, row, timeout)` | 对端 + 报文参数 / `OpPolicy` 行 | 组报文 + 经 02 发请求 | 销账 + 错误码翻译 + 事件翻号段投递 | 发送失败/超时：错误码回填 | 03/06 客户端桩壳 |

> `block` 住 [modules/02-ipc.md](modules/02-ipc.md)、`heap` 住 [modules/04-timer.md](modules/04-timer.md)，其余住 with.h；工厂只依赖参数类型与策略行指针，不反向依赖模块。

## 规则

- **切面准入**：①机械性（与业务无关）②重复 ≥ 2 处——同时满足才进库；业务相关出切面回本体，只写一次的就地写。
- **切面 = 数据**：统一 `Aspect`（state + enter + exit + name），一律工厂产出、不收裸函数对；state 禁业务数据（业务数据住 lambda 捕获）。
- **工厂参数审查**：新切面必出工厂 + `requires`/`static_assert`（策略行类别对、表/条目类型匹配、句柄非零），错挂错配编译不过。
- **顺序条款结构化**：先翻译后结账、完成必撤超时、动堆必重设、用完必 release、读完成必重挂、stop 后拒绝——全由切面吸收，模块规则改指切面、不再罗列步骤。
- **注释单点**：机械语义只在切面定义处写一次（with.h 切面看上表 enter/exit/拒绝列，`block`/`heap` 看 02/04 定义行，别处只留索引）；调用点的业务注释只答"做什么"，最多带一个切面名当索引（形如 `// ticket：登记本单`），禁复述顺序、收尾、计数条款——语义改动只动切面定义，调用点注释不腐。
- **切面查表不判策略**：策略值以行指针挂 `Aspect.state`（`backpressure`→`EventDef`、`ticket`/`settle`→`OpPolicy`、`intercept`→`ErrPolicy`，见 [policy.md](./policy.md)）；**行指针一律显式作参**——查行的切面签名必收行（错类别编译不过），工厂内不隐式查注册表，调用点传哪个行一眼可见。
- **平铺即组合**：多切面一次平铺（形如 `with(ticket(…), settle(…), lambda)`），禁嵌套；仅当切面参数依赖本体产出的量（背压要等事件造出）才就地二起 `with`——该点收进内脏编排函数（如 01 的 `enqueue_event`）。
- **与 RAII / 纯函数分工**：RAII 管资源生死，`with` 管一次动作的前后置；判定类内脏（`should_drop`、`translate`）仍是纯函数、被切面调用。
- **切面只增机械**：切面里长出业务分支即出库；演进只许收紧顺序与计数（演进防火墙）。
