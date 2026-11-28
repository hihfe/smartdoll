#pragma once

// ===== ipc.h：02 进程间通信——契约类型 + 门面（应用层唯一可见面） =====
// 控制面 UDS、数据面 memfd+mmap 共享内存环（薄层自研）、eventfd 只做数据就绪唤醒；
// 消息格式、号段 100–199、请求/响应与错误语义在本模块定死。详见 doc/modules/02-ipc.md。
//
// 落地节奏（相对设计稿的范围偏移，用法一致）：
//   · 本文件 = 完整契约（类型 + 门面声明）；当前落地面 = 纯函数 + ShmRing 环薄层 + 机制编排
//     （UDS 分帧、请求表、超时/断连、环唤醒），白盒见 src/02-ipc/ipc_internal.h；
//     重连退避（kTypeIpcReconnect / Backoff 行）随后续落地。
//   · create 返回 IpcPtr（IpcDeleter 住实现文件，同 DispatcherPtr——类不完整也能销毁）。
//   · listen/connect 增 `events` 参数：入站事件收件管道"注册时声明"（02 文档门面清单漏列，
//     03 文档明写"listen/connect 的 events 参数"、PeerRecord.events 注释亦为"注册时声明"）。
//   · request 的超时不交 ticket 挂：超时模板要带请求号，而号在 ticket 发号之后才知道——
//     ticket 传空 timer，本体拿号后自挂 tmr::after（settle 的 close_entry 按 e.th 照样撤）。
//   · eventfd 唤醒取保守形：读者未取到本块即醒（doc"空→非空"的扩形，闭合"读者追赶中
//     错过唤醒、尾块滞留到下一 put"的窗口），见 ipc_internal.h 的 ShmRing 注。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "disp.h"
#include "tmr.h"
#include "with.h"   // block 切面的 Aspect 形（签名住本文件）

using RequestId = uint64_t;   // 请求-响应关联号；通知类 = 0
using PeerId    = uint32_t;   // 对端进程编号（accept/connect 时分配）；兼任 SourceId，0 = 无效
using RingId    = uint32_t;   // 环编号：每对进程、每方向一条；建环时 02 分配，重连复用（同 PeerId）

// 02 号段 100–199：消息类型 = 事件类型，线上 head.type 与 Event.type 同值。
// 事件定义 = 策略行（policy.h，声明即注册）；本号段的行与负载结构由 02 定义，消费方 any_cast 取回。
inline constexpr EventDef kTypeIpcRequest   {100, kIpc, Qos::kBlock};   // 收到请求（需响应）
inline constexpr EventDef kTypeIpcResponse  {101, kIpc, Qos::kBlock};   // 收到响应（head.error_code 有效）
inline constexpr EventDef kTypeIpcNotify    {102, kIpc, Qos::kBlock};   // 收到通知（request_id = 0）
inline constexpr EventDef kTypeIpcDataReady {103, kIpc, Qos::kBlock};   // 数据面：环非空，负载 = DataBlockView（直达）
inline constexpr EventDef kTypeIpcBroken    {104, kIpc, Qos::kBlock};   // 传输层断连/错误，本层归纳后上报
inline constexpr EventDef kTypeIpcTimeout   {105, kIpc, Qos::kBlock};   // 内部：请求超时触发，只回 02 自己的 inbox，
                                                                       // 业务看到的是 error_code = kTimeout 的响应
inline constexpr EventDef kTypeIpcDataLost  {106, kIpc, Qos::kBlock};   // 数据面：seq 对账发现丢块，投该环 deliver 管道
inline constexpr EventDef kTypeIpcReconnect {107, kIpc, Qos::kBlock};   // 内部：重连退避到期，只回 02 自己的 inbox

inline constexpr uint32_t kMaxPayload = 1u << 20;   // 负载上限 1MiB：decode 先验长度再分配，防对端骗分配

// 协议指纹与版本（线上格式定死；不符即 kBadMagic / kUnsupported 断连）
inline constexpr uint32_t kIpcMagic   = 0x444f4c4c;   // "DOLL"：异种流/残留流（传错 fd、对端重启的半截连接）
inline constexpr uint16_t kIpcVersion = 1;

// 定长头：控制面消息的线上格式，32 字节 POD，UDS/共享内存上直接 memcpy
struct IpcHeader {
    uint32_t magic;         // 协议指纹：不符即 kBadMagic 断连
    uint16_t version;
    uint16_t type;          // 消息类型：取 02 号段常量值（100–199），与 EventType 同值可直接互转
    uint64_t request_id;    // 请求-响应关联；通知类 = 0
    uint32_t payload_len;   // 负载长度 = UDS 分帧依据；上限 kMaxPayload
    int32_t  error_code;    // 响应里回填：0 = 成功；请求/通知忽略
    uint64_t seq;           // 消息序号：防丢/乱序检测、崩溃对账
};
static_assert(sizeof(IpcHeader) == 32);

