// 02 纯函数单测（白盒：ipc_internal.h）：头校验、帧编解码、消息翻事件、超时合成、seq 对账，
// 以及 ShmRing 环薄层（put/peek/consume 顺序、peek 不摘、槽位复用、环满、seq 连续、跨尾填充、唤醒）。
// 全部用例为 Arrange-Act-Assert 三段式，每段一个具名函数（规范见 doc/testing.md）。
#include <catch2/catch_test_macros.hpp>

#include <poll.h>
#include <unistd.h>

#include <any>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ipc_internal.h"

namespace {

std::vector<std::byte> bytes_of(std::string_view s) {
    std::vector<std::byte> v(s.size());
    for (size_t i = 0; i < s.size(); ++i) v[i] = std::byte{uint8_t(s[i])};
    return v;
}

IpcHeader good_header() {
    return IpcHeader{kIpcMagic, kIpcVersion, uint16_t(EventType(kTypeIpcRequest)),
                     42, 0, 0, 7};
}

// eventfd 探测：poll 不破坏计数（efd 阻塞形——01 读单在内核睡等，测试不能 read 干等）
bool wake_pending(int fd) {
    pollfd p{fd, POLLIN, 0};
    return ::poll(&p, 1, 0) == 1 && (p.revents & POLLIN);
}

uint64_t drain_wake(int fd) {
    uint64_t v = 0;
    return ::read(fd, &v, sizeof v) == ssize_t(sizeof v) ? v : 0;
}

}  // namespace

// ===== 纯函数：协议判定 =====

TEST_CASE("02：check_header 头校验", "[02]") {
    struct Fx {
        IpcHeader good{};
        IpcHeader bad_magic{};
        IpcHeader bad_version{};
        IpcHeader type_below{};
        IpcHeader type_above{};
        IpcHeader at_limit{};
        IpcHeader over_limit{};
        // 各头的校验判定（缺省 kOk：Act 没跑，坏帧的断言就会撞上失败）
        IpcError good_verdict = IpcError::kOk;
        IpcError bad_magic_verdict = IpcError::kOk;
        IpcError bad_version_verdict = IpcError::kOk;
        IpcError type_below_verdict = IpcError::kOk;
        IpcError type_above_verdict = IpcError::kOk;
        IpcError at_limit_verdict = IpcError::kOk;
        IpcError over_limit_verdict = IpcError::kOk;
    };

    auto build_header_variant_matrix = [] {
        auto fx = std::make_unique<Fx>();
        fx->good = good_header();
        fx->bad_magic = good_header();
        fx->bad_magic.magic ^= 1;
        fx->bad_version = good_header();
        fx->bad_version.version = uint16_t(kIpcVersion + 1);
        fx->type_below = good_header();
        fx->type_below.type = uint16_t(kIpc) - 1;
        fx->type_above = good_header();
        fx->type_above.type = uint16_t(kIpc) + 100;
        fx->at_limit = good_header();
        fx->at_limit.payload_len = kMaxPayload;
        fx->over_limit = good_header();
        fx->over_limit.payload_len = kMaxPayload + 1;
        return fx;
    };
    auto run_header_validation_matrix = [](Fx& fx) {
        fx.good_verdict = ipc::check_header(fx.good);
        fx.bad_magic_verdict = ipc::check_header(fx.bad_magic);
        fx.bad_version_verdict = ipc::check_header(fx.bad_version);
        fx.type_below_verdict = ipc::check_header(fx.type_below);
        fx.type_above_verdict = ipc::check_header(fx.type_above);
        fx.at_limit_verdict = ipc::check_header(fx.at_limit);
        fx.over_limit_verdict = ipc::check_header(fx.over_limit);
    };
    auto expect_verdicts_for_each_header = [](const Fx& fx) {
        REQUIRE(fx.good_verdict == IpcError::kOk);
        REQUIRE(fx.bad_magic_verdict == IpcError::kBadMagic);        // 指纹不符
        REQUIRE(fx.bad_version_verdict == IpcError::kUnsupported);
        REQUIRE(fx.type_below_verdict == IpcError::kUnsupported);    // 号段外
        REQUIRE(fx.type_above_verdict == IpcError::kUnsupported);
        REQUIRE(fx.at_limit_verdict == IpcError::kOk);               // 恰上限可收
        REQUIRE(fx.over_limit_verdict == IpcError::kTooLarge);       // 超限先拒（decode 不为其分配）
    };

    auto fx = build_header_variant_matrix();
    run_header_validation_matrix(*fx);
    expect_verdicts_for_each_header(*fx);
}

