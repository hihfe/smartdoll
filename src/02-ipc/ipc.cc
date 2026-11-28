// ===== ipc.cc：02 实现 =====
// 落地面：纯函数（协议判定）+ ShmRing 环薄层 + 机制编排（UDS 分帧、请求表、超时/断连、环唤醒）。
// 封装靠文件边界：类定义与内脏住本文件，应用层只 include ipc.h（白盒见 ipc_internal.h）。
// 重连退避（kTypeIpcReconnect / Backoff 行）随后续落地。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE          // memfd_create（<sys/mman.h>，glibc 需此宏）
#endif

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ipc_internal.h"

namespace {

// ===== 共享内存布局（memfd 映射内，跨进程可见） =====

struct ShmHead {              // 数据区前的游标区（32B，随映射 16 对齐）
    uint64_t write;           // 写游标（单调字节位，写侧推进）
    uint64_t deliver;         // 投递游标（peek 推进）
    uint64_t consume;         // 消费游标（release 推进）
    uint64_t next_seq;        // 下一块 seq（写侧发号）
};
static_assert(sizeof(ShmHead) == 32);
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free);   // 跨进程原子的前提

struct FrameHead {            // 帧头 16B：数据帧 = 头 + 负载，16 对齐、不跨环尾
    uint32_t len;             // 数据帧 = 负载长；填充帧 = 填充总长 − 16
    uint32_t kind;            // 0 = 数据 / 1 = 填充（绕回占位，peek 跳过）
    uint64_t seq;
};
static_assert(sizeof(FrameHead) == 16);

constexpr uint32_t kKindData = 0;
constexpr uint32_t kKindPad  = 1;
constexpr size_t   kMinCap   = 32;      // 最小环容 = 最小数据帧（16 头 + 1B 负载对齐到 32）

size_t frame_size(size_t payload) {      // 帧总长：头 + 负载，16 对齐
    return (sizeof(FrameHead) + payload + 15) & ~size_t{15};
}

ShmHead* head_of(void* map) { return static_cast<ShmHead*>(map); }
std::byte* area_of(void* map) { return static_cast<std::byte*>(map) + sizeof(ShmHead); }

// 02 号段行查询：type → 策略行（行随事件，查无 = 空 = 背压按 kBlock）
const EventDef* row_of(EventType t) {
    if (t == kTypeIpcRequest)   return &kTypeIpcRequest;
    if (t == kTypeIpcResponse)  return &kTypeIpcResponse;
    if (t == kTypeIpcNotify)    return &kTypeIpcNotify;
    if (t == kTypeIpcDataReady) return &kTypeIpcDataReady;
    if (t == kTypeIpcBroken)    return &kTypeIpcBroken;
    if (t == kTypeIpcTimeout)   return &kTypeIpcTimeout;
    if (t == kTypeIpcDataLost)  return &kTypeIpcDataLost;
    if (t == kTypeIpcReconnect) return &kTypeIpcReconnect;
    return nullptr;
}

// ===== 机制层常量与合成事件 =====

constexpr size_t kReadChunk = 4096;     // 每次 UDS 读的粒度（分帧状态按头 payload_len 攒）
constexpr size_t kRingBytes = 1u << 16; // 每环 64KiB（数据面薄层，环满交调用方处置）

struct SpinLock {                       // 写侧收敛（同环多线程 write_data 串行）
    std::atomic_flag f = ATOMIC_FLAG_INIT;
    void lock() { while (f.test_and_set(std::memory_order_acquire)) {} }
    void unlock() { f.clear(std::memory_order_release); }
};

// 合成响应（与响应同形）：超时 / kBroken / kTooLarge 都走这一形，业务只见 error_code
Event make_err_event(RequestId id, PipeId reply, int32_t err, uint64_t now_ns) {
    IpcMessageView v;
    v.from = 0;                          // 合成事件：source 保留值 0
    v.head = IpcHeader{0, 0, uint16_t(EventType(kTypeIpcResponse)), id, 0, err, 0};
    Event ev{};
    ev.type = kTypeIpcResponse;
    ev.source = 0;
    ev.pipe = reply;
    ev.ts_ns = now_ns;
    ev.payload = std::move(v);
    ev.def = &kTypeIpcResponse;
    return ev;
}

// kTypeIpcTimeout 模板（挂 04，到期原样投回 inbox）：payload 只带 id（销账都在 inbox 侧）
Event make_timeout_tpl(RequestId id, PipeId inbox) {
    Event ev{};
    ev.type = kTypeIpcTimeout;
    ev.source = 0;
    ev.pipe = inbox;
    ev.payload = IpcTimeoutPayload{id};
    ev.def = &kTypeIpcTimeout;
    return ev;
}

}  // namespace

// ===== 内部类型（实现文件；应用层只见 ipc.h 的前向声明） =====

struct PeerRecord {                     // 每对端一份（unique_ptr 持有：地址稳定，读单 buf 指进来）
    PeerId id = 0;
    PipeId events = 0;                  // 入站事件收件管道（listen/connect 注册时声明）
    int    fd = -1;
    bool   dead = false;

