#pragma once

#include <atomic>
#include <cstdint>

namespace paramcad {

using ObjectId = std::uint64_t;
constexpr ObjectId kInvalidObjectId = 0;

class ObjectIdGenerator {
public:
    static ObjectId Next() {
        static std::atomic<ObjectId> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace paramcad