TEST_CASE("02：encode→decode 往返（盖章 + 二进制负载保真）", "[02]") {
    struct Fx {
        IpcMessage msg{};
        size_t frame_size = 0;
        bool decoded = false;
        IpcHeader got_head{};      // 解出的头（没解出时零值）
        std::string got_body;      // 解出的负载
    };

    auto build_embedded_nul_message = [] {
        auto fx = std::make_unique<Fx>();
        fx->msg.head = IpcHeader{0, 0, uint16_t(EventType(kTypeIpcNotify)), 0, 0, 0, 3};
        fx->msg.body = std::string("a\0b", 3);   // 内嵌 NUL：负载是字节不是 C 串
        return fx;
    };
    auto run_encode_then_decode_roundtrip = [](Fx& fx) {
        auto f = ipc::encode_frame(fx.msg);
        fx.frame_size = f.size();
        auto d = ipc::decode_frame(f);
        fx.decoded = d.has_value();
        if (fx.decoded) {
            fx.got_head = d->head;
            fx.got_body = d->body;
        }
    };
    auto expect_roundtrip_faithful = [](const Fx& fx) {
        REQUIRE(fx.frame_size == sizeof(IpcHeader) + 3);
        REQUIRE(fx.decoded);
        REQUIRE(fx.got_head.magic == kIpcMagic);      // encode 盖章
        REQUIRE(fx.got_head.version == kIpcVersion);
        REQUIRE(fx.got_head.payload_len == 3u);
        REQUIRE(fx.got_head.type == fx.msg.head.type);
        REQUIRE(fx.got_head.request_id == fx.msg.head.request_id);
        REQUIRE(fx.got_head.seq == fx.msg.head.seq);
        REQUIRE(fx.got_head.error_code == fx.msg.head.error_code);
        REQUIRE(fx.got_body == fx.msg.body);
    };

    auto fx = build_embedded_nul_message();
    run_encode_then_decode_roundtrip(*fx);
    expect_roundtrip_faithful(*fx);
}

TEST_CASE("02：decode 坏帧带出错误码（不分配/不越界）", "[02]") {
    struct Fx {
        struct DecodeOut {
            bool has_value = false;
            IpcError err = IpcError::kOk;    // 仅解出失败时有效
        };
        std::vector<std::byte> tiny;            // 头不完整
        std::vector<std::byte> bad_magic;       // 指纹不符
        std::vector<std::byte> bad_version;     // 版本不识别（version 在偏移 4，直接改头）
        std::vector<std::byte> over_long;       // 超长负载：先验长度再分配（声称 kMaxPayload+1，帧只有几十字节）
        std::vector<std::byte> truncated;       // 帧长与长度字段不符（截断）
        DecodeOut tiny_out;
        DecodeOut bad_magic_out;
        DecodeOut bad_version_out;
        DecodeOut over_long_out;
        DecodeOut truncated_out;
    };

    auto build_malformed_frame_set = [] {
        auto fx = std::make_unique<Fx>();
        fx->tiny = std::vector<std::byte>(sizeof(IpcHeader) - 1);
        fx->bad_magic = ipc::encode_frame(IpcMessage{good_header(), "abc"});
        fx->bad_magic[0] ^= std::byte{0xff};
        IpcHeader h = good_header();
        h.version = uint16_t(kIpcVersion + 1);
        fx->bad_version = ipc::encode_frame(IpcMessage{good_header(), "abc"});
        std::memcpy(fx->bad_version.data(), &h, sizeof h);
        h = good_header();
        h.payload_len = kMaxPayload + 1;
        fx->over_long = ipc::encode_frame(IpcMessage{good_header(), "abc"});
        std::memcpy(fx->over_long.data(), &h, sizeof h);
        fx->truncated = ipc::encode_frame(IpcMessage{good_header(), "abc"});
        fx->truncated.resize(sizeof(IpcHeader) + 2);
        return fx;
    };
    auto run_decode_on_each_bad_frame = [](Fx& fx) {
        auto r = ipc::decode_frame(fx.tiny);
        fx.tiny_out = {r.has_value(), r.has_value() ? IpcError::kOk : r.error()};
        r = ipc::decode_frame(fx.bad_magic);
        fx.bad_magic_out = {r.has_value(), r.has_value() ? IpcError::kOk : r.error()};
        r = ipc::decode_frame(fx.bad_version);
        fx.bad_version_out = {r.has_value(), r.has_value() ? IpcError::kOk : r.error()};
        r = ipc::decode_frame(fx.over_long);
        fx.over_long_out = {r.has_value(), r.has_value() ? IpcError::kOk : r.error()};
        r = ipc::decode_frame(fx.truncated);
        fx.truncated_out = {r.has_value(), r.has_value() ? IpcError::kOk : r.error()};
    };
    auto expect_bad_frames_rejected_with_codes = [](const Fx& fx) {
        REQUIRE_FALSE(fx.tiny_out.has_value);
        REQUIRE(fx.tiny_out.err == IpcError::kBadMagic);
        REQUIRE_FALSE(fx.bad_magic_out.has_value);
        REQUIRE(fx.bad_magic_out.err == IpcError::kBadMagic);
        REQUIRE_FALSE(fx.bad_version_out.has_value);
        REQUIRE(fx.bad_version_out.err == IpcError::kUnsupported);
        REQUIRE_FALSE(fx.over_long_out.has_value);
        REQUIRE(fx.over_long_out.err == IpcError::kTooLarge);
        REQUIRE_FALSE(fx.truncated_out.has_value);
        REQUIRE(fx.truncated_out.err == IpcError::kBadMagic);
    };

    auto fx = build_malformed_frame_set();
    run_decode_on_each_bad_frame(*fx);
    expect_bad_frames_rejected_with_codes(*fx);
}

