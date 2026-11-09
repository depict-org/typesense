#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

// Owned by one ordinary multi_search invocation, accessed by its serial leg thread.
// Values never alias the arrays owned by a leg's sort_by::eval_t.
class eval_membership_cache_t {
    struct key_t {
        uint64_t incarnation;
        std::string expression;

        bool operator==(const key_t& other) const {
            return incarnation == other.incarnation && expression == other.expression;
        }
    };

    struct key_hash_t {
        size_t operator()(const key_t& key) const {
            return std::hash<std::string>{}(key.expression) ^ std::hash<uint64_t>{}(key.incarnation);
        }
    };

    struct entry_t {
        uint64_t generation;
        std::vector<uint32_t> ids;
    };

    const size_t max_bytes;
    const size_t max_entries;
    size_t retained_bytes = 0;
    size_t hit_count = 0;
    size_t admission_count = 0;
    std::unordered_map<key_t, entry_t, key_hash_t> entries;

public:
    explicit eval_membership_cache_t(size_t max_bytes = 8 * 1024 * 1024, size_t max_entries = 256)
        : max_bytes(max_bytes), max_entries(max_entries) {}

    eval_membership_cache_t(const eval_membership_cache_t&) = delete;
    eval_membership_cache_t& operator=(const eval_membership_cache_t&) = delete;

    // Called while holding this Index's shared lock, before consulting its entries.
    void prepare(uint64_t incarnation, uint64_t generation) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->first.incarnation == incarnation && it->second.generation != generation) {
                retained_bytes -= it->first.expression.size() + it->second.ids.size() * sizeof(uint32_t);
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool copy(uint64_t incarnation, uint64_t generation, const std::string& expression,
              uint32_t*& ids, uint32_t& count) {
        // Key construction can allocate. This optional optimization must not turn an
        // otherwise successful leg into an error when its extra allocation fails.
        try {
            auto it = entries.find({incarnation, expression});
            if (it == entries.end() || it->second.generation != generation) {
                return false;
            }
            const auto& stored = it->second.ids;
            std::unique_ptr<uint32_t[]> result(new uint32_t[stored.size()]);
            std::copy(stored.begin(), stored.end(), result.get());
            count = stored.size();
            ids = result.release();
            ++hit_count;
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    void admit(uint64_t incarnation, uint64_t generation, const std::string& expression,
               const uint32_t* ids, uint32_t count) {
        if (entries.size() >= max_entries || expression.size() > max_bytes - retained_bytes ||
            count > (max_bytes - retained_bytes - expression.size()) / sizeof(uint32_t)) {
            return;
        }
        try {
            entry_t entry{generation, {}};
            if (count != 0) {
                entry.ids.assign(ids, ids + count);
            }
            if (entries.emplace(key_t{incarnation, expression}, std::move(entry)).second) {
                retained_bytes += expression.size() + size_t(count) * sizeof(uint32_t);
                ++admission_count;
            }
        } catch (const std::bad_alloc&) {
            // Keep the leg's already materialized result; admission is optional.
        }
    }

    size_t size() const { return entries.size(); }
    size_t bytes() const { return retained_bytes; }
    size_t hits() const { return hit_count; }
    size_t admissions() const { return admission_count; }
};