    OpCtx  read_op{};                   // 常驻读单原型（buf 指 rbuf，完成即重挂）
    std::array<std::byte, kReadChunk> rbuf{};
    std::vector<std::byte> acc;         // InboundFrame 的落地形：粘包/半包累积缓冲（inbox 单线程）

    std::mutex wmu;                     // 写队列收敛（request/respond/notify 任意线程）
    std::vector<std::byte> wbuf;        // 在飞写缓冲（仅 writing=false 时可换，防写单读悬垂）
    std::deque<std::vector<std::byte>> wq;
    bool writing = false;
};

struct RingRecord {                     // 每环一份（本阶段每对端一条，环回读写同体）
    RingId   id = 0;
    PeerId   peer = 0;
    PipeId   deliver = 0;               // kTypeIpcDataReady / kTypeIpcDataLost 投递管道
    ipc::ShmRing ring;                  // 环 + eventfd
    SpinLock wlock;                     // 写侧收敛（环本体仍 SPSC）
    uint64_t expect = 0;                // 下一期望 seq（对账基准）
    OpCtx    wake{};                    // eventfd 常驻读单原型（完成即重挂）
    uint64_t wake_buf = 0;

    RingRecord(RingId i, PeerId p, PipeId dv, size_t cap)
        : id(i), peer(p), deliver(dv), ring(cap) {}
};

struct RequestEntry {                   // 请求表条目（Ledger 条目协议：id/inflight/cancelled/th）
    PeerId     to = 0;
    PipeId     reply = 0;               // 回执管道：响应与超时/断连合成都投这
    TimerHandle th = 0;                 // 04 语义句柄：响应先到要撤
    uint64_t   id = 0;
    int        inflight = 0;
    bool       cancelled = false;
};

struct RequestTable {                   // request_id → RequestEntry（Ledger 协议，见 with.h）
    using entry_type = RequestEntry;
    using id_type    = uint64_t;

    std::mutex mu;
    std::unordered_map<RequestId, RequestEntry> by_id;
    uint64_t next = 1;
    // 销账唯一裁决点 = inbox 消费线程（登记在调用线程）：late_drops 只被 settle 线程 ++，
    // 观测在测试等待后读，无并发写；表内一把锁护增删查，find 出来的条目指针在同一次 settle 内用。
    size_t   late_drops = 0;

    uint64_t alloc_id() { std::lock_guard lk(mu); return next++; }
    RequestEntry& insert(RequestEntry e) {
        auto [it, _] = by_id.emplace(e.id, std::move(e));
        return it->second;
    }
    RequestEntry* find(const RequestId& k) {
        std::lock_guard lk(mu);
        auto it = by_id.find(k);
        return it == by_id.end() ? nullptr : &it->second;
    }
    void erase(RequestEntry& e) { std::lock_guard lk(mu); by_id.erase(e.id); }

    size_t size() { std::lock_guard lk(mu); return by_id.size(); }
    std::vector<RequestEntry> take_to(PeerId to) {          // 断连/停机：摘出该对端全部未决
        std::lock_guard lk(mu);
        std::vector<RequestEntry> out;
        for (auto it = by_id.begin(); it != by_id.end();)
            if (it->second.to == to) { out.push_back(it->second); it = by_id.erase(it); }
            else ++it;
        return out;
    }
    std::vector<RequestEntry> take_all() {                  // 停机：摘空整表
        std::lock_guard lk(mu);
        std::vector<RequestEntry> out;
        out.reserve(by_id.size());
        for (auto& [_, e] : by_id) out.push_back(e);
        by_id.clear();
        return out;
    }
};

class Ipc {                             // 资源聚合体：生死归 create/stop，操作全在门面自由函数
public:
    Ipc(Dispatcher& d_, Timer& t_) : d(d_), timer(t_) {}

    PipeId      inbox = kNoPipe;        // 02 收件管道：01 投回的 IO 完成 / Wake / 错误，内部超时也进这
    Dispatcher& d;
    Timer&      timer;

    std::mutex  mu;                     // 护 peers / rings / 监听资源
    std::unordered_map<PeerId, std::unique_ptr<PeerRecord>> peers;   // 停机前不摘（读单 buf 悬垂防护）
    std::unordered_map<RingId, std::unique_ptr<RingRecord>> rings;
    std::unordered_map<PeerId, RingId> tx_ring;             // write_data 的 PeerId → 环
    RequestTable pending;               // 请求账本

    std::atomic<int>  listen_fd{-1};
    std::atomic<int>  stop_fd{-1};      // acceptor 的退出唤醒（监听套接字 close 不打断阻塞 accept）
    std::string       listen_path;
    PipeId            listen_events = 0;
    std::thread       acceptor;

    PeerId next_peer = 1;
    RingId next_ring = 1;
    std::atomic<size_t> unknown{0};     // 号段内未识类型丢弃计数

    std::atomic<bool> running{true};
    bool stopped() const { return !running.load(); }   // live 切面查（已停才拒）
};