TEST_CASE("02：to_event 消息翻事件（type 同值、行随事件）", "[02]") {
    struct Fx {
        IpcMessage msg{good_header(), "hi"};
        IpcMessage msg_no_row{good_header(), "hi"};   // 号段内未出行的类型
        Event ev{};
        Event ev_no_row{};
        IpcMessageView view{};                        // ev.payload 解出的送达形态
    };

    auto build_messages_with_and_without_row = [] {
        auto fx = std::make_unique<Fx>();
        fx->msg_no_row.head.type = uint16_t(kIpc) + 50;
        return fx;
    };
    auto run_message_to_event_conversion = [](Fx& fx) {
        fx.ev = ipc::to_event(fx.msg, 5, 42, 1000);
        fx.view = std::any_cast<IpcMessageView>(fx.ev.payload);
        fx.ev_no_row = ipc::to_event(fx.msg_no_row, 1, 2, 3);
    };
    auto expect_type_matches_and_row_follows = [](const Fx& fx) {
        REQUIRE(fx.ev.type == EventType(kTypeIpcRequest));
        REQUIRE(fx.ev.source == 5);                 // PeerId 即 SourceId（寻址约定）
        REQUIRE(fx.ev.pipe == 42);
        REQUIRE(fx.ev.ts_ns == 1000);
        REQUIRE(fx.ev.def == &kTypeIpcRequest);     // 策略行随事件
        REQUIRE(fx.view.from == 5);
        REQUIRE(fx.view.head.request_id == 42);
        REQUIRE(fx.view.head.seq == 7);
        REQUIRE(fx.view.body == "hi");

        // 号段内未出行的类型：type 照转，行查无 = 空（背压按 kBlock）
        REQUIRE(fx.ev_no_row.type == EventType(kIpc) + 50);
        REQUIRE(fx.ev_no_row.def == nullptr);
    };

    auto fx = build_messages_with_and_without_row();
    run_message_to_event_conversion(*fx);
    expect_type_matches_and_row_follows(*fx);
}

TEST_CASE("02：make_timeout_event 合成 kTimeout 响应（与响应同形）", "[02]") {
    struct Fx {
        Event ev{};
        IpcMessageView view{};      // ev.payload 解出的送达形态
    };

    auto build_timeout_synthesis_case = [] {
        return std::make_unique<Fx>();
    };
    auto run_timeout_event_synthesis = [](Fx& fx) {
        fx.ev = ipc::make_timeout_event(9, 77, 500);
        fx.view = std::any_cast<IpcMessageView>(fx.ev.payload);
    };
    auto expect_timeout_shaped_as_response = [](const Fx& fx) {
        REQUIRE(fx.ev.type == EventType(kTypeIpcResponse));   // 业务看到的是响应（不是内部超时事件）
        REQUIRE(fx.ev.def == &kTypeIpcResponse);
        REQUIRE(fx.ev.pipe == 77);                            // 投回执管道
        REQUIRE(fx.ev.ts_ns == 500);
        REQUIRE(fx.ev.source == 0);                           // 合成事件：source 保留值 0
        REQUIRE(fx.view.head.request_id == 9);
        REQUIRE(fx.view.head.error_code == int32_t(IpcError::kTimeout));
        REQUIRE(fx.view.head.type == uint16_t(EventType(kTypeIpcResponse)));
        REQUIRE(fx.view.body.empty());
    };

    auto fx = build_timeout_synthesis_case();
    run_timeout_event_synthesis(*fx);
    expect_timeout_shaped_as_response(*fx);
}

