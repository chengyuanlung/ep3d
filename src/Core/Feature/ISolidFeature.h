#pragma once

#include "Core/Document/ObjectId.h"
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

    // The upstream solid this feature CONSUMES, or kInvalidObjectId for a
    // feature that builds from nothing (Box, Pad). M8's chain declaration
    // (ADR-M8-001/003): consumers like Pocket override this, and anything that
    // must follow the chain tail -- the viewer, above all -- asks this
    // capability instead of enumerating concrete feature types, which is
    // ADR-M3-007's rule applied to the chain.
    //
    // Defaulted rather than pure, deliberately: building-from-nothing is the
    // common case, and forcing every such feature to write "invalid" would
    // add a line per type that says nothing.
    virtual ObjectId consumedSolidId() const noexcept { return kInvalidObjectId; }
};

} // namespace paramcad