// ===== 编排与门面 =====

namespace ipc {

// ===== 纯函数 =====

IpcError check_header(const IpcHeader& h) {
    if (h.magic != kIpcMagic) return IpcError::kBadMagic;
    if (h.version != kIpcVersion) return IpcError::kUnsupported;
    if (h.type < uint16_t(kIpc) || h.type >= uint16_t(kIpc) + 100) return IpcError::kUnsupported;
    if (h.payload_len > kMaxPayload) return IpcError::kTooLarge;
    return IpcError::kOk;
}

std::expected<IpcMessage, IpcError> decode_frame(std::span<const std::byte> f) {
    if (f.size() < sizeof(IpcHeader)) return std::unexpected(IpcError::kBadMagic);
    IpcHeader h{};
    std::memcpy(&h, f.data(), sizeof h);
    if (auto e = check_header(h); e != IpcError::kOk) return std::unexpected(e);   // 先验头，再信长度
    if (f.size() != sizeof(IpcHeader) + h.payload_len) return std::unexpected(IpcError::kBadMagic);
    IpcMessage m;
    m.head = h;
    m.body.assign(reinterpret_cast<const char*>(f.data()) + sizeof(IpcHeader), h.payload_len);
    return m;
}

std::vector<std::byte> encode_frame(const IpcMessage& m) {
    IpcHeader h = m.head;
    h.magic = kIpcMagic;                    // 盖章：指纹/版本/长度由编码侧定死
    h.version = kIpcVersion;
    h.payload_len = uint32_t(m.body.size());
    std::vector<std::byte> out(sizeof(IpcHeader) + m.body.size());
    std::memcpy(out.data(), &h, sizeof h);
    if (!m.body.empty())
        std::memcpy(out.data() + sizeof(IpcHeader), m.body.data(), m.body.size());
    return out;
}

Event to_event(const IpcMessage& m, PeerId from, PipeId to, uint64_t now_ns) {
    Event ev{};
    ev.type = m.head.type;                   // 线上 type 与 Event.type 同值
    ev.source = from;                        // PeerId 即 SourceId（寻址约定）
    ev.pipe = to;
    ev.ts_ns = now_ns;
    ev.payload = IpcMessageView{from, m.head, m.body};
    ev.def = row_of(m.head.type);
    return ev;
}

Event make_timeout_event(RequestId id, PipeId to, uint64_t now_ns) {
    return make_err_event(id, to, int32_t(IpcError::kTimeout), now_ns);   // 与响应同形
}

bool seq_gap(uint64_t expect, uint64_t got, uint64_t& from, uint64_t& to) {
    if (got <= expect) return false;         // 连续 / 回退（SPSC 无乱序）不按丢块
    from = expect;
    to = got;
    return true;
}

// ===== ShmRing：共享内存 SPSC 环薄层 =====

ShmRing::ShmRing(size_t capacity) {
    size_t cap = (capacity + 15) & ~size_t{15};
    if (cap < kMinCap) cap = kMinCap;
    memfd_ = ::memfd_create("doll-ring", MFD_CLOEXEC);
    if (memfd_ < 0) return;                  // 置死形：put 恒 false / peek 恒空（库错误不抛上层）
    map_size_ = sizeof(ShmHead) + cap;
    if (::ftruncate(memfd_, off_t(map_size_)) != 0) goto fail;
    map_ = ::mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, memfd_, 0);
    if (map_ == MAP_FAILED) { map_ = nullptr; goto fail; }
    efd_ = ::eventfd(0, EFD_CLOEXEC);        // 阻塞形：01 读单在内核睡等，非空才醒
    if (efd_ < 0) { ::munmap(map_, map_size_); map_ = nullptr; goto fail; }
    return;
fail:
    ::close(memfd_);
    memfd_ = -1;
}

ShmRing::~ShmRing() {
    if (map_) ::munmap(map_, map_size_);
    if (memfd_ >= 0) ::close(memfd_);
    if (efd_ >= 0) ::close(efd_);
}

ShmRing::ShmRing(ShmRing&& o) noexcept
    : map_(std::exchange(o.map_, nullptr)), map_size_(std::exchange(o.map_size_, 0)),
      memfd_(std::exchange(o.memfd_, -1)), efd_(std::exchange(o.efd_, -1)) {}

ShmRing& ShmRing::operator=(ShmRing&& o) noexcept {
    if (this == &o) return *this;
    if (map_) ::munmap(map_, map_size_);
    if (memfd_ >= 0) ::close(memfd_);
    if (efd_ >= 0) ::close(efd_);
    map_ = std::exchange(o.map_, nullptr);
    map_size_ = std::exchange(o.map_size_, 0);
    memfd_ = std::exchange(o.memfd_, -1);
    efd_ = std::exchange(o.efd_, -1);
    return *this;
}