TEST_CASE("02：seq 对账跳号 → 丢块区间", "[02]") {
    struct Fx {
        uint64_t from = 0, to = 0;          // seq_gap 的丢块出参
        bool gap_on_continuous = true;      // 初值取 true：Act 没跑到就过不了断言
        bool gap_on_jump = false;
        uint64_t jump_from = 0, jump_to = 0;
        bool gap_on_rewind = true;          // 初值取 true：同上
    };

    auto build_seq_gap_probes = [] {
        return std::make_unique<Fx>();
    };
    auto run_three_seq_gap_probes = [](Fx& fx) {
        fx.gap_on_continuous = ipc::seq_gap(5, 5, fx.from, fx.to);
        fx.gap_on_jump = ipc::seq_gap(5, 8, fx.from, fx.to);
        fx.jump_from = fx.from;
        fx.jump_to = fx.to;
        fx.gap_on_rewind = ipc::seq_gap(5, 3, fx.from, fx.to);
    };
    auto expect_jump_reports_closed_open_range = [](const Fx& fx) {
        REQUIRE_FALSE(fx.gap_on_continuous);   // 连续
        REQUIRE(fx.gap_on_jump);               // 跳号：丢了 [5, 8)
        REQUIRE(fx.jump_from == 5u);
        REQUIRE(fx.jump_to == 8u);
        REQUIRE_FALSE(fx.gap_on_rewind);       // 回退不按丢块（SPSC 无乱序）
    };

    auto fx = build_seq_gap_probes();
    run_three_seq_gap_probes(*fx);
    expect_jump_reports_closed_open_range(*fx);
}

// ===== ShmRing：环薄层 =====

TEST_CASE("02：ShmRing put/peek/consume 顺序，peek 只投不摘", "[02]") {
    struct Fx {
        ipc::ShmRing ring{256};
        bool wake_fd_ok = false;
        bool put_aaaa_ok = false;
        bool put_bbbb_ok = false;
        DataBlockView v1{}, v2{}, v3{};
        int cmp_aaaa = -1, cmp_bbbb = -1;            // memcmp 结果（0 = 保真）
        uint64_t consumed_after_peeks = 0;           // 两 peek 后的 consume 游标
        uint64_t delivered_after_drain = 0;          // 取尽后的 deliver 游标
        uint64_t consumed_after_v1 = 0;
        uint64_t consumed_after_v2 = 0;
    };

    auto build_256b_ring = [] {
        return std::make_unique<Fx>();
    };
    auto run_put_peek_consume_sequence = [](Fx& fx) {
        fx.wake_fd_ok = fx.ring.wake_fd() >= 0;
        fx.put_aaaa_ok = fx.ring.put(bytes_of("AAAA"));
        fx.put_bbbb_ok = fx.ring.put(bytes_of("BBBB"));

        fx.v1 = fx.ring.peek();                     // 第一次 peek：取头块
        fx.cmp_aaaa = fx.v1.data ? std::memcmp(fx.v1.data, "AAAA", 4) : -1;
        fx.v2 = fx.ring.peek();                     // deliver 游标前移：取次块
        fx.cmp_bbbb = fx.v2.data ? std::memcmp(fx.v2.data, "BBBB", 4) : -1;
        fx.consumed_after_peeks = fx.ring.consumed();

        fx.v3 = fx.ring.peek();                     // 取尽
        fx.delivered_after_drain = fx.ring.delivered();

        fx.ring.consume(fx.v1);
        fx.consumed_after_v1 = fx.ring.consumed();
        fx.ring.consume(fx.v2);
        fx.consumed_after_v2 = fx.ring.consumed();
    };
    auto expect_peek_delivers_in_order_without_consuming = [](const Fx& fx) {
        REQUIRE(fx.wake_fd_ok);
        REQUIRE(fx.put_aaaa_ok);
        REQUIRE(fx.put_bbbb_ok);

        REQUIRE(fx.v1.len == 4u);
        REQUIRE(fx.cmp_aaaa == 0);
        REQUIRE(fx.v1.seq == 0u);
        REQUIRE(fx.v1.pos == 0u);
        REQUIRE(fx.v1.ring == 0u);                       // ring/from 归 RingRecord 层回填
        REQUIRE(fx.v1.from == 0u);

        REQUIRE(fx.cmp_bbbb == 0);
        REQUIRE(fx.v2.seq == 1u);
        REQUIRE(fx.v2.pos == 32u);                       // 4B 负载 → 帧 16+4 对齐到 32
        REQUIRE(fx.consumed_after_peeks == 0u);          // peek 不 consume：槽位未还

        REQUIRE(fx.v3.data == nullptr);
        REQUIRE(fx.v3.len == 0u);
        REQUIRE(fx.delivered_after_drain == 64u);

        REQUIRE(fx.consumed_after_v1 == 32u);            // release 才摘
        REQUIRE(fx.consumed_after_v2 == 64u);
    };

    auto fx = build_256b_ring();
    run_put_peek_consume_sequence(*fx);
    expect_peek_delivers_in_order_without_consuming(*fx);
}

