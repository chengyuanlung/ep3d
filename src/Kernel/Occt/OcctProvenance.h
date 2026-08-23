#pragma once

#include "Core/Kernel/KernelShape.h"

#include <cstdint>

namespace paramcad {

// Recording who made which face (M17.13, ADR-M17-035).
//
// Called by a feature right after it builds, with the shape it consumed and
// the shape it produced. Everything in the result that was NOT in the base is
// this feature's work; everything the base already knew about is carried
// forward, so a chain of features accumulates a full account rather than only
// the last step's.
//
// This lives OUTSIDE IGeometryKernel on purpose. Threading a tag through
// extrude, revolve, subtract, fillet and chamfer would put a document concept
// into five kernel signatures and into every fake kernel that implements them
// -- for a fact none of those operations needs in order to build geometry.
// Comparing the result against the base afterwards gets the same answer from
// outside, because OCCT's booleans share their TShapes with their inputs: a
// face the operation left alone IS the base's face, not a copy of it.
//
// `base` may be an invalid KernelShape, which is what a Pad or a Revolve has:
// nothing came before, so every face in the result is theirs.
//
// Returns a KernelShape wrapping the SAME topology with the provenance
// attached. The input is not modified -- shapes are values here, and a
// function that edited its argument's history would make the order features
// are recomputed in observable.
KernelShape WithProvenance(const KernelShape& result, const KernelShape& base,
                           std::uint64_t tag);

} // namespace paramcad
