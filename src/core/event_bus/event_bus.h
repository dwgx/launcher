#pragma once

// 类型擦除事件总线，线程安全。core 通过它通知 UI；UI 不持有 core 引用。
//
// 用法：
//   bus.subscribe<SubscriptionUpdated>([](const auto& e){...});
//   bus.publish(SubscriptionUpdated{...});

#include "app/common.h"
#include <functional>
#include <mutex>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace launcher::core {

class EventBus {
public:
    static EventBus& instance();

    template <typename E>
    void subscribe(std::function<void(const E&)> fn) {
        auto wrap = [fn = std::move(fn)](const void* p) {
            fn(*static_cast<const E*>(p));
        };
        std::lock_guard<std::mutex> g(m_mtx);
        m_subs[std::type_index(typeid(E))].push_back(std::move(wrap));
    }

    template <typename E>
    void publish(const E& e) {
        std::vector<std::function<void(const void*)>> snapshot;
        {
            std::lock_guard<std::mutex> g(m_mtx);
            auto it = m_subs.find(std::type_index(typeid(E)));
            if (it == m_subs.end()) return;
            snapshot = it->second;
        }
        for (auto& fn : snapshot) fn(&e);
    }

private:
    EventBus() = default;
    LAUNCHER_DISALLOW_COPY(EventBus);

    std::mutex m_mtx;
    std::unordered_map<std::type_index,
                       std::vector<std::function<void(const void*)>>> m_subs;
};

}  // namespace launcher::core
