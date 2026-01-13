#pragma once

#include <crate/crate_export.h>
#include <list>
#include <unordered_map>
#include <optional>
#include <cstddef>

namespace crate {
    /// Simple LRU (Least Recently Used) cache
    /// @tparam Key The key type (must be hashable)
    /// @tparam Value The value type
    template<typename Key, typename Value>
    class CRATE_EXPORT lru_cache {
        public:
            /// Create an LRU cache with the specified capacity
            /// @param capacity Maximum number of entries (0 = unlimited)
            explicit lru_cache(size_t capacity = 0)
                : capacity_(capacity) {
            }

            /// Get a value from the cache
            /// @param key The key to look up
            /// @return The value if found, or nullopt
            std::optional <Value*> get(const Key& key) {
                auto it = map_.find(key);
                if (it == map_.end()) {
                    return std::nullopt;
                }
                // Move to front (most recently used)
                list_.splice(list_.begin(), list_, it->second);
                return &it->second->second;
            }

            /// Insert or update a value in the cache
            /// @param key The key
            /// @param value The value to store
            void put(const Key& key, Value value) {
                auto it = map_.find(key);
                if (it != map_.end()) {
                    // Update existing entry and move to front
                    it->second->second = std::move(value);
                    list_.splice(list_.begin(), list_, it->second);
                    return;
                }

                // Evict oldest entry if at capacity
                if (capacity_ > 0 && map_.size() >= capacity_) {
                    auto& oldest = list_.back();
                    map_.erase(oldest.first);
                    list_.pop_back();
                }

                // Insert new entry at front
                list_.emplace_front(key, std::move(value));
                map_[key] = list_.begin();
            }

            /// Check if a key exists in the cache
            [[nodiscard]] bool contains(const Key& key) const {
                return map_.find(key) != map_.end();
            }

            /// Get current number of entries
            [[nodiscard]] size_t size() const { return map_.size(); }

            /// Get capacity
            [[nodiscard]] size_t capacity() const { return capacity_; }

            /// Clear the cache
            void clear() {
                list_.clear();
                map_.clear();
            }

            /// Set new capacity (may evict entries)
            void set_capacity(size_t capacity) {
                capacity_ = capacity;
                while (capacity_ > 0 && map_.size() > capacity_) {
                    auto& oldest = list_.back();
                    map_.erase(oldest.first);
                    list_.pop_back();
                }
            }

        private:
            using ListType = std::list <std::pair <Key, Value>>;
            using MapType = std::unordered_map <Key, typename ListType::iterator>;

            size_t capacity_;
            ListType list_; // Front = most recently used
            MapType map_;
    };
} // namespace crate