TEST_CASE("02：ShmRing 环满返 false，consume 后槽位复用", "[02]") {
    struct Fx {
        ipc::ShmRing ring{64};                  // 有效 64B
        std::vector<std::byte> blk;             // 16B 负载 → 帧 32B
        std::vector<std::byte> oversize_blk;    // 100B 单块超环负载
        bool put_first_ok = false;
        bool put_second_ok = false;
        bool put_third_ok = true;               // 初值取 true：Act 没跑到就过不了断言
        bool put_oversize_ok = true;            // 初值取 true：同上
        size_t peek_len = 0;
        bool put_while_full_ok = true;          // 初值取 true：同上
        bool put_after_release_ok = false;
    };

    auto build_ring_fill_inputs = [] {
        auto fx = std::make_unique<Fx>();
        fx->blk = bytes_of("0123456789abcdef");
        fx->oversize_blk = bytes_of(std::string(100, 'x'));
        return fx;
    };
    auto run_fill_reject_and_reuse = [](Fx& fx) {
        fx.put_first_ok = fx.ring.put(fx.blk);
        fx.put_second_ok = fx.ring.put(fx.blk);
        fx.put_third_ok = fx.ring.put(fx.blk);
        fx.put_oversize_ok = fx.ring.put(fx.oversize_blk);

        auto v = fx.ring.peek();               // peek 只投不摘 → 槽位未还
        fx.peek_len = v.len;
        fx.put_while_full_ok = fx.ring.put(fx.blk);

        fx.ring.consume(v);                    // release 才摘
        fx.put_after_release_ok = fx.ring.put(fx.blk);
    };
    auto expect_ring_full_until_release = [](const Fx& fx) {
        REQUIRE(fx.put_first_ok);
        REQUIRE(fx.put_second_ok);                                 // 64B 恰满
        REQUIRE_FALSE(fx.put_third_ok);                            // 环满：不阻塞返 false
        REQUIRE_FALSE(fx.put_oversize_ok);                         // 单块超环：永不可入
        REQUIRE(fx.peek_len == 16u);
        REQUIRE_FALSE(fx.put_while_full_ok);                       // 仍满
        REQUIRE(fx.put_after_release_ok);                          // 槽位复用成功
    };

    auto fx = build_ring_fill_inputs();
    run_fill_reject_and_reuse(*fx);
    expect_ring_full_until_release(*fx);
}

