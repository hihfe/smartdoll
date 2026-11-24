# 智能玩偶

## 环境
运行环境：Ubuntu24
语言：c++24，lua

## 功能
1. 网络连接外部终端
2. 接收终端的语音消息，并回复语音
3. 接收终端的事件，并根据需要转发

## 设计

### 进程设计
共四个进程
- 网络服务
- ai 服务
- 翻译引擎
- 策略引擎
> 非语音为文本流，语音追加二进制 PCM 音频

#### 网络服务
负责收发信息

```cpp
// 消息头
struct MessageHeader {
    std::string source;      // 来源标识（终端 ID 或服务名）
    std::string msg_type;    // "voice" / "event" / "command"
    int64_t timestamp;       // 毫秒时间戳
};

// 语音元数据（进 JSON）
struct VoiceMeta {
    std::string format;          // 固定 "pcm"
    int sample_rate;             // 采样率（Hz）
    int channels;                // 声道数
    int bits_per_sample;         // 位深
    int sequence;                // 分块序号，从 0 开始
    bool is_final;               // 整句结束标志（非块结束标志）
};

// 语音负载：元数据 + 音频 blob 分离
struct VoicePayload {
    VoiceMeta meta;                    // 进 JSON
    std::vector<uint8_t> audio_data;   // 走帧尾 blob，不进 JSON
};

// 事件负载
struct EventPayload {
    std::string event_name;      // 事件名称
    std::string payload_json;    // 附加数据（JSON 字符串）
};

// 命令负载
struct CommandPayload {
    std::string command;         // 命令名称
    std::string params_json;     // 参数（JSON 字符串）
};

// 消息体（多态）
using MessageBody = std::variant<VoicePayload, EventPayload, CommandPayload>;

// 输入消息（终端 → 网络服务 → 翻译引擎）
struct IncomingMessage {
    MessageHeader header;
    MessageBody body;
};

// 路由决策（翻译引擎内部使用，不进入 wire format）
struct RouteDecision {
    std::string target_id;        // 目标终端 ID
    std::string transformed_type; // 转换后的消息类型（可选）
    bool should_forward;          // 是否转发
};

// 输出消息（翻译引擎 → 网络服务 → 终端）
struct OutgoingMessage {
    std::string target_id;        // 目标终端 ID，网络服务据此投递（序列化为顶层 "target"）
    MessageHeader header;
    MessageBody body;
};
```

#### 线格式（Wire Format）

进程间通过 socket 传递消息。非语音消息为纯文本，语音消息采用「定长前缀 + JSON 元数据 + 裸 PCM 音频」的帧结构。

##### 帧布局

```
[4B total_len （大端）]        // = 12 + json_len + audio_len
[4B json_len  （大端）]        // JSON 元数据长度
[4B audio_len （大端）]        // 音频裸字节长度，无音频时为 0
[JSON: 元数据（Header + Body；出站帧另含 target），不含 audio_data]
[audio: 裸 PCM 字节，长度可为 0]
```

规则：
- 所有整数统一大端（网络序）。
- 所有消息都必须写 `audio_len`，非语音消息为 0，保证帧结构统一。
- 音频参数（采样率/声道/位深）由 VoiceMeta 提供，音频 blob 为裸 PCM。
- JSON 中不包含 `audio_data`，音频以裸字节形式附在帧尾。
- 校验 `total_len == 12 + json_len + audio_len`。
- `json_len` / `audio_len` 设上限，防止畸形长度导致巨额分配。
- `VoicePayload.audio_data` 走帧尾 blob，不进 JSON 序列化。
- 出站帧 JSON 顶层含 `target`（目标终端 ID），由网络服务消费后投递；入站帧不含该字段，`RouteDecision` 本身不上线格式。

#### 翻译引擎
接收消息并处理。内部数据流分三层，每层只回答自己的问题，错误在最早的一层拦截：

| 层 | 结构 | 回答的问题 | 生命周期 |
|---|---|---|---|
| 帧层 | `Frame` / `FrameHeader` | 字节对不对？ | 只活在 socket 读写边界 |
| 消息层 | `IncomingMessage` | 业务上这是什么消息？ | 活在翻译引擎内部 |
| 路由层 | `RouteDecision` | 该往哪去？ | 只活在 `route()` 调用内，不序列化 |

数据结构：

```cpp
// ========== 帧层：只活在 socket 读写边界 ==========

// 从每帧前 12 字节解析（全大端）
// 双向 socket 不需要显式方向字段：网络服务只发上游帧、
// 翻译引擎只发下游帧，方向由"谁在读"隐含。
struct FrameHeader {
    uint32_t total_len;   // = 12 + json_len + audio_len，用于校验
    uint32_t json_len;    // JSON 元数据长度
    uint32_t audio_len;   // 裸 PCM 长度，非语音为 0
};

// 读写缓冲：一帧的原始数据（解析前/序列化前的中间形态）
struct Frame {
    FrameHeader head;
    std::string json;                  // json_len 字节
    std::vector<uint8_t> audio;        // audio_len 字节（仅语音）
};
```

> 消息层结构（`MessageHeader`、`MessageBody`、`IncomingMessage`、`RouteDecision`、`OutgoingMessage`）定义见上文「网络服务」一节。

处理流水线：先解析成 `IncomingMessage`，再路由，最后构造出站消息。

```cpp
std::optional<Frame>           read_frame(int fd);               // 读 12B 帧头 → 校验 → 读 json/audio
std::optional<IncomingMessage> parse_incoming(const Frame& f);   // JSON + blob → 填充 variant
std::optional<RouteDecision>   route(const IncomingMessage& in); // 内部策略查询，纯函数
std::optional<OutgoingMessage> build_outgoing(const IncomingMessage& in,
                                               const RouteDecision& rd);
bool write_frame(int fd, const OutgoingMessage& out);            // 序列化 + 追加 PCM + 写 socket
```

```
socket → Frame → IncomingMessage → route() → RouteDecision
                                          ↓
socket ← Frame ← OutgoingMessage ← build_outgoing()
```

规则：
- 帧头先到先校验，通过后才分配内存；校验失败丢弃整帧，字节不流入消息层。
- 解析或路由失败返回空 `std::optional`，丢弃该消息，不用异常。
- `IncomingMessage` 与 `OutgoingMessage` 是两个独立类型：方向即类型，防止入站消息未经转换直接发出。
- `route()` 是纯函数、无 I/O，策略变化不影响序列化层，便于单测。

#### ai 服务
负责处理语音消息

#### 策略引擎
调配策略，组合服务
