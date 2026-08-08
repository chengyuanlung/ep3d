#pragma once

#include <atomic>
#include <cstdint>

namespace paramcad {

using ObjectId = std::uint64_t;
constexpr ObjectId kInvalidObjectId = 0;
// Largest id a restore path may inject (2^63 - 1). AdvancePast clamps to this
// value, so restored ids can push the counter to at most 2^63 -- it can never
// wrap to kInvalidObjectId and still leaves 2^63 organic allocations of
// headroom. Schema v1 rejects persisted ids above this cap.
constexpr ObjectId kMaxObjectId = 0x7FFF'FFFF'FFFF'FFFF;

class ObjectIdGenerator {
public:
    static ObjectId Next() {
        return Counter().fetch_add(1, std::memory_order_relaxed);
    }

    // Ensures every id generated after this call is strictly greater than
    // 'used' (clamped to kMaxObjectId, see above -- the counter never wraps).
    // Never moves the counter backwards: advancing past a value the counter
    // has already passed is a no-op.
    static void AdvancePast(ObjectId used) {
        const ObjectId target = (used < kMaxObjectId ? used : kMaxObjectId) + 1;
        auto& counter = Counter();
        ObjectId current = counter.load(std::memory_order_relaxed);
        while (current < target &&
               !counter.compare_exchange_weak(current, target,
                                              std::memory_order_relaxed)) {
        }
    }

private:
    static std::atomic<ObjectId>& Counter() {
        static std::atomic<ObjectId> next{1};
        return next;
    }
};

// Shared helper for restore constructors: registers a persisted id with the
// generator (so freshly generated ids can never collide with restored ones)
// and passes it through for member initialization.
inline ObjectId RestoreObjectId(ObjectId id) {
    ObjectIdGenerator::AdvancePast(id);
    return id;
}

} // namespace paramcad
