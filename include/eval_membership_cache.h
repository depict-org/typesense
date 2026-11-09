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
public:
    // Bound added work as well as retained memory. Exhaustion always computes
    // the ordinary result; these budgets are never reset by generation changes.
    static constexpr size_t MAX_COPY_BYTES = 4 * 1024 * 1024;
    static constexpr size_t MAX_ENTRY_BYTES = 1024 * 1024;
    static constexpr size_t MAX_LOOKUPS = 128;
    static constexpr size_t MAX_EXPRESSION_BYTES = 16 * 1024;
    // One unit per AST node and per leaf value visited.
    static constexpr size_t MAX_ELIGIBILITY_WORK = 8192;

private:
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

    const size_t copy_limit, entry_limit, lookup_limit, expression_limit, eligibility_limit;
    size_t retained_bytes = 0;
    size_t hit_count = 0;
    size_t admission_count = 0;
    size_t copied_bytes = 0;
    size_t lookups = 0;
    size_t eligibility_work = 0;
    std::unordered_map<key_t, entry_t, key_hash_t> entries;

public:
    explicit eval_membership_cache_t(size_t copy_limit = MAX_COPY_BYTES, size_t entry_limit = MAX_ENTRY_BYTES,
                                     size_t lookup_limit = MAX_LOOKUPS, size_t expression_limit = MAX_EXPRESSION_BYTES,
                                     size_t eligibility_limit = MAX_ELIGIBILITY_WORK)
        : copy_limit(copy_limit), entry_limit(entry_limit), lookup_limit(lookup_limit),
          expression_limit(expression_limit), eligibility_limit(eligibility_limit) {}

    eval_membership_cache_t(const eval_membership_cache_t&) = delete;
    eval_membership_cache_t& operator=(const eval_membership_cache_t&) = delete;

    bool can_consult(const std::string& expression) {
        if (copied_bytes >= copy_limit || lookups >= lookup_limit ||
            eligibility_work >= eligibility_limit || expression.size() > expression_limit) {
            return false;
        }
        ++lookups;
        return true;
    }

    bool consume_eligibility_work() {
        if (eligibility_work >= eligibility_limit) return false;
        ++eligibility_work;
        return true;
    }

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
        if (entries.empty() || expression.size() > expression_limit) return false;
        try {
            auto it = entries.find({incarnation, expression});
            if (it == entries.end() || it->second.generation != generation) {
                return false;
            }
            const auto& stored = it->second.ids;
            const auto bytes = stored.size() * sizeof(uint32_t);
            if (bytes > copy_limit - copied_bytes) return false;
            std::unique_ptr<uint32_t[]> result(new uint32_t[stored.size()]);
            std::copy(stored.begin(), stored.end(), result.get());
            count = stored.size();
            ids = result.release();
            ++hit_count;
            copied_bytes += bytes;
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    void admit(uint64_t incarnation, uint64_t generation, const std::string& expression,
               const uint32_t* ids, uint32_t count) {
        if (expression.size() > expression_limit || count > entry_limit / sizeof(uint32_t) ||
            count > (copy_limit - copied_bytes) / sizeof(uint32_t)) {
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
                copied_bytes += size_t(count) * sizeof(uint32_t);
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