bool ShmRing::put(std::span<const std::byte> data) {
    if (data.empty()) return true;           // 空负载：无操作（不占环不唤醒）
    if (!map_ || efd_ < 0) return false;     // 环置死
    auto* h = head_of(map_);
    const size_t cap = map_size_ - sizeof(ShmHead);
    const size_t fs = frame_size(data.size());
    if (fs > cap) return false;              // 单块超环：永不可入

    // SPSC 写侧独占游标；consume 读 acquire（读者归还后槽位方可复用）
    const uint64_t w = std::atomic_ref<uint64_t>(h->write).load(std::memory_order_relaxed);
    const uint64_t c = std::atomic_ref<uint64_t>(h->consume).load(std::memory_order_acquire);
    const size_t off = size_t(w % cap);
    const size_t tail = cap - off;
    const size_t pad = tail < fs ? tail : 0; // 环尾不足先写填充帧（帧不跨尾）
    if (fs + pad > cap - size_t(w - c)) return false;   // 环满（含填充开销）：不阻塞返 false

    auto* base = area_of(map_);
    if (pad != 0) {                          // 填充帧占满环尾（头不跨尾，故总长 ≥ 16 可写头）
        FrameHead ph{uint32_t(pad - sizeof(FrameHead)), kKindPad, 0};
        std::memcpy(base + off, &ph, sizeof ph);
    }
    const size_t foff = pad != 0 ? 0 : off;
    const uint64_t seq = std::atomic_ref<uint64_t>(h->next_seq).load(std::memory_order_relaxed);
    std::atomic_ref<uint64_t>(h->next_seq).store(seq + 1, std::memory_order_relaxed);
    FrameHead fh{uint32_t(data.size()), kKindData, seq};
    std::memcpy(base + foff, &fh, sizeof fh);
    std::memcpy(base + foff + sizeof(FrameHead), data.data(), data.size());

    // 发布：数据全落 → 游标可见（seq_cst 与读者的 deliver/write 对偶，杜绝丢唤醒）
    std::atomic_ref<uint64_t>(h->write).store(w + pad + fs, std::memory_order_seq_cst);
    const uint64_t d = std::atomic_ref<uint64_t>(h->deliver).load(std::memory_order_seq_cst);
    if (d == w) ::eventfd_write(efd_, 1);    // 空→非空（读者已取尽此前数据）才醒
    return true;
}

DataBlockView ShmRing::peek() {
    DataBlockView v{nullptr, 0, 0, 0, 0, 0};
    if (!map_) return v;
    auto* h = head_of(map_);
    const size_t cap = map_size_ - sizeof(ShmHead);
    auto* base = area_of(map_);
    const uint64_t w = std::atomic_ref<uint64_t>(h->write).load(std::memory_order_seq_cst);
    for (;;) {
        const uint64_t d = std::atomic_ref<uint64_t>(h->deliver).load(std::memory_order_relaxed);
        if (d == w) { v.pos = d; return v; }  // 取尽：空视图
        const size_t off = size_t(d % cap);
        FrameHead fh;
        std::memcpy(&fh, base + off, sizeof fh);   // 头 16 对齐不跨尾
        if (fh.kind == kKindPad) {           // 填充帧：跳过（deliver 前移，不投递）
            std::atomic_ref<uint64_t>(h->deliver).store(d + sizeof(FrameHead) + fh.len,
                                                        std::memory_order_seq_cst);
            continue;
        }
        v.data = base + off + sizeof(FrameHead);   // 零拷贝直达共享内存
        v.len = fh.len;
        v.seq = fh.seq;
        v.pos = d;
        std::atomic_ref<uint64_t>(h->deliver).store(d + frame_size(fh.len),
                                                    std::memory_order_seq_cst);   // 只投不摘
        return v;
    }
}

void ShmRing::consume(const DataBlockView& v) {
    if (!map_ || v.data == nullptr) return;
    auto* h = head_of(map_);
    const uint64_t nc = v.pos + frame_size(v.len);   // 按块归还（跨尾时一并越过填充帧）
    const uint64_t c = std::atomic_ref<uint64_t>(h->consume).load(std::memory_order_relaxed);
    if (nc > c)   // 单消费者：乱序/重复 release 不回退游标
        std::atomic_ref<uint64_t>(h->consume).store(nc, std::memory_order_release);
}

void ShmRing::skip_seq(uint64_t n) {
    if (!map_) return;
    std::atomic_ref<uint64_t>(head_of(map_)->next_seq)
        .fetch_add(n, std::memory_order_relaxed);
}

int ShmRing::wake_fd() const { return efd_; }

uint64_t ShmRing::delivered() const {
    return map_ ? std::atomic_ref<uint64_t>(head_of(map_)->deliver).load(std::memory_order_seq_cst)
                : 0;
}

uint64_t ShmRing::consumed() const {
    return map_ ? std::atomic_ref<uint64_t>(head_of(map_)->consume).load(std::memory_order_seq_cst)
                : 0;
}

// ===== 机制层内脏 =====

