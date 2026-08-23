#pragma once

#include "Core/Document/ObjectId.h"

namespace paramcad {

// A feature built FROM a sketch (M17.10, ADR-M17-033).
//
// A capability, not a list of concrete types, for the reason ADR-M3-007 and
// ADR-M4-009 already state -- and this particular list has already been caught
// out of date once. `featuresReferencingSketch` enumerated Pad alone; a review
// found it after Pocket and Revolve had both shipped, which made that
// function's own contract ("empty means the sketch can be deleted") false for
// two of the three kinds. The fix was to add two more branches, which leaves
// the same trap set for the fourth kind.
//
// Asking the FEATURE which sketch it consumes cannot go out of date: a new
// sketch-consuming feature either implements this and is found everywhere, or
// does not and is found nowhere -- and "found nowhere" is a compile-time
// decision its author makes on purpose, not a branch somebody forgot to add to
// a switch on the other side of the codebase.
//
// Fillet and Chamfer deliberately do NOT implement it: they dress the edges of
// a solid and reference no sketch at all.
class ISketchConsuming {
public:
    virtual ~ISketchConsuming() = default;

    // The sketch this feature is built from. Never kInvalidObjectId for a
    // feature that implements this -- a pad with no sketch is not a pad.
    virtual ObjectId consumedSketchId() const noexcept = 0;
};

} // namespace paramcad
