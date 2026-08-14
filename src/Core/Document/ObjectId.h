#pragma once

#include <atomic>
#include <cstdint>

namespace paramcad {

using ObjectId = std::uint64_t;
constexpr ObjectId kInvalidObjectId = 0;
// Largest id a restore path may inject, and the largest the SERIALIZER will
// accept (2^63 - 1). AdvancePast clamps to this value, so restored ids can push
// the counter to at most 2^63 and it can never wrap to kInvalidObjectId.
//
// An earlier version of this comment claimed the clamp "still leaves 2^63
// organic allocations of headroom". That was false in the way that matters: the
// counter can go on issuing ids past the cap, but every one of them is ABOVE
// what the loader accepts, so the SAVABLE headroom is zero. A process that
// restores an id of 2^63-1 thereafter produces documents that save cleanly and
// can never be opened again.
//
// `validateSaveable` now refuses such a save outright, which converts silent
// data loss into a reported failure. It does not create headroom -- there is
// none to create -- and a process in that state must be restarted.
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
