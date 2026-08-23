#pragma once

#include "Core/Document/ObjectId.h"

#include <vector>
#include "Core/Feature/ComputeState.h"
#include "Core/Kernel/KernelShape.h"

namespace paramcad {

// What a downstream consumer needs from any feature that produces a solid.
//
// Introduced in M4 for the reason ADR-M3-007 recorded: features are
// heterogeneous, and M3 shipped two defects that came from assuming every
// Feature behaves like a BoxFeature. MassPropertiesNode previously named
// BoxFeature directly, so adding PadFeature would have meant a second
// dynamic_cast and a third place that has to be updated for every future
// solid-producing feature type. Consumers now depend on this capability rather
// than on a concrete type.
//
// Deliberately narrow: it exposes only the runtime shape and its currency.
// Identity, naming and persistence stay on Feature -- this interface says what
// a feature CAN DO, not what it IS.
class ISolidFeature {
public:
    virtual ~ISolidFeature() = default;

    // Last successfully built shape. Retained byte-for-byte across a failed
    // rebuild (ADR-M3-001/004), so it may be stale -- always pair it with
    // currentState() rather than trusting it on its own.
    virtual const KernelShape& currentShape() const noexcept = 0;

    // The feature's cached ComputeState. The graph remains authoritative for
    // scheduling (ADR-M3-004/007); this is the same synchronized cache
    // Feature::state() exposes, reachable without naming a concrete type.
    virtual ComputeState currentState() const noexcept = 0;

    // EVERY upstream solid this feature consumes, in the order it uses them --
    // empty for a feature that builds from nothing (Box, Pad).
    //
    // M8's chain declaration (ADR-M8-001/003): consumers like Pocket override
    // this, and anything that must follow the chain tail -- the viewer, above
    // all -- asks this capability instead of enumerating concrete feature
    // types, which is ADR-M3-007's rule applied to the chain.
    //
    // PLURAL SINCE M21, because a boolean eats two. It used to be one id, and
    // a two-operand feature could only declare one of them: the other would
    // stay a live chain tail, so the viewer would draw the leftover as well as
    // the result and the part would appear twice.
    //
    // Defaulted rather than pure, deliberately: building-from-nothing is the
    // common case, and forcing every such feature to write "nothing" would add
    // a line per type that says nothing.
    virtual std::vector<ObjectId> consumedSolidIds() const { return {}; }

    // The PRIMARY one -- what a chain walk follows when it needs a single
    // upstream to step to.
    //
    // NOT VIRTUAL, and that is the point. This used to be the overridable one,
    // and a feature that consumed two could answer only half the question
    // while looking complete. Defining it in terms of the plural means a new
    // multi-consumer cannot forget: there is only one thing to override.
    ObjectId consumedSolidId() const noexcept {
        const std::vector<ObjectId> ids = consumedSolidIds();
        return ids.empty() ? kInvalidObjectId : ids.front();
    }
};

} // namespace paramcad
