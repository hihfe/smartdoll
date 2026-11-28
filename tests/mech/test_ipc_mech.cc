// 02 机制测：环回建连首挂读单、notify 单向、request↔respond 对账、超时合成 + 迟到拒收、
// 断连 kBroken 回填 + kTypeIpcBroken、坏 magic 断连、超大负载 kTooLarge、stop 摘表拒新单、
// 数据面 write_data → kTypeIpcDataReady → release 槽位复用、seq 跳号 → kTypeIpcDataLost。
// 全部用例为 Arrange-Act-Assert 三段式，每段一个具名函数（规范见 doc/testing.md）。
#include <catch2/catch_test_macros.hpp>

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/fixture.h"
#include "ipc_internal.h"

using namespace doll::test;

namespace {

std::atomic<int> g_sock{0};

// 环回 harness：一个 Ipc 同时 listen + connect（双端同进程，走全量真实链路——
// io_uring 读写、UDS 分帧、请求表、eventfd 唤醒都真跑）。
// 构造即建连到位（listen → connect → 等双端落位），故各用例 Arrange 只需 new 它一次。
struct IpcHarness {
    LoopFixture f;
    TimerPtr t;
    IpcPtr ip;
    EventBox box;
    std::string path;
    PeerId peer = 0;          // 客户端对端（connect 返回；服务端由 acceptor 线程落位）
    bool peers_up = false;    // 双端建连成功（connect 返回非零 + 两 PeerRecord 入表）

    IpcHarness() : t(tmr::create(*f.d)), ip(ipc::create(*f.d, *t)), box(*f.d) {
        path = "/tmp/doll-ipc-" + std::to_string(::getpid()) + "-" +
               std::to_string(g_sock.fetch_add(1)) + ".sock";
        ipc::listen(*ip, path, box.pipe());   // 双端共用同一条 events 管道（一个 box 收全部）
        peer = ipc::connect(*ip, path, box.pipe());
        peers_up = wait_until([&] { return ipc::peer_ids(*ip).size() == 2; }, ms(2000));
    }
    ~IpcHarness() {
        ipc::stop(*ip);
        ::unlink(path.c_str());
    }

    // 取第 skip 个指定类型的事件
    std::optional<Event> find(EventType t, size_t skip = 0) const {
        size_t n = 0;
        for (auto& e : box.all())
            if (e.type == t && n++ == skip) return e;
        return std::nullopt;
    }
};

std::vector<std::byte> bytes_of(const std::string& s) {
    std::vector<std::byte> v(s.size());
    for (size_t i = 0; i < s.size(); ++i) v[i] = std::byte{uint8_t(s[i])};
    return v;
}

}  // namespace

TEST_CASE("02：listen/connect 环回建连，双端各首挂读单", "[02]") {
    struct Fx {
        std::unique_ptr<IpcHarness> h;      // 构造即 listen + connect + 等双端
        std::vector<PeerId> ids;            // 存活对端清单（首挂读单入表后才可见）
    };

    // Arrange：环回 harness 一次到位
    auto build_loopback_pair = [] {
        auto fx = std::make_unique<Fx>();
        fx->h = std::make_unique<IpcHarness>();
        return fx;
    };
    // Act：取存活对端清单
    auto run_collect_live_peers = [](Fx& fx) {
        fx.ids = ipc::peer_ids(*fx.h->ip);
    };
    // Assert：客户端号非零、双端建连、两个互异对端各据一条连接
    auto expect_two_distinct_live_peers = [](const Fx& fx) {
        REQUIRE(fx.h->peer != 0);
        REQUIRE(fx.h->peers_up);
        REQUIRE(fx.ids.size() == 2);
        REQUIRE(fx.ids[0] != fx.ids[1]);
    };

    auto fx = build_loopback_pair();
    run_collect_live_peers(*fx);
    expect_two_distinct_live_peers(*fx);
}