namespace {

PeerRecord* lookup_peer(Ipc& ip, PeerId id) {
    std::lock_guard lk(ip.mu);
    auto it = ip.peers.find(id);
    return it == ip.peers.end() ? nullptr : it->second.get();   // 摘除只在停机后，指针稳定
}

OpCtx make_read_op(Ipc& ip, PeerRecord& rec) {
    OpCtx op{};
    op.kind = IoKind::kSocketRead;
    op.fd = rec.fd;
    op.source = rec.id;                    // 事件回来 ev.source 直接当 PeerId（域看 source）
    op.pipe = ip.inbox;
    op.buf = rec.rbuf.data();
    op.len = rec.rbuf.size();
    op.row = &kOpUringIo;
    return op;
}

PeerId register_peer(Ipc& ip, int fd, PipeId events) {
    PeerId id = 0;
    OpCtx op{};
    {
        std::lock_guard lk(ip.mu);
        id = ip.next_peer++;
        auto rec = std::make_unique<PeerRecord>();
        rec->id = id;
        rec->events = events;
        rec->fd = fd;
        op = make_read_op(ip, *rec);
        rec->read_op = op;
        ip.peers.emplace(id, std::move(rec));
    }
    disp::submit_op(ip.d, op);              // 连接建立即提交首张读申请单（锁外经 01 代交）
    return id;
}

// UDS 写：队列 + 单飞（在飞写缓冲不换，防 01 读单 buf 悬垂）
bool queue_frame(Ipc& ip, PeerId to, std::vector<std::byte> frame) {
    PeerRecord* rec = nullptr;
    {
        std::lock_guard lk(ip.mu);
        auto it = ip.peers.find(to);
        if (it == ip.peers.end() || it->second->dead) return false;
        rec = it->second.get();
    }
    std::lock_guard lk(rec->wmu);
    if (rec->fd < 0) return false;
    if (rec->writing) { rec->wq.push_back(std::move(frame)); return true; }
    rec->wbuf = std::move(frame);
    rec->writing = true;
    OpCtx op{};
    op.kind = IoKind::kSocketWrite;
    op.fd = rec->fd;
    op.source = rec->id;
    op.pipe = ip.inbox;
    op.buf = rec->wbuf.data();
    op.len = rec->wbuf.size();
    op.row = &kOpUringIo;
    disp::submit_op(ip.d, op);
    return true;
}

bool send_msg(Ipc& ip, PeerId to, IpcMessage msg) {
    return queue_frame(ip, to, encode_frame(msg));
}

// 发送失败/超大负载：立即销账（撤超时 + 回填错误响应），不等请求超时
void fill_request(Ipc& ip, RequestEntry& e, int32_t err) {
    const RequestId id = e.id;
    const PipeId reply = e.reply;
    if (e.th) { tmr::cancel(ip.timer, e.th); e.th = 0; }
    ip.pending.erase(e);
    disp::post(ip.d, make_err_event(id, reply, err, clock_ns()));
}

void close_peer(Ipc& ip, PeerId pid, int32_t why) {
    PeerRecord* rec = nullptr;
    {
        std::lock_guard lk(ip.mu);
        auto it = ip.peers.find(pid);
        if (it == ip.peers.end() || it->second->dead) return;
        rec = it->second.get();
        rec->dead = true;
    }
    {
        std::lock_guard lk(rec->wmu);       // 写队列一并作废
        rec->wq.clear();
        rec->writing = false;
    }
    if (rec->fd >= 0) { ::shutdown(rec->fd, SHUT_RDWR); ::close(rec->fd); rec->fd = -1; }

    for (auto& e : ip.pending.take_to(pid)) {           // 未决请求 kBroken 回填
        if (e.th) tmr::cancel(ip.timer, e.th);
        disp::post(ip.d, make_err_event(e.id, e.reply, int32_t(IpcError::kBroken), clock_ns()));
    }

    Event ev{};                                          // kTypeIpcBroken 上报（投该对端 events 管道）
    ev.type = kTypeIpcBroken;
    ev.source = pid;
    ev.pipe = rec->events;
    ev.payload = IpcBrokenPayload{pid, why};
    ev.def = &kTypeIpcBroken;
    disp::post(ip.d, ev);
}

void dispatch_frame(Ipc& ip, PeerId from, PipeId events, const IpcMessage& msg, uint64_t now) {
    switch (msg.head.type) {
    case uint16_t(EventType(kTypeIpcRequest)):
        disp::post(ip.d, to_event(msg, from, events, now));
        break;
    case uint16_t(EventType(kTypeIpcResponse)):           // 销账唯一裁决点 = inbox 消费线程
        with(settle(ip.pending, msg.head.request_id, kOpIpcRequest, &ip.timer),
             [&](RequestEntry& e) {
                 disp::post(ip.d, to_event(msg, from, e.reply, now));
             });
        break;
    case uint16_t(EventType(kTypeIpcNotify)):
        disp::post(ip.d, to_event(msg, from, events, now));
        break;
    default:
        ip.unknown++;                                    // 号段内未识类型：丢弃并计数
        break;
    }
}

void on_read_done(Ipc& ip, const Event& ev) {
    PeerRecord* rec = lookup_peer(ip, ev.source);
    if (!rec) return;
    const auto& io = std::any_cast<IoResult>(ev.payload);
    if (io.bytes == 0) {                                 // EOF：对端关连接
        close_peer(ip, ev.source, 0);
        return;
    }
    rec->acc.insert(rec->acc.end(), rec->rbuf.begin(),
                    rec->rbuf.begin() + std::ptrdiff_t(io.bytes));
    for (;;) {                                           // 按头 payload_len 分帧（粘包/半包攒）
        if (rec->acc.size() < sizeof(IpcHeader)) break;
        IpcHeader h{};
        std::memcpy(&h, rec->acc.data(), sizeof h);
        if (auto e = check_header(h); e != IpcError::kOk) {   // 帧头非法（坏 magic/超长）→ 断连
            close_peer(ip, ev.source, int32_t(e));
            return;
        }
        const size_t total = sizeof(IpcHeader) + h.payload_len;
        if (rec->acc.size() < total) break;              // 半包：等下一读
        auto frame = decode_frame(std::span<const std::byte>(rec->acc.data(), total));
        if (!frame) { close_peer(ip, ev.source, int32_t(frame.error())); return; }
        dispatch_frame(ip, ev.source, rec->events, *frame, clock_ns());
        rec->acc.erase(rec->acc.begin(), rec->acc.begin() + std::ptrdiff_t(total));
        if (rec->dead) return;                           // 分帧中判定断连：不再读不再重挂
    }
    if (rec->dead) return;
    disp::submit_op(ip.d, rec->read_op);                 // 读完成必重挂（rearm）
}

void on_write_done(Ipc& ip, const Event& ev) {
    PeerRecord* rec = lookup_peer(ip, ev.source);
    if (!rec) return;
    std::lock_guard lk(rec->wmu);
    rec->writing = false;
    if (rec->dead || rec->fd < 0 || rec->wq.empty()) { rec->wq.clear(); return; }
    rec->wbuf = std::move(rec->wq.front());              // 写完成 → 释放发送缓冲，续下一批
    rec->wq.pop_front();
    rec->writing = true;
    OpCtx op{};
    op.kind = IoKind::kSocketWrite;
    op.fd = rec->fd;
    op.source = rec->id;
    op.pipe = ip.inbox;
    op.buf = rec->wbuf.data();
    op.len = rec->wbuf.size();
    op.row = &kOpUringIo;
    disp::submit_op(ip.d, op);
}

void drain_ring(Ipc& ip, RingRecord& rr) {
    for (;;) {
        DataBlockView v = rr.ring.peek();                // 只投递不摘（release 才 consume）
        if (!v.data) break;
        v.ring = rr.id;                                  // ring/from 归 RingRecord 层回填
        v.from = rr.peer;
        uint64_t f = 0, t = 0;
        if (seq_gap(rr.expect, v.seq, f, t)) {           // seq 跳号 → 丢块区间
            Event lost{};
            lost.type = kTypeIpcDataLost;
            lost.source = rr.peer;
            lost.pipe = rr.deliver;
            lost.payload = IpcLostPayload{rr.id, f, t};
            lost.def = &kTypeIpcDataLost;
            disp::post(ip.d, lost);
        }
        if (v.seq >= rr.expect) rr.expect = v.seq + 1;
        Event ready{};
        ready.type = kTypeIpcDataReady;
        ready.source = rr.peer;
        ready.pipe = rr.deliver;
        ready.payload = v;                               // 视图直达（零拷贝指向共享内存）
        ready.def = &kTypeIpcDataReady;
        disp::post(ip.d, ready);
    }
}

RingRecord* find_ring(Ipc& ip, RingId id) {
    std::lock_guard lk(ip.mu);
    auto it = ip.rings.find(id);
    return it == ip.rings.end() ? nullptr : it->second.get();
}

// 取环（无则建）：每对端一条，建时首挂 eventfd 读单（空→非空唤醒）
RingRecord* ring_for(Ipc& ip, PeerId to) {
    std::lock_guard lk(ip.mu);
    auto pit = ip.peers.find(to);
    if (pit == ip.peers.end() || pit->second->dead) return nullptr;
    PipeId deliver = pit->second->events;
    auto it = ip.tx_ring.find(to);
    if (it != ip.tx_ring.end()) {
        auto r = ip.rings.find(it->second);
        if (r != ip.rings.end()) return r->second.get();
    }
    RingId rid = ip.next_ring++;
    auto rr = std::make_unique<RingRecord>(rid, to, deliver, kRingBytes);
    RingRecord* raw = rr.get();
    OpCtx w{};
    w.kind = IoKind::kEventFdWake;
    w.fd = raw->ring.wake_fd();
    w.source = to;                                       // 非内部源 → translate 翻 kTypeWake
    w.pipe = ip.inbox;
    w.buf = &raw->wake_buf;
    w.len = sizeof(raw->wake_buf);
    w.row = &kOpUringIo;
    raw->wake = w;
    ip.rings.emplace(rid, std::move(rr));
    ip.tx_ring[to] = rid;
    disp::submit_op(ip.d, w);                            // 首挂（锁外经 01 代交亦可，此处已持锁但不回取）
    return raw;
}

void on_wake(Ipc& ip, const Event& ev) {
    RingRecord* rr = nullptr;
    {
        std::lock_guard lk(ip.mu);
        auto it = ip.tx_ring.find(ev.source);
        if (it != ip.tx_ring.end()) {
            auto r = ip.rings.find(it->second);
            if (r != ip.rings.end()) rr = r->second.get();
        }
    }
    if (!rr) return;
    disp::submit_op(ip.d, rr->wake);                     // 读完成必重挂（rearm）
    drain_ring(ip, *rr);                                 // 就绪块投事件，seq 跳号投 DataLost
}

void on_broken(Ipc& ip, const Event& ev) {
    const auto& io = std::any_cast<IoResult>(ev.payload);
    close_peer(ip, ev.source, io.err);                   // 传输错误 → 归纳成 kTypeIpcBroken
}

void on_timeout(Ipc& ip, RequestId id) {
    with(settle(ip.pending, id, kOpIpcRequest, &ip.timer), [&](RequestEntry& e) {
        disp::post(ip.d, make_timeout_event(id, e.reply, clock_ns()));   // 合成 kTimeout 同形响应
    });                                                  // 落空 = 响应先到：查表拒绝，计数
}

void on_ipc_event(Ipc& ip, const Event& ev) {
    with(ev, {
        {kTypeIoReadDone,   [&](const Event& e){ on_read_done(ip, e); }},
        {kTypeIoWriteDone,  [&](const Event& e){ on_write_done(ip, e); }},
        {kTypeWake,         [&](const Event& e){ on_wake(ip, e); }},
        {kTypeError,        [&](const Event& e){ on_broken(ip, e); }},
        {kTypeIpcTimeout,   [&](const Event& e){
            on_timeout(ip, std::any_cast<IpcTimeoutPayload>(e.payload).id);
        }},
    });
}

void acceptor_loop(Ipc& ip) {                            // 专用线程：poll 监听套接字 + 退出唤醒
    for (;;) {
        const int lfd = ip.listen_fd.load();
        const int sfd = ip.stop_fd.load();
        if (lfd < 0 || sfd < 0) break;
        pollfd fds[2] = {{lfd, POLLIN, 0}, {sfd, POLLIN, 0}};
        const int r = ::poll(fds, 2, -1);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (fds[1].revents) break;                       // 停机唤醒
        if (fds[0].revents & POLLIN) {
            const int cfd = ::accept(lfd, nullptr, nullptr);
            if (cfd >= 0) register_peer(ip, cfd, ip.listen_events);   // accept → 首挂读单
        }
    }
}

}  // namespace（机制层内脏：文件内可见，不跨模块直调）

