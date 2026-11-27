#pragma once

// ===== disp_internal.h：01 内脏头 =====
// 纯函数 + 内部薄壳类型：tests 白盒可见；应用层只 include disp.h（封装靠文件边界）。
#include <cstddef>
#include <cstdint>
#include <optional>

#include "disp.h"

namespace disp {

struct Cqe {                 // 完成项薄壳（liburing 的 cqe 封在实现文件，白盒用此形）
    int64_t  res;
    uint64_t user_data;
};

// 票根 + 完成项 → 事件：翻译、错误拦截都在此，不碰账本（取消丢弃住 settle 的拒绝路径）
std::optional<Event> translate(const Cqe&, const OpCtx&, uint64_t now_ns);

// 观测口（测试用）
size_t late_drop_count(Dispatcher&);   // settle 拒绝路计数（迟到/作废）
size_t drop_count(Dispatcher&);        // 背压丢弃计数（含无主管道）

}
