// 通用「字节预算」LRU 缓存 — ImageCache / GifCache 共用。
//
// 设计目标（见 tmp/image-opt/PLAN-client_design.md §3）：
//   - list<Node> 维护 MRU→LRU 顺序 + unordered_map<Key,iterator> O(1) 命中/移动。
//   - 每个节点带 bytes；总字节超预算时从冷端(back)逐出。
//   - 逐出**只**在 evictToBudget() 里发生（调用方保证不在 paint 期间调用，避免把
//     本帧正被 DrawBitmap 引用的 ID2D1Bitmap 释放成悬垂指针 —— 详见 image_cache.h）。
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <cstddef>
#include <list>
#include <unordered_map>
#include <utility>

namespace launcher::d2d {

template <class Key, class Value, class Hash = std::hash<Key>,
          class KeyEq = std::equal_to<Key>>
class LruCache {
public:
    struct Node {
        Key    key;
        Value  value;
        size_t bytes = 0;
    };

    // 命中 → 移到 MRU 端并返回可变指针；未命中返回 nullptr。
    Value* get(const Key& k) {
        auto it = index_.find(k);
        if (it == index_.end()) return nullptr;
        nodes_.splice(nodes_.begin(), nodes_, it->second);
        return &it->second->value;
    }
    // 不改动 LRU 顺序的只读查找（drain 时按 key 定位用）。
    Value* peek(const Key& k) {
        auto it = index_.find(k);
        if (it == index_.end()) return nullptr;
        return &it->second->value;
    }

    // 插入/覆盖到 MRU 端。返回新值的可变指针。
    Value* put(const Key& k, Value&& v, size_t bytes) {
        auto it = index_.find(k);
        if (it != index_.end()) {
            total_ -= it->second->bytes;
            it->second->value = std::move(v);
            it->second->bytes = bytes;
            total_ += bytes;
            nodes_.splice(nodes_.begin(), nodes_, it->second);
            return &it->second->value;
        }
        nodes_.push_front(Node{ k, std::move(v), bytes });
        index_.emplace(k, nodes_.begin());
        total_ += bytes;
        return &nodes_.begin()->value;
    }

    // 更新已存在节点的字节计（Pending→Ready 时把预留 0 字节改成真实位图字节）。
    void setBytes(const Key& k, size_t bytes) {
        auto it = index_.find(k);
        if (it == index_.end()) return;
        total_ -= it->second->bytes;
        it->second->bytes = bytes;
        total_ += bytes;
    }

    // 逐出冷端直到 total <= budget。onEvict(key,value) 在真正 erase 前回调。
    // 至少保留 1 个节点（避免刚插入的项立刻被自己逐出）。
    template <class OnEvict>
    void evictToBudget(size_t budget, OnEvict&& onEvict) {
        while (total_ > budget && nodes_.size() > 1) {
            Node& back = nodes_.back();
            onEvict(back.key, back.value);
            total_ -= back.bytes;
            index_.erase(back.key);
            nodes_.pop_back();
        }
    }

    // 删除单个 key（存在则回调 onEvict）。
    template <class OnEvict>
    void erase(const Key& k, OnEvict&& onEvict) {
        auto it = index_.find(k);
        if (it == index_.end()) return;
        onEvict(it->second->key, it->second->value);
        total_ -= it->second->bytes;
        nodes_.erase(it->second);
        index_.erase(it);
    }

    // 删除所有满足 pred(key) 的节点（evict(path) 用：丢掉某路径的全部 targetPx 变体）。
    template <class Pred, class OnEvict>
    void eraseIf(Pred&& pred, OnEvict&& onEvict) {
        for (auto it = nodes_.begin(); it != nodes_.end();) {
            if (pred(it->key)) {
                onEvict(it->key, it->value);
                total_ -= it->bytes;
                index_.erase(it->key);
                it = nodes_.erase(it);
            } else {
                ++it;
            }
        }
    }

    template <class OnEvict>
    void clear(OnEvict&& onEvict) {
        for (auto& n : nodes_) onEvict(n.key, n.value);
        nodes_.clear();
        index_.clear();
        total_ = 0;
    }

    size_t bytes() const { return total_; }
    size_t size()  const { return index_.size(); }

private:
    std::list<Node> nodes_;                                       // front = MRU
    std::unordered_map<Key, typename std::list<Node>::iterator,
                       Hash, KeyEq> index_;
    size_t total_ = 0;
};

}  // namespace launcher::d2d