TEST_CASE("02：ShmRing seq 连续递增，槽位复用不回卷", "[02]") {
    struct Fx {
        ipc::ShmRing ring{128};
        std::vector<bool> put_ok;               // 五次 put 的返回
        std::vector<uint64_t> seqs;             // 逐块记下的 seq
        bool drained = false;                   // 五轮后取尽
    };

    auto build_128b_ring = [] {
        return std::make_unique<Fx>();
    };
    auto run_five_put_peek_consume_cycles = [](Fx& fx) {
        for (uint64_t i = 0; i < 5; ++i) {
            fx.put_ok.push_back(fx.ring.put(bytes_of("x")));
            auto v = fx.ring.peek();
            fx.seqs.push_back(v.seq);
            fx.ring.consume(v);
        }
        fx.drained = fx.ring.peek().data == nullptr;
    };
    auto expect_monotonic_seqs_after_cycling = [](const Fx& fx) {
        REQUIRE(fx.put_ok == std::vector<bool>{true, true, true, true, true});   // 1B 负载 → 帧 32B，5 次必跨环尾
        REQUIRE(fx.seqs == std::vector<uint64_t>{0, 1, 2, 3, 4});                // seq 全局递增，consume 复用槽位不重号
        REQUIRE(fx.drained);
    };

    auto fx = build_128b_ring();
    run_five_put_peek_consume_cycles(*fx);
    expect_monotonic_seqs_after_cycling(*fx);
}

TEST_CASE("02：ShmRing 跨尾填充帧：peek 跳过、数据保真", "[02]") {
    struct Fx {
        ipc::ShmRing ring{64};
        std::vector<std::byte> blk;             // 24B 负载 → 帧 48B
        bool put_first_ok = false;
        size_t first_pos = 0;
        bool put_wrapped_ok = false;
        size_t wrapped_pos = 0;
        uint64_t wrapped_seq = 0;
        size_t wrapped_len = 0;
        int wrapped_cmp = -1;                   // memcmp 结果（0 = 保真）
        uint64_t consumed_after_first_release = 0;
        uint64_t consumed_after_second_release = 0;
        bool drained = false;
    };

    auto build_wrap_around_inputs = [] {
        auto fx = std::make_unique<Fx>();
        fx->blk = bytes_of(std::string(24, 'q'));
        return fx;
    };
    auto run_wrap_around_frame_scenario = [](Fx& fx) {
        fx.put_first_ok = fx.ring.put(fx.blk);
        auto v1 = fx.ring.peek();
        fx.first_pos = v1.pos;
        fx.ring.consume(v1);                    // 游标 48，环尾只剩 16B

        fx.put_wrapped_ok = fx.ring.put(fx.blk);
        auto v2 = fx.ring.peek();               // peek 内部跳过填充帧
        fx.wrapped_pos = v2.pos;
        fx.wrapped_seq = v2.seq;
        fx.wrapped_len = v2.len;
        fx.wrapped_cmp = v2.data ? std::memcmp(v2.data, std::string(24, 'q').data(), 24) : -1;
        fx.consumed_after_first_release = fx.ring.consumed();

        fx.ring.consume(v2);
        fx.consumed_after_second_release = fx.ring.consumed();
        fx.drained = fx.ring.peek().data == nullptr;
    };
    auto expect_wrapped_frame_skipped_and_faithful = [](const Fx& fx) {
        REQUIRE(fx.put_first_ok);                            // [0, 48)
        REQUIRE(fx.first_pos == 0u);
        REQUIRE(fx.put_wrapped_ok);                          // 尾 16 < 48：先写填充帧再绕回首部
        REQUIRE(fx.wrapped_pos == 64u);                      // 数据帧起点（绕回 0，单调位 64）
        REQUIRE(fx.wrapped_seq == 1u);
        REQUIRE(fx.wrapped_len == 24u);
        REQUIRE(fx.wrapped_cmp == 0);
        REQUIRE(fx.consumed_after_first_release == 48u);     // 填充帧不占 consume（按块 pos 归还）
        REQUIRE(fx.consumed_after_second_release == 112u);
        REQUIRE(fx.drained);
    };

    auto fx = build_wrap_around_inputs();
    run_wrap_around_frame_scenario(*fx);
    expect_wrapped_frame_skipped_and_faithful(*fx);
}

