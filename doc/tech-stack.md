# 技术选型

> 智能玩偶项目（Ubuntu24，C++24 + Lua）的开源库选型记录。
> 模块设计文档（`doc/modules/*`）引用本文档，不重复选型。
> 原则：底座力学件（事件源、队列、线程池、定时器）全部用开源库，自研代码只做编排与业务。

## 选型表

| 模块 | 选型 | 协议 | 形态 | 说明 |
|---|---|---|---|---|
| io_uring 事件源 | **liburing** | MIT | 动态库 | 异步完成事件（CQE）来源，架构图中的 `Uring` |
| 无锁队列（queue / 管道） | **moodycamel::ConcurrentQueue** | BSD-2 / CC0 | header-only | 多生产者多消费者无锁队列，事实标准 |
| 线程池 | **BS::thread_pool** | MIT | header-only | 消费管道任务，极简任务队列 + 工作线程模型 |
| 定时器（时间堆） | **Boost.Heap** | BSL-1.0 | header-only | `d_ary_heap` + 句柄，支持到期项删除/更新；到期唤醒用 timerfd 设时 + 常驻读申请单与事件循环集成 |
| IPC：控制指令 | Unix Domain Socket | — | 原生 | 主进程 ↔ AI 进程 / 仿生进程的控制指令 |
| IPC：音频流等大数据 | memfd_create + mmap 共享内存 | — | 原生 | Zero-copy，共享内存内放无锁环形队列（薄层自研，语义对齐 moodycamel） |
| IPC：事件唤醒 | eventfd | — | 原生 | 唤醒事件循环，约 20 行封装，不引库 |
| 定时唤醒 | timerfd | — | 原生 | 04 按堆顶 `timerfd_settime` 设时，01 常驻读申请单，到期读完成进统一收货口 |
| 设备 IO（I2C/SPI/GPIO） | **libperiphery** | MIT | 动态库 | 专为 Linux 外设设计的干净 C 库 |
| 音频采集 / 播放 | **ALSA**（备选 miniaudio） | LGPL / MIT-0 | 动态库 / header-only | Ubuntu24 默认 PipeWire，底层兼容 ALSA |
| ASR / TTS | **sherpa-onnx** | Apache-2.0 | 动态库 | 本地推理，一站式语音识别 + 合成，C++ API |
| LLM 对接 | 原生 socket + **nlohmann/json** | MIT | header-only | 协议简单，不引重库 |
| Lua 绑定 | **sol2** | MIT | header-only | C++24 与 Lua 互调 |
| 测试 | **Catch2** | BSL-1.0 | 静态库 | 单元测试 |
| 构建 | **CMake** | BSD-3 | 工具 | — |

所有依赖均为宽松协议（MIT / BSD / Apache / BSL），商用无压力。

## 关键选型说明

### 四大底座件（全部引库，不自研）

| 件 | 职责 | 为什么选它 |
|---|---|---|
| liburing | 异步事件源 | 架构图的 `Uring` 就是 io_uring，liburing 是其官方用户态库 |
| moodycamel::ConcurrentQueue | queue 与管道 | 无锁并发队列性能标杆，header-only，MPMC 场景覆盖事件分发与管道消费 |
| BS::thread_pool | 任务执行 | 纯"任务队列 + 工作线程"，与"管道消费"模型一一对应；比 Taskflow 概念轻（流水线无复杂 DAG 依赖） |
| Boost.Heap | 定时器管理 | `d_ary_heap` 配 handle 可 O(log n) 删除/更新到期项，正是时间堆的教科书实现；与事件循环的集成点是最近到期点 `timerfd_settime` 设时 + 常驻读申请单 |

### 时间堆与事件循环的集成

- 时间堆（04 持有）只管理"什么时候到期"，不自己睡眠、不自己唤醒；
- 04 取堆顶到期时刻，`timerfd_settime(TFD_TIMER_ABSTIME, CLOCK_MONOTONIC)` 设时；timerfd 归 04（RAII），每进程一个、全部定时器共用；
- 01 对该 fd 常驻一张 read 申请单：到期 fd 可读 → 读完成项照常从统一收货口出来 → `kTypeTimeout` 投 04 的 inbox；
- 04 弹堆、投用户事件，按新堆顶重设 timerfd 并重挂 read 申请单；读回的过期计数（uint64）就是"跳过积压"的现成信号；
- 动堆（改期/取消/暂停/恢复）= `timerfd_settime` 原子替换（堆空清零停表），读申请单不动——无在飞 SQE 撤销、无迟到完成项分支；机制句柄（堆内位置）由 Boost.Heap 的 `handle_type` 提供，语义句柄 `TimerHandle` 是 04 自己的单调发号（不出模块边界）。

### 共享内存环形队列

- Zero-copy 的做法是"共享内存里放无锁环形队列"；
- 跨进程的内存序与进程崩溃恢复比进程内复杂，故此环为自研薄层（SPSC 优先）；
- eventfd 负责"环从空到非空 / 从满到非空"的唤醒，数据搬运不经 socket。

## 依赖引入方式

| 形态 | 库 | 引入方式 |
|---|---|---|
| header-only | moodycamel、BS::thread_pool、Boost.Heap、nlohmann/json、sol2 | CMake `FetchContent` 或 vendored 子目录 |
| 动态库 | liburing、libperiphery、ALSA、sherpa-onnx | 系统包 / 本地编译安装，CMake `find_package` 或 `pkg-config` |

## 备选与放弃记录

| 方案 | 放弃原因 |
|---|---|
| Asio 事件平台 | Proactor 语义原生、生态熟，但底层是 epoll 而非 io_uring，与架构图 `Uring` 不符；其 io_uring 支持成熟度存疑 |
| libuv | 老牌稳定，但同样是 epoll，且 C 风格回调与 C++24 气质不合 |
| Taskflow | 支持任务依赖图，但当前流水线无复杂 DAG，概念偏重；若后续流水线出现阶段依赖再启用 |
| io_uring 超时替代时间堆 | 代码最少，但无"时间堆"结构，取消/批量更新到期项时不如有句柄的堆；作为兜底方案保留 |
| `IORING_OP_TIMEOUT` 挂堆顶 | 改期/取消需撤在飞 SQE（churn + 迟到完成项分支）；timerfd `timerfd_settime` 原子改期更简，读完成项仍走统一收货口 |
| miniaudio（音频） | 备选；若 ALSA 直调复杂度超预期则切换，MIT-0 协议 |