// ===== 门面 =====

IpcPtr create(Dispatcher& d, Timer& t) {
    IpcPtr ip(new Ipc(d, t));
    ip->inbox = disp::open_pipe(d, [raw = ip.get()](const Event& ev) { on_ipc_event(*raw, ev); });
    return ip;
}

void listen(Ipc& ip, const std::string& path, PipeId events) {
    with(live(ip), [&] {
        if (ip.listen_fd.load() >= 0) return;
        sockaddr_un addr{};
        if (path.size() >= sizeof(addr.sun_path)) return;
        addr.sun_family = AF_UNIX;
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        ::unlink(path.c_str());                          // 清残留套接字
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            ::listen(fd, 16) != 0) {
            ::close(fd);
            return;
        }
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);   // acceptor 走 poll
        const int sfd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);     // 退出唤醒
        ip.listen_fd = fd;
        ip.stop_fd = sfd;
        ip.listen_path = path;
        ip.listen_events = events;
        ip.acceptor = std::thread([&ip] { acceptor_loop(ip); });
    });
}

PeerId connect(Ipc& ip, const std::string& path, PipeId events) {
    return with(live(ip), [&]() -> PeerId {
        sockaddr_un addr{};
        if (path.size() >= sizeof(addr.sun_path)) return 0;
        addr.sun_family = AF_UNIX;
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return 0;
        std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            ::close(fd);
            return 0;
        }
        return register_peer(ip, fd, events);            // 建立即提交首张读申请单
    });
}

