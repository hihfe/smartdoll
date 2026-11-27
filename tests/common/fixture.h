// tests/common：机制测试夹具——后台事件循环、事件收集管道、带期限等待。
#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "disp.h"

namespace doll::test {

using Clock = std::chrono::steady_clock;

inline std::chrono::milliseconds ms(int v) { return std::chrono::milliseconds(v); }

// 带期限轮询等待：谓词成立即真，到期复核一次
template <class Pred>
bool wait_until(Pred&& pred, std::chrono::milliseconds timeout) {
    auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(ms(1));
    }
    return pred();
}

// 测试事件构造（全字段赋值，避免聚合缺字段警告）
inline Event mk_event(EventType type, SourceId src, PipeId pipe, const EventDef* def = nullptr) {
    Event ev{};
    ev.type = type;
    ev.source = src;
    ev.pipe = pipe;
    ev.def = def;
    return ev;
}

// 事件收集管道：open_pipe 的消费者把事件收进盒里，测试线程等条件
class EventBox {
public:
    explicit EventBox(Dispatcher& d) : d_(&d) {
        pipe_ = disp::open_pipe(d, [this](const Event& ev) {
            std::lock_guard lk(mu_);
            evs_.push_back(ev);
            cv_.notify_all();
        });
    }
    ~EventBox() { disp::close_pipe(*d_, pipe_); }   // 注销并等在跑的消费批收尾（防闭包悬垂）

    PipeId pipe() const { return pipe_; }

    bool wait_count(size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return evs_.size() >= n; });
    }

    bool wait_type(EventType t, std::chrono::milliseconds timeout) {
        std::unique_lock lk(mu_);
        return cv_.wait_for(lk, timeout, [&] {
            for (auto& e : evs_)
                if (e.type == t) return true;
            return false;
        });
    }

    std::vector<Event> all() const {
        std::lock_guard lk(mu_);
        return evs_;
    }

    size_t count(EventType t) const {
        std::lock_guard lk(mu_);
        size_t n = 0;
        for (auto& e : evs_)
            if (e.type == t) ++n;
        return n;
    }

private:
    Dispatcher* d_ = nullptr;
    PipeId pipe_ = 0;
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    std::vector<Event> evs_;
};

// 后台事件循环夹具：构造即起线程跑 disp::run，析构 stop + join
struct LoopFixture {
    DispatcherPtr d;
    std::thread loop;

    LoopFixture() : d(disp::create()) {
        loop = std::thread([this] { disp::run(*d); });
    }
    ~LoopFixture() {
        disp::stop(*d);
        if (loop.joinable()) loop.join();
    }
};

}  // namespace doll::test
