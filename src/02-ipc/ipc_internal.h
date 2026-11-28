#pragma once

// ===== ipc_internal.h：02 内脏头 =====
// 纯函数 + 库薄壳类型：tests 白盒可见；应用层只 include ipc.h（封装靠文件边界）。
// 编排函数（on_ipc_event / on_read_done / …）随门面落地，本阶段只收纯函数与环薄层。
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

#include "ipc.h"

namespace ipc {

// ===== 纯函数：查询/变换，无副作用，可直接单测 =====

IpcError                            check_header(const IpcHeader&);   // 校验头 → kOk 或错误码
std::expected<IpcMessage, IpcError> decode_frame(std::span<const std::byte>); // 一帧 → 消息；头非法带出错误码
std::vector<std::byte>              encode_frame(const IpcMessage&);  // 消息 → 一帧
Event to_event(const IpcMessage&, PeerId from, PipeId to, uint64_t now_ns); // 消息 → 事件（type 同值，pipe = to）
Event make_timeout_event(RequestId, PipeId, uint64_t);   // 超时合成：kTimeout 响应（与响应同形，投回执管道）

// seq 对账纯函数：读到的 seq 与期望不符 → 丢块区间（左闭右开）；连续返回 false
bool seq_gap(uint64_t expect, uint64_t got, uint64_t& from, uint64_t& to);

// ===== ShmRing：共享内存 SPSC 环薄层（库薄壳：memfd + mmap + eventfd） =====
// 布局：[ShmHead | 数据区（capacity 字节，16 对齐）]；帧 = 16B 帧头（len/kind/seq）+ 负载，
// 帧 16 对齐不跨环尾，尾部不足先写填充帧（kind = kPad，peek 跳过）。
// 三游标（单调字节位，跨进程原子）：write（写侧推进）/ deliver（peek 推进，只投不摘）/
// consume（release 推进，槽位复用）。单写单读：写侧并发由 RingRecord 写锁收敛（02 编排层）。
// 唤醒：put 后 deliver 未越过本块 → 写 eventfd（"空→非空"的保守形，见 ipc.h 头注）。
// peek 回填 data/len/seq/pos；ring/from 归 RingRecord 层（drain）回填——本壳不识对端。
class ShmRing {
public:
    explicit ShmRing(size_t capacity);   // memfd + mmap + eventfd（capacity 向上取 16 对齐，至少一帧）
    ~ShmRing();
    ShmRing(ShmRing&&) noexcept;
    ShmRing& operator=(ShmRing&&) noexcept;
    ShmRing(const ShmRing&) = delete;
    ShmRing& operator=(const ShmRing&) = delete;

    bool          put(std::span<const std::byte>);  // 环满（或单块超环）返回 false，不阻塞；空负载 = 无操作 true
    DataBlockView peek();                           // 取下一块只投递不摘（跳填充帧）；空 = data 空、len 0
    void          consume(const DataBlockView&);    // release 时摘块（按 peek 序归还），槽位复用

    void      skip_seq(uint64_t n);   // 白盒：seq 跳号 n（模拟崩溃重建丢块，对账用例）
    int       wake_fd() const;        // 观测口：eventfd（01 读单挂它；测试读它验唤醒）
    uint64_t  delivered() const;      // 观测口：deliver 游标（单调字节位）
    uint64_t  consumed() const;       // 观测口：consume 游标（单调字节位）

private:
    void* map_       = nullptr;   // ShmHead + 数据区（memfd 映射）
    size_t map_size_ = 0;
    int    memfd_    = -1;
    int    efd_      = -1;
};

// ===== 机制层白盒观测口（测试用；正常路径走门面） =====

std::vector<PeerId> peer_ids(Ipc&);            // 存活对端清单（含 accept/connect 双端）
void      kill_peer(Ipc&, PeerId);             // 模拟对端断开：shutdown 该端 → 双端读单收到 EOF
int       peer_fd(Ipc&, PeerId);               // 暴露 fd（测试直写坏帧等原始字节）
size_t    pending_count(Ipc&);                 // 未决请求数
size_t    late_response_count(Ipc&);           // 迟到响应 settle 拒收计数
size_t    unknown_type_count(Ipc&);            // 号段内未识类型丢弃计数
void      skip_ring_seq(Ipc&, PeerId, uint64_t);// 白盒：该对端环 seq 跳号（模拟崩溃重建丢块）

}  // namespace ipc