TEST_CASE("02：ShmRing 空→非空 eventfd 唤醒", "[02]") {
    struct Fx {
        ipc::ShmRing ring{256};
        bool wake_at_start = true;              // 初值取 true：Act 没跑到就过不了断言
        bool put_aa_ok = false;
        bool wake_after_put_aa = false;
        uint64_t drain_after_put_aa = 0;
        bool wake_after_drain = true;           // 初值取 true：同上
        bool put_bb_ok = false;
        bool wake_after_put_bb = true;          // 初值取 true：同上
        bool put_cc_ok = false;
        bool wake_after_put_cc = false;
        uint64_t drain_after_put_cc = 0;
        bool wake_after_peek_only = true;       // 初值取 true：同上
        bool wake_after_consume = true;         // 初值取 true：同上
    };

    auto build_empty_ring = [] {
        return std::make_unique<Fx>();
    };
    auto run_wake_on_empty_to_nonempty_scenario = [](Fx& fx) {
        auto fd = fx.ring.wake_fd();
        fx.wake_at_start = wake_pending(fd);

        fx.put_aa_ok = fx.ring.put(bytes_of("AA"));
        fx.wake_after_put_aa = wake_pending(fd);
        fx.drain_after_put_aa = drain_wake(fd);
        fx.wake_after_drain = wake_pending(fd);

        fx.put_bb_ok = fx.ring.put(bytes_of("BB"));
        fx.wake_after_put_bb = wake_pending(fd);

        auto v1 = fx.ring.peek();
        fx.ring.consume(v1);                    // 只取走一块：deliver 未到 write
        auto v2 = fx.ring.peek();
        fx.ring.consume(v2);                    // 取尽：deliver == write

        fx.put_cc_ok = fx.ring.put(bytes_of("CC"));
        fx.wake_after_put_cc = wake_pending(fd);
        fx.drain_after_put_cc = drain_wake(fd);

        auto v3 = fx.ring.peek();               // peek/consume 自己不写 eventfd
        fx.wake_after_peek_only = wake_pending(fd);
        fx.ring.consume(v3);
        fx.wake_after_consume = wake_pending(fd);
    };
    auto expect_wake_only_on_empty_to_nonempty = [](const Fx& fx) {
        REQUIRE_FALSE(fx.wake_at_start);            // 初始无数据无唤醒
        REQUIRE(fx.put_aa_ok);                      // 空→非空 → 醒
        REQUIRE(fx.wake_after_put_aa);
        REQUIRE(fx.drain_after_put_aa == 1u);
        REQUIRE_FALSE(fx.wake_after_drain);         // 已排空
        REQUIRE(fx.put_bb_ok);                      // 读者未取尽 → 不重复醒
        REQUIRE_FALSE(fx.wake_after_put_bb);
        REQUIRE(fx.put_cc_ok);                      // 再次空→非空 → 醒
        REQUIRE(fx.wake_after_put_cc);
        REQUIRE(fx.drain_after_put_cc == 1u);
        REQUIRE_FALSE(fx.wake_after_peek_only);
        REQUIRE_FALSE(fx.wake_after_consume);
    };

    auto fx = build_empty_ring();
    run_wake_on_empty_to_nonempty_scenario(*fx);
    expect_wake_only_on_empty_to_nonempty(*fx);
}

TEST_CASE("02：ShmRing capacity 钳到至少一帧", "[02]") {
    struct Fx {
        ipc::ShmRing ring{8};                   // 16 对齐后 16 < 帧下限 32 → 钳到 32
        std::vector<std::byte> full_blk;        // 8B 负载 → 帧 32B
        std::vector<std::byte> extra_blk;       // 1B 负载
        bool put_ok = false;
        bool put_overflow_ok = true;            // 初值取 true：Act 没跑到就过不了断言
        size_t peek_len = 0;
        int cmp = -1;                           // memcmp 结果（0 = 保真）
    };

    auto build_clamped_tiny_ring = [] {
        auto fx = std::make_unique<Fx>();
        fx->full_blk = bytes_of("12345678");
        fx->extra_blk = bytes_of("x");
        return fx;
    };
    auto run_clamped_capacity_probe = [](Fx& fx) {
        fx.put_ok = fx.ring.put(fx.full_blk);
        fx.put_overflow_ok = fx.ring.put(fx.extra_blk);
        auto v = fx.ring.peek();
        fx.peek_len = v.len;
        fx.cmp = v.data ? std::memcmp(v.data, "12345678", 8) : -1;
    };
    auto expect_clamped_ring_holds_one_frame = [](const Fx& fx) {
        REQUIRE(fx.put_ok);                     // 8B 负载 → 帧 32B：恰满
        REQUIRE_FALSE(fx.put_overflow_ok);      // 无余量
        REQUIRE(fx.peek_len == 8u);
        REQUIRE(fx.cmp == 0);
    };

    auto fx = build_clamped_tiny_ring();
    run_clamped_capacity_probe(*fx);
    expect_clamped_ring_holds_one_frame(*fx);
}