struct IpcMessage {
    IpcHeader   head;
    std::string body;       // 文本负载（如 JSON），语义由 head.type 决定；序列化约定由 02 定死
};

// 错误码：协议层专用（响应头回填），独立常量空间——不占 EventType 号段，避免撞号。
// kBroken 是"未决请求的回填码"，与 kTypeIpcBroken 事件两回事：事件报连接状态，错误码填响应。
enum class IpcError : int32_t {
    kOk          = 0,
    kBadMagic    = 1,       // 头校验失败（指纹不符 / 头不完整 / 帧长与长度字段不符）
    kUnsupported = 2,       // 版本或类型不识别
    kRejected    = 3,       // 对端业务拒绝
    kTimeout     = 4,       // 响应超时（本层合成）
    kBusy        = 5,       // 背压拒绝
    kBroken      = 6,       // 连接断开（断连时回填未决请求）
    kTooLarge    = 7,       // 负载超 kMaxPayload
};

// 02 的单据行（ticket/settle 查行，行随契约出生）：请求超时由调用方给（kCaller），
// 到期合成 kTimeout 响应投回执管道；迟到响应查表落空丢弃并计数
inline constexpr OpPolicy kOpIpcRequest{OpKind::kIpcRequest, TimeoutSrc::kCaller, 0,
                                        OpTimeoutAct::kSynthReply, OpLateAct::kDropCount};

// ===== 事件负载（02 号段事件的 std::any 载荷） =====

struct IpcMessageView {     // 请求/响应/通知的送达形态
    PeerId      from;
    IpcHeader   head;       // request_id / error_code / seq 都在头里
    std::string body;
};

struct IpcTimeoutPayload {  // kTypeIpcTimeout 负载（只回 inbox）
    RequestId id;           // 只带 id 够用：reply 查请求表（销账都在 inbox 侧）
};

struct IpcBrokenPayload {   // kTypeIpcBroken 负载
    PeerId  peer;
    int32_t why;            // 传输层原因码（来自 IoResult 错误码）
};

struct IpcLostPayload {     // kTypeIpcDataLost 负载：seq 对账丢块区间
    RingId   ring;
    uint64_t from, to;      // 丢的 seq 区间（左闭右开）
};

struct IpcReconnectPayload { // kTypeIpcReconnect 负载（只回 inbox）
    PeerId peer;             // 哪个对端的重连状态机到点
};

struct DataBlockView {      // 数据面块视图：事件负载直达，零拷贝指向共享内存
    const void* data;  size_t len;
    RingId      ring;       // 哪条环：release 据此定位 consume
    PeerId      from;
    uint64_t    seq;        // 块序号：崩溃对账
    size_t      pos;        // 环上位置（单调字节游标）：release 时据此 consume
};                          // 用完必须 release()，见"生命周期"

class Ipc;                  // 资源聚合体（定义在实现文件）：应用层只见名字，用指针/引用持有

struct IpcDeleter {         // 删除器住实现文件（同 DispatcherPtr：类不完整也能销毁）
    void operator()(Ipc*) const;
};
using IpcPtr = std::unique_ptr<Ipc, IpcDeleter>;

// ===== 门面：自由函数操作集，资源引用作第一参数 =====
namespace ipc {
IpcPtr    create(Dispatcher&, Timer&);                       // 资源出生（RAII）
void      listen(Ipc&, const std::string& path, PipeId events);   // 主进程 listen + 收连接：每 accept 即提交首张读申请单
PeerId    connect(Ipc&, const std::string& path, PipeId events);  // 子进程 connect：建立即提交首张读申请单；断线重连（指数退避）
RequestId request(Ipc&, PeerId to, IpcMessage, PipeId reply, uint64_t timeout_ns); // 发请求（ticket）
void      respond(Ipc&, PeerId to, RequestId, IpcMessage, int32_t err);            // 回响应：头填 id 与 error_code
void      notify(Ipc&, PeerId to, IpcMessage);                                     // 单向通知：id = 0，不登记不等待
bool      write_data(Ipc&, PeerId to, std::span<const std::byte>);                 // 写环，环满返回 false
void      release(Ipc&, DataBlockView&);                                           // 归还即 consume
void      stop(Ipc&);                                                              // 资源死：摘请求表（未决 kBroken 回填）
                                                                            // → 关连接拆环，之后门面调用一律拒绝（live）

// ===== 数据面块切面：block 的定义处（Aspect 契约见 ../with.md） =====
Aspect block(Ipc&, DataBlockView&);   // state = Ipc + 块视图；with(block(ip, view), 本体)——
                                      // 本体只读 data/len，exit 恒 release()
}