RequestId request(Ipc& ip, PeerId to, IpcMessage msg, PipeId reply, uint64_t timeout_ns) {
    // ticket 传空 timer：超时模板要带请求号，号在发号之后才知道——本体拿号自挂（settle 照样撤）
    return with(live(ip),
                ticket(ip.pending, RequestEntry{to, reply}, kOpIpcRequest, nullptr, 0, Event{}),
                [&](RequestEntry& e) -> RequestId {
        const RequestId id = e.id;
        msg.head.type = uint16_t(EventType(kTypeIpcRequest));   // 头填 type/id（error_code 请求忽略）
        msg.head.request_id = id;
        msg.head.error_code = 0;
        if (msg.body.size() > kMaxPayload) {              // 超大负载：不发不等，立即销账回填
            fill_request(ip, e, int32_t(IpcError::kTooLarge));
            return id;
        }
        e.th = tmr::after(ip.timer, timeout_ns, make_timeout_tpl(id, ip.inbox));
        if (!send_msg(ip, to, std::move(msg)))            // 发送失败即销账（kBroken）
            fill_request(ip, e, int32_t(IpcError::kBroken));
        return id;
    });
}

void respond(Ipc& ip, PeerId to, RequestId id, IpcMessage msg, int32_t err) {
    with(live(ip), [&] {
        msg.head.type = uint16_t(EventType(kTypeIpcResponse));   // 头填 type/id/error_code
        msg.head.request_id = id;
        msg.head.error_code = err;
        (void)send_msg(ip, to, std::move(msg));
    });
}