TEST_CASE("02：notify 单向送达 events 管道（request_id = 0）", "[02]") {
    struct Fx {
        std::unique_ptr<IpcHarness> h;
        IpcMessage msg{};
        bool delivered = false;              // 初值 false：Act 没跑到就过不了断言
        std::optional<Event> ev;
        PeerId from = 0;                     // 初值 0：过不了 != 0
        IpcMessageView view{};
        size_t pending_after = 1;            // 初值 1：过不了 == 0
    };

    // Arrange：harness + 待发通知
    auto build_notify_envelope = [] {
        auto fx = std::make_unique<Fx>();
        fx->h = std::make_unique<IpcHarness>();
        fx->msg.body = "hello-notify";
        return fx;
    };
    // Act：发通知 → 等送达 → 取事件解视图，观察记进夹具
    auto run_send_notify_and_capture_delivery = [](Fx& fx) {
        ipc::notify(*fx.h->ip, fx.h->peer, fx.msg);
        fx.delivered = fx.h->box.wait_type(kTypeIpcNotify, ms(2000));
        fx.ev = fx.h->find(kTypeIpcNotify);
        if (fx.ev) {
            fx.from = fx.ev->source;
            fx.view = std::any_cast<IpcMessageView>(fx.ev->payload);
        }
        fx.pending_after = ipc::pending_count(*fx.h->ip);
    };
    // Assert：单向送达、id = 0（不登记不等待）、事件不进请求表
    auto expect_notify_delivered_without_registration = [](const Fx& fx) {
        REQUIRE(fx.h->peers_up);
        REQUIRE(fx.delivered);
        REQUIRE(fx.ev.has_value());
        REQUIRE(fx.from != 0);
        REQUIRE(fx.view.head.type == uint16_t(EventType(kTypeIpcNotify)));
        REQUIRE(fx.view.head.request_id == 0u);
        REQUIRE(fx.view.body == "hello-notify");
        REQUIRE(fx.pending_after == 0u);
    };

    auto fx = build_notify_envelope();
    run_send_notify_and_capture_delivery(*fx);
    expect_notify_delivered_without_registration(*fx);
}

