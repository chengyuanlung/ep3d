#pragma once

#include "Core/Document/ObjectId.h"

#include <vector>

namespace paramcad {

// What a feature wants from ONE sketch it reads (M26.8).
struct ConsumedSketch {
    ObjectId sketchId{kInvalidObjectId};

    // Whether the feature needs that sketch to CLOSE INTO A PROFILE.
    //
    // A pad, a pocket, a revolve and a loft section sweep an AREA, so a sketch
    // that does not close is a real failure for them. A hole reads POINTS and a
    // path reads a CURVE: for those, "not a closed loop" is the drawing being
    // exactly what it should be, and marking it Failed is the tree reporting a
    // fault in a part that built perfectly.
    bool needsClosedProfile{true};
};

// A feature built FROM one or more sketches (M17.10, ADR-M17-033).
//
// A capability, not a list of concrete types, for the reason ADR-M3-007 and
// ADR-M4-009 already state -- and this particular list has already been caught
// out of date once. `featuresReferencingSketch` enumerated Pad alone; a review
// found it after Pocket and Revolve had both shipped, which made that
// function's own contract ("empty means the sketch can be deleted") false for
// two of the three kinds. The fix was to add two more branches, which leaves
// the same trap set for the fourth kind.
//
// Asking the FEATURE which sketches it consumes cannot go out of date: a new
// sketch-consuming feature either implements this and is found everywhere, or
// does not and is found nowhere -- and "found nowhere" is a compile-time
// decision its author makes on purpose, not a branch somebody forgot to add to
// a switch on the other side of the codebase.
//
// A LIST, NOT ONE ID (M26.8). The single-id form was the same trap one level
// down: a Loft reads several sections and named only its first, a Sweep reads a
// profile AND a path and named only the profile, and a curve pattern reads a
// path and did not implement this at all. So the deletion gate protected the
// first section of a loft and let the other two be deleted out from under it,
// and the tree marked a hole's point sketch "Failed" for not being a closed
// loop it was never meant to be.
//
// Fillet and Chamfer deliberately do NOT implement it: they dress the edges of
// a solid and reference no sketch at all.
class ISketchConsuming {
public:
    virtual ~ISketchConsuming() = default;

    // EVERY sketch this feature reads, and what it wants from each. Never
    // empty for a feature that implements this -- a pad with no sketch is not
    // a pad.
    //
    // ORDERED, with the PRIMARY one first: `consumedSketchId()` below is the
    // front of this list, so a feature that has a natural "the" sketch puts it
    // there.
    virtual std::vector<ConsumedSketch> consumedSketches() const = 0;

    // The PRIMARY sketch -- the one whose lineage a tree draws this feature
    // over.
    //
    // NOT VIRTUAL, and DERIVED from the list above rather than stored beside
    // it. Two members answering "which sketch" is exactly the pair that drifts,
    // and this file exists because a version of that already did.
    ObjectId consumedSketchId() const {
        const std::vector<ConsumedSketch> all = consumedSketches();
        return all.empty() ? kInvalidObjectId : all.front().sketchId;
    }

    // Whether this feature needs `sketchId` to close into a profile. False for
    // a sketch it does not read at all.
    bool needsClosedProfileOf(ObjectId sketchId) const {
        for (const ConsumedSketch& one : consumedSketches())
            if (one.sketchId == sketchId && one.needsClosedProfile) return true;
        return false;
    }

    bool reads(ObjectId sketchId) const {
        for (const ConsumedSketch& one : consumedSketches())
            if (one.sketchId == sketchId) return true;
        return false;
    }
};

} // namespace paramcad