void notify(Ipc& ip, PeerId to, IpcMessage msg) {
    with(live(ip), [&] {
        msg.head.type = uint16_t(EventType(kTypeIpcNotify));     // 单向：id = 0，不登记不等待
        msg.head.request_id = 0;
        msg.head.error_code = 0;
        (void)send_msg(ip, to, std::move(msg));
    });
}

bool write_data(Ipc& ip, PeerId to, std::span<const std::byte> data) {
    return with(live(ip), [&]() -> bool {
        RingRecord* rr = ring_for(ip, to);
        if (!rr) return false;
        std::lock_guard lk(rr->wlock);                   // 同环多线程写自旋锁收敛
        return rr->ring.put(data);
    });
}

void release(Ipc& ip, DataBlockView& v) {
    with(live(ip), [&] {
        RingRecord* rr = find_ring(ip, v.ring);
        if (rr) rr->ring.consume(v);                     // 归还即 consume，槽位复用
    });
}

void stop(Ipc& ip) {
    if (!ip.running.exchange(false)) return;             // 幂等
    const int sfd = ip.stop_fd.load();                   // ① 停 acceptor（先唤醒再等退出）
    if (sfd >= 0) { uint64_t one = 1; (void)::write(sfd, &one, sizeof one); }
    if (ip.acceptor.joinable()) ip.acceptor.join();

    disp::close_pipe(ip.d, ip.inbox);                    // ② 注销 inbox：等在跑的消费批收尾——
                                                         //    此后只有本线程动账本（销账单线程化的收口）

    for (auto& e : ip.pending.take_all()) {              // ③ 摘请求表：未决 kBroken 回填 + 撤超时
        if (e.th) tmr::cancel(ip.timer, e.th);
        disp::post(ip.d, make_err_event(e.id, e.reply, int32_t(IpcError::kBroken), clock_ns()));
    }

    const int lfd = ip.listen_fd.exchange(-1);           // ④ 关连接拆环
    {
        std::lock_guard lk(ip.mu);
        for (auto& [_, rec] : ip.peers) {
            if (rec->fd >= 0) { ::shutdown(rec->fd, SHUT_RDWR); ::close(rec->fd); rec->fd = -1; }
            rec->dead = true;
        }
        ip.rings.clear();
        ip.tx_ring.clear();
    }
    if (lfd >= 0) ::close(lfd);
    if (sfd >= 0) ::close(sfd);
    ip.stop_fd = -1;
    if (!ip.listen_path.empty()) { ::unlink(ip.listen_path.c_str()); ip.listen_path.clear(); }
}

// ===== 白盒观测口（测试用） =====

std::vector<PeerId> peer_ids(Ipc& ip) {
    std::lock_guard lk(ip.mu);
    std::vector<PeerId> out;
    for (auto& [id, rec] : ip.peers)
        if (!rec->dead) out.push_back(id);
    return out;
}

void kill_peer(Ipc& ip, PeerId pid) {                     // 模拟对端断开：shutdown → 双端读单 EOF
    if (PeerRecord* rec = lookup_peer(ip, pid); rec && rec->fd >= 0)
        ::shutdown(rec->fd, SHUT_RDWR);
}

int peer_fd(Ipc& ip, PeerId pid) {
    PeerRecord* rec = lookup_peer(ip, pid);
    return rec ? rec->fd : -1;
}

size_t pending_count(Ipc& ip) { return ip.pending.size(); }
size_t late_response_count(Ipc& ip) { return ip.pending.late_drops; }
size_t unknown_type_count(Ipc& ip) { return ip.unknown.load(); }

void skip_ring_seq(Ipc& ip, PeerId to, uint64_t n) {
    RingRecord* rr = nullptr;
    {
        std::lock_guard lk(ip.mu);
        auto it = ip.tx_ring.find(to);
        if (it != ip.tx_ring.end()) {
            auto r = ip.rings.find(it->second);
            if (r != ip.rings.end()) rr = r->second.get();
        }
    }
    if (rr) rr->ring.skip_seq(n);
}

}  // namespace ipc

// ===== 资源生死（删除器住实现文件：类不完整也能销毁） =====

void IpcDeleter::operator()(Ipc* p) const {
    if (p) {
        ipc::stop(*p);
        delete p;
    }
}
