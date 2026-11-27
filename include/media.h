#pragma once

// ===== media.h：共享语音契约（纯头文件：没有实现、没有进程、不占模块号） =====
// 语音类型与传输属性的唯一定义处——03 / 05 / 06 / M2 共同引用。
// 定义权在此，解释权在 06（sequence/is_final 产生）与 M2（选路/节奏）；
// 传输层只搬运、不解释。详见 doc/media.md。

#include <cstdint>

struct AudioFormat {          // 音频参数：05 设备侧与 06 语音前端共用
    uint32_t sample_rate;     // Hz
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t period_frames;   // 一拍帧数：流式时延预算粒度
};

struct VoiceMeta {            // 语音元数据：终端线格式的 JSON 字段；03 只搬运不解释
    uint32_t sample_rate;     // Hz
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t sequence;        // 分块序号，从 0 开始
    uint8_t  is_final;        // 整句结束标志（非块结束标志）：含义归 06/M2
};

// 大块负载的种类（不透明编号）：语义由生产与消费双方约定，传输层不解释。
constexpr uint32_t kKindVoicePcm = 1;   // 终端语音：负载 = 语音头 + 裸 PCM

// 背压归属：由块的生产者声明，传输层按声明执行，不按类型猜。
constexpr uint8_t kQosCritical = 1;     // 关键：阻塞投递方，不许丢（控制、回执）
constexpr uint8_t kQosRealtime = 2;     // 实时：丢旧留新 + 计数（语音、视频）