TEST_CASE("02：request→respond 往返，request_id 对账 + error_code/body 透传", "[02]") {
    struct Fx {
        std::unique_ptr<IpcHarness> h;
        IpcMessage req{};
        IpcMessage resp{};
        RequestId id = 0;
        size_t pending_after_request = 0;    // 初值 0：过不了 == 1
        bool request_seen = false;
        std::optional<Event> in;
        IpcMessageView in_view{};
        bool response_seen = false;
        std::optional<Event> out;
        IpcMessageView out_view{};
        size_t pending_after_response = 1;   // 初值 1：过不了 == 0
    };

    // Arrange：harness + 请求与响应报文
    auto build_request_response_pair = [] {
        auto fx = std::make_unique<Fx>();
        fx->h = std::make_unique<IpcHarness>();
        fx->req.body = R"({"op":"ping"})";
        fx->resp.body = R"({"ok":1})";
        return fx;
    };
    // Act：发请求 → 等入站 → 回响应 → 等回执，每步观察记进夹具
    auto run_request_then_respond_roundtrip = [](Fx& fx) {
        fx.id = ipc::request(*fx.h->ip, fx.h->peer, fx.req, fx.h->box.pipe(),
                             2'000'000'000ull);
        fx.pending_after_request = ipc::pending_count(*fx.h->ip);

        fx.request_seen = fx.h->box.wait_type(kTypeIpcRequest, ms(2000));
        fx.in = fx.h->find(kTypeIpcRequest);
        if (fx.in) {
            fx.in_view = std::any_cast<IpcMessageView>(fx.in->payload);
            ipc::respond(*fx.h->ip, fx.in->source, fx.id, fx.resp,
                         int32_t(IpcError::kOk));   // 回给收到的那端
        }

        fx.response_seen = fx.h->box.wait_type(kTypeIpcResponse, ms(2000));
        fx.out = fx.h->find(kTypeIpcResponse);
        if (fx.out) fx.out_view = std::any_cast<IpcMessageView>(fx.out->payload);
        fx.pending_after_response = ipc::pending_count(*fx.h->ip);
    };
    // Assert：线上号 = 发出号、错误码与负载原样透传、settle 销账
    auto expect_roundtrip_reconciles_by_request_id = [](const Fx& fx) {
        REQUIRE(fx.h->peers_up);
        REQUIRE(fx.id != 0);
        REQUIRE(fx.pending_after_request == 1u);              // ticket 登记在飞
        REQUIRE(fx.request_seen);
        REQUIRE(fx.in.has_value());
        REQUIRE(fx.in_view.head.request_id == fx.id);
        REQUIRE(fx.in_view.head.type == uint16_t(EventType(kTypeIpcRequest)));
        REQUIRE(fx.in_view.body == R"({"op":"ping"})");
        REQUIRE(fx.response_seen);
        REQUIRE(fx.out.has_value());
        REQUIRE(fx.out_view.head.request_id == fx.id);
        REQUIRE(fx.out_view.head.error_code == 0);
        REQUIRE(fx.out_view.body == R"({"ok":1})");
        REQUIRE(fx.pending_after_response == 0u);             // settle 销账
    };

    auto fx = build_request_response_pair();
    run_request_then_respond_roundtrip(*fx);
    expect_roundtrip_reconciles_by_request_id(*fx);
}

TEST_CASE("02：请求超时合成 kTimeout 响应（与响应同形）；迟到响应 settle 拒收计数", "[02]") {
    struct Fx {
        std::unique_ptr<IpcHarness> h;
        IpcMessage req{};
        RequestId id = 0;
        bool response_seen = false;
        std::optional<Event> ev;
        IpcMessageView view{};
        PeerId ev_source = 1;                // 初值 1：过不了 == 0（合成事件 source 保留值）
        size_t pending_after_timeout = 1;    // 初值 1：过不了 == 0
        size_t late_before = 0;
        bool late_counted = false;
        size_t late_after = 0;
    };

    // Arrange：harness + 永不作答的请求
    auto build_unanswered_request = [] {
        auto fx = std::make_unique<Fx>();
        fx->h = std::make_unique<IpcHarness>();
        fx->req.body = "never-answered";
        return fx;
    };
    // Act：发请求等超时回执 → 记账 → 补发迟到响应 → 等拒收计数上涨
    auto run_timeout_then_send_late_response = [](Fx& fx) {
        fx.id = ipc::request(*fx.h->ip, fx.h->peer, fx.req, fx.h->box.pipe(), 50'000'000);
        fx.response_seen = fx.h->box.wait_type(kTypeIpcResponse, ms(2000));
        fx.ev = fx.h->find(kTypeIpcResponse);
        if (fx.ev) {
            fx.view = std::any_cast<IpcMessageView>(fx.ev->payload);
            fx.ev_source = fx.ev->source;
        }
        fx.pending_after_timeout = ipc::pending_count(*fx.h->ip);

        fx.late_before = ipc::late_response_count(*fx.h->ip);
        IpcMessage late;
        late.body = "too-late";
        ipc::respond(*fx.h->ip, fx.h->peer, fx.id, late, int32_t(IpcError::kOk));
        fx.late_counted = wait_until(
            [&] { return ipc::late_response_count(*fx.h->ip) > fx.late_before; }, ms(2000));
        fx.late_after = ipc::late_response_count(*fx.h->ip);
    };
    // Assert：与响应同形的 kTimeout、超时即销账；迟到响应查表落空被拒收且只计一次
    auto expect_timeout_response_and_late_rejection = [](const Fx& fx) {
        REQUIRE(fx.h->peers_up);
        REQUIRE(fx.response_seen);
        REQUIRE(fx.ev.has_value());
        REQUIRE(fx.view.head.request_id == fx.id);
        REQUIRE(fx.view.head.error_code == int32_t(IpcError::kTimeout));
        REQUIRE(fx.ev_source == 0);                          // 合成事件 source 保留值 0
        REQUIRE(fx.pending_after_timeout == 0u);             // 超时销账（撤表）
        REQUIRE(fx.late_counted);
        REQUIRE(fx.late_after == fx.late_before + 1);        // 迟到 settle 拒收并计数
    };

    auto fx = build_unanswered_request();
    run_timeout_then_send_late_response(*fx);
    expect_timeout_response_and_late_rejection(*fx);
}

TEST_CASE("02：断连 → 未决请求 kBroken 回填 + kTypeIpcBroken 上报", "[02]") {
    struct Fx {
        std::unique_ptr<IpcHarness> h;
        IpcMessage req{};
        RequestId id = 0;
        bool request_seen = false;
        size_t pending_after_request = 0;    // 初值 0：过不了 == 1
        bool broken_seen = false;
        std::optional<Event> br;
        PeerId broken_peer = 0;              // 初值 0：过不了 != 0
        bool response_seen = false;
        std::optional<Event> ev;
        IpcMessageView view{};
        size_t pending_after_kill = 1;       // 初值 1：过不了 == 0
    };

    // Arrange：harness + 挂着不答的请求
    auto build_outstanding_request = [] {
        auto fx = std::make_unique<Fx>();
        fx->h = std::make_unique<IpcHarness>();
        fx->req.body = "waiting-for-forever";
        return fx;
    };
    // Act：发请求 → 等对端收到 → 模拟对端退出 → 等断连上报与 kBroken 回执
    auto run_kill_peer_with_outstanding_request = [](Fx& fx) {
        fx.id = ipc::request(*fx.h->ip, fx.h->peer, fx.req, fx.h->box.pipe(),
                             10'000'000'000ull);
        fx.request_seen = fx.h->box.wait_type(kTypeIpcRequest, ms(2000));   // 对端已收到（未回）
        fx.pending_after_request = ipc::pending_count(*fx.h->ip);

        ipc::kill_peer(*fx.h->ip, fx.h->peer);   // 模拟对端进程退出：shutdown → 双端 EOF

        fx.broken_seen = fx.h->box.wait_type(kTypeIpcBroken, ms(2000));
        fx.br = fx.h->find(kTypeIpcBroken);
        if (fx.br) fx.broken_peer = std::any_cast<IpcBrokenPayload>(fx.br->payload).peer;

        fx.response_seen = fx.h->box.wait_type(kTypeIpcResponse, ms(2000));
        fx.ev = fx.h->find(kTypeIpcResponse);
        if (fx.ev) fx.view = std::any_cast<IpcMessageView>(fx.ev->payload);
        fx.pending_after_kill = ipc::pending_count(*fx.h->ip);
    };
    // Assert：断连事件带对端号，未决请求以 kBroken 回填并销账
    auto expect_broken_report_and_pending_backfilled = [](const Fx& fx) {
        REQUIRE(fx.h->peers_up);
        REQUIRE(fx.request_seen);
        REQUIRE(fx.pending_after_request == 1u);
        REQUIRE(fx.broken_seen);
        REQUIRE(fx.br.has_value());
        REQUIRE(fx.br->def == &kTypeIpcBroken);
        REQUIRE(fx.broken_peer != 0);
        REQUIRE(fx.response_seen);
        REQUIRE(fx.ev.has_value());
        REQUIRE(fx.view.head.request_id == fx.id);
        REQUIRE(fx.view.head.error_code == int32_t(IpcError::kBroken));
        REQUIRE(fx.pending_after_kill == 0u);
    };

    auto fx = build_outstanding_request();
    run_kill_peer_with_outstanding_request(*fx);
    expect_broken_report_and_pending_backfilled(*fx);
}

TEST_CASE("02：坏 magic 断连（帧头校验失败，不投递）", "[02]") {
    struct Fx {
        std::unique_ptr<IpcHarness> h;
        std::array<char, sizeof(IpcHeader)> raw{};   // 原始字节：指纹故意写错
        int fd = -1;
        ssize_t wrote = -1;                          // 初值 -1：过不了 == raw.size()
        bool broken_seen = false;
        size_t request_events = 1;                   // 初值 1：过不了 == 0（坏帧不投业务）
    };

    // Arrange：harness + 指纹写错的 32B 原始头
    auto build_bad_magic_frame = [] {
        auto fx = std::make_unique<Fx>();
        fx->h = std::make_unique<IpcHarness>();
        IpcHeader h{};
        h.magic = 0xdeadbeef;
        h.version = kIpcVersion;
        h.type = uint16_t(EventType(kTypeIpcRequest));
        std::memcpy(fx->raw.data(), &h, sizeof h);
        return fx;
    };
    // Act：取对端 fd 直写坏帧 → 等断连上报 → 数入站请求数
    auto run_inject_bad_magic_and_wait_break = [](Fx& fx) {
        fx.fd = ipc::peer_fd(*fx.h->ip, fx.h->peer);
        if (fx.fd >= 0)
            fx.wrote = ::write(fx.fd, fx.raw.data(), fx.raw.size());
        fx.broken_seen = fx.h->box.wait_type(kTypeIpcBroken, ms(2000));
        fx.request_events = fx.h->box.count(kTypeIpcRequest);
    };
    // Assert：坏帧触发断连，且一个业务请求都不投
    auto expect_break_without_business_delivery = [](const Fx& fx) {
        REQUIRE(fx.h->peers_up);
        REQUIRE(fx.fd >= 0);
        REQUIRE(fx.wrote == ssize_t(fx.raw.size()));
        REQUIRE(fx.broken_seen);
        REQUIRE(fx.request_events == 0u);
    };

    auto fx = build_bad_magic_frame();
    run_inject_bad_magic_and_wait_break(*fx);
    expect_break_without_business_delivery(*fx);
}

TEST_CASE("02：超 kMaxPayload 请求回填 kTooLarge，不发不等超时", "[02]") {
    struct Fx {
        std::unique_ptr<IpcHarness> h;
        IpcMessage req{};
        RequestId id = 0;
        bool response_seen = false;
        std::optional<Event> ev;
        IpcMessageView view{};
        size_t pending_after = 1;             // 初值 1：过不了 == 0
        size_t request_events = 1;            // 初值 1：过不了 == 0（对端根本没收到）
    };

    // Arrange：harness + 超上限负载（kMaxPayload + 1）
    auto build_oversize_request = [] {
        auto fx = std::make_unique<Fx>();
        fx->h = std::make_unique<IpcHarness>();
        fx->req.body = std::string(kMaxPayload + 1, 'x');
        return fx;
    };
    // Act：发请求 → 等即时回执 → 记账与入站请求数
    auto run_send_oversize_and_capture_rejection = [](Fx& fx) {
        fx.id = ipc::request(*fx.h->ip, fx.h->peer, fx.req, fx.h->box.pipe(),
                             5'000'000'000ull);
        fx.response_seen = fx.h->box.wait_type(kTypeIpcResponse, ms(2000));
        fx.ev = fx.h->find(kTypeIpcResponse);
        if (fx.ev) fx.view = std::any_cast<IpcMessageView>(fx.ev->payload);
        fx.pending_after = ipc::pending_count(*fx.h->ip);
        fx.request_events = fx.h->box.count(kTypeIpcRequest);
    };
    // Assert：立即回 kTooLarge 并销账，且帧根本没出（不等超时）
    auto expect_too_large_filled_immediately = [](const Fx& fx) {
        REQUIRE(fx.h->peers_up);
        REQUIRE(fx.id != 0);
        REQUIRE(fx.response_seen);
        REQUIRE(fx.ev.has_value());
        REQUIRE(fx.view.head.request_id == fx.id);
        REQUIRE(fx.view.head.error_code == int32_t(IpcError::kTooLarge));
        REQUIRE(fx.pending_after == 0u);                     // 立即销账（发送失败即销账同族）
        REQUIRE(fx.request_events == 0u);
    };

    auto fx = build_oversize_request();
    run_send_oversize_and_capture_rejection(*fx);
    expect_too_large_filled_immediately(*fx);
}

TEST_CASE("02：stop 摘请求表 kBroken 回填，之后门面一律拒绝", "[02]") {
    struct Fx {
        std::unique_ptr<IpcHarness> h;
        IpcMessage req{};
        IpcMessage probe{};
        RequestId id = 0;
        bool request_seen = false;
        bool response_seen = false;
        std::optional<Event> ev;
        IpcMessageView view{};
        size_t pending_after_stop = 1;       // 初值 1：过不了 == 0
        bool peers_empty = false;
        RequestId rejected_request = 1;      // 初值 1：过不了 == 0（live 拒绝）
        PeerId rejected_connect = 1;         // 初值 1：过不了 == 0（live 拒绝）
        bool rejected_write = true;          // 初值 true：过不了 REQUIRE_FALSE
    };

    // Arrange：harness + 挂着的请求 + 探针报文
    auto build_request_then_stop_scenario = [] {
        auto fx = std::make_unique<Fx>();
        fx->h = std::make_unique<IpcHarness>();
        fx->req.body = "outstanding";
        fx->probe.body = "x";
        return fx;
    };
    // Act：发请求 → 等入站 → stop → 等 kBroken 回执 → 再打各门面验 live 拒绝
    auto run_stop_then_probe_every_facade = [](Fx& fx) {
        fx.id = ipc::request(*fx.h->ip, fx.h->peer, fx.req, fx.h->box.pipe(),
                             30'000'000'000ull);
        fx.request_seen = fx.h->box.wait_type(kTypeIpcRequest, ms(2000));

        ipc::stop(*fx.h->ip);

        fx.response_seen = fx.h->box.wait_type(kTypeIpcResponse, ms(2000));
        fx.ev = fx.h->find(kTypeIpcResponse);
        if (fx.ev) fx.view = std::any_cast<IpcMessageView>(fx.ev->payload);
        fx.pending_after_stop = ipc::pending_count(*fx.h->ip);
        fx.peers_empty = ipc::peer_ids(*fx.h->ip).empty();    // 连接全关

        fx.rejected_request = ipc::request(*fx.h->ip, fx.h->peer, fx.probe,
                                           fx.h->box.pipe(), 1'000'000);
        fx.rejected_connect = ipc::connect(*fx.h->ip, fx.h->path, fx.h->box.pipe());
        fx.rejected_write = ipc::write_data(*fx.h->ip, fx.h->peer, bytes_of("x"));
        ipc::notify(*fx.h->ip, fx.h->peer, fx.probe);         // 拒绝即静默（不发不炸）
        ipc::respond(*fx.h->ip, fx.h->peer, fx.id, fx.probe, 0);
    };
    // Assert：停机摘表回填 kBroken，此后 request/connect/write 一律被 live 拒绝
    auto expect_stop_backfills_and_rejects_new_calls = [](const Fx& fx) {
        REQUIRE(fx.h->peers_up);
        REQUIRE(fx.request_seen);
        REQUIRE(fx.response_seen);
        REQUIRE(fx.ev.has_value());
        REQUIRE(fx.view.head.request_id == fx.id);
        REQUIRE(fx.view.head.error_code == int32_t(IpcError::kBroken));
        REQUIRE(fx.pending_after_stop == 0u);
        REQUIRE(fx.peers_empty);
        REQUIRE(fx.rejected_request == 0);
        REQUIRE(fx.rejected_connect == 0);
        REQUIRE_FALSE(fx.rejected_write);
    };

    auto fx = build_request_then_stop_scenario();
    run_stop_then_probe_every_facade(*fx);
    expect_stop_backfills_and_rejects_new_calls(*fx);
}

TEST_CASE("02：write_data 零拷贝送达 kTypeIpcDataReady，release 归还槽位", "[02]") {
    struct Fx {
        std::unique_ptr<IpcHarness> h;
        std::string payload;
        bool wrote_first = false;
        bool ready_seen = false;
        std::optional<Event> ev;
        DataBlockView v{};
        int cmp = -1;                        // memcmp 结果（0 = 保真）
        bool wrote_second = false;
        bool two_ready = false;
        std::optional<Event> ev2;
        DataBlockView v2{};
    };

    // Arrange：harness + 一块负载
    auto build_audio_block_scenario = [] {
        auto fx = std::make_unique<Fx>();
        fx->h = std::make_unique<IpcHarness>();
        fx->payload = "PCM-BLOCK-0001";
        return fx;
    };
    // Act：写块 → 等 DataReady 解视图 → release → 再写一块验槽位复用
    auto run_write_release_then_reuse_slot = [](Fx& fx) {
        fx.wrote_first = ipc::write_data(*fx.h->ip, fx.h->peer, bytes_of(fx.payload));
        fx.ready_seen = fx.h->box.wait_type(kTypeIpcDataReady, ms(2000));
        fx.ev = fx.h->find(kTypeIpcDataReady);
        if (fx.ev) {
            fx.v = std::any_cast<DataBlockView>(fx.ev->payload);
            fx.cmp = fx.v.data ? std::memcmp(fx.v.data, fx.payload.data(), fx.v.len) : -1;
        }
        ipc::release(*fx.h->ip, fx.v);                       // 归还即 consume → 槽位复用

        fx.wrote_second = ipc::write_data(*fx.h->ip, fx.h->peer, bytes_of(fx.payload));
        fx.two_ready = fx.h->box.wait_count(2, ms(2000));
        fx.ev2 = fx.h->find(kTypeIpcDataReady, 1);
        if (fx.ev2) fx.v2 = std::any_cast<DataBlockView>(fx.ev2->payload);
        ipc::release(*fx.h->ip, fx.v2);
    };
    // Assert：块零拷贝保真、环号/来源齐全，release 后仍写得进且 seq 不回卷
    auto expect_block_delivered_and_slot_reused = [](const Fx& fx) {
        REQUIRE(fx.h->peers_up);
        REQUIRE(fx.wrote_first);
        REQUIRE(fx.ready_seen);
        REQUIRE(fx.ev.has_value());
        REQUIRE(fx.ev->def == &kTypeIpcDataReady);
        REQUIRE(fx.ev->source == PeerId(fx.h->peer));
        REQUIRE(fx.v.len == fx.payload.size());
        REQUIRE(fx.cmp == 0);                                // 零拷贝直达共享内存
        REQUIRE(fx.v.from == fx.h->peer);
        REQUIRE(fx.v.ring != 0);
        REQUIRE(fx.v.seq == 0u);
        REQUIRE(fx.wrote_second);
        REQUIRE(fx.two_ready);
        REQUIRE(fx.ev2.has_value());
        REQUIRE(fx.v2.seq == 1u);                             // 槽位复用不回卷 seq
    };

    auto fx = build_audio_block_scenario();
    run_write_release_then_reuse_slot(*fx);
    expect_block_delivered_and_slot_reused(*fx);
}

TEST_CASE("02：seq 跳号 → kTypeIpcDataLost（崩溃重建对账）", "[02]") {
    struct Fx {
        std::unique_ptr<IpcHarness> h;
        bool wrote_first = false;
        bool first_ready = false;
        DataBlockView v1{};
        bool wrote_second = false;
        bool lost_seen = false;
        std::optional<Event> lost;
        IpcLostPayload lp{};
        bool second_ready = false;
        std::optional<Event> rd;
        DataBlockView v2{};
    };

    // Arrange：harness（首块与跳号后的块在 Act 依次写）
    auto build_seq_gap_pair = [] {
        auto fx = std::make_unique<Fx>();
        fx->h = std::make_unique<IpcHarness>();
        return fx;
    };
    // Act：首块入环消费（立对账基准）→ 白盒跳号 → 再写 → 等 DataLost 与后续 DataReady
    auto run_skip_seq_then_capture_gap = [](Fx& fx) {
        fx.wrote_first = ipc::write_data(*fx.h->ip, fx.h->peer, bytes_of("block-0"));
        fx.first_ready = fx.h->box.wait_type(kTypeIpcDataReady, ms(2000));
        auto e1 = fx.h->find(kTypeIpcDataReady);
        if (e1) fx.v1 = std::any_cast<DataBlockView>(e1->payload);
        ipc::release(*fx.h->ip, fx.v1);                       // 基准：expect 已推进到 1

        ipc::skip_ring_seq(*fx.h->ip, fx.h->peer, 2);         // 白盒：下一 seq = 3（丢了 1、2）

        fx.wrote_second = ipc::write_data(*fx.h->ip, fx.h->peer, bytes_of("block-3"));
        fx.lost_seen = fx.h->box.wait_type(kTypeIpcDataLost, ms(2000));
        fx.lost = fx.h->find(kTypeIpcDataLost);
        if (fx.lost) fx.lp = std::any_cast<IpcLostPayload>(fx.lost->payload);

        fx.second_ready = wait_until(              // 按类型计数等第二件：wait_type 会被首块的旧 DataReady 提前满足
            [&] { return fx.h->box.count(kTypeIpcDataReady) >= 2; }, ms(2000));
        fx.rd = fx.h->find(kTypeIpcDataReady, 1);
        if (fx.rd) fx.v2 = std::any_cast<DataBlockView>(fx.rd->payload);
        ipc::release(*fx.h->ip, fx.v2);
    };
    // Assert：丢块区间 [1, 3) 投 deliver 管道，跳号不挡后续块投递
    auto expect_gap_range_reported_and_flow_continues = [](const Fx& fx) {
        REQUIRE(fx.h->peers_up);
        REQUIRE(fx.wrote_first);
        REQUIRE(fx.first_ready);
        REQUIRE(fx.wrote_second);
        REQUIRE(fx.lost_seen);
        REQUIRE(fx.lost.has_value());
        REQUIRE(fx.lost->def == &kTypeIpcDataLost);
        REQUIRE(fx.lost->pipe == fx.h->box.pipe());
        REQUIRE(fx.lp.ring != 0);
        REQUIRE(fx.lp.from == 1u);                            // 丢块区间 [1, 3)
        REQUIRE(fx.lp.to == 3u);
        REQUIRE(fx.second_ready);
        REQUIRE(fx.rd.has_value());
        REQUIRE(fx.v2.seq == 3u);
    };

    auto fx = build_seq_gap_pair();
    run_skip_seq_then_capture_gap(*fx);
    expect_gap_range_reported_and_flow_continues(*fx);
}
