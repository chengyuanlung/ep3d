#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Feature/ComputeState.h"
#include <cstddef>
#include <functional>
#include <unordered_map>
#include <vector>

namespace paramcad {

enum class GraphError { None, NodeNotFound, NodeAlreadyExists, WouldCreateCycle };

struct GraphResult {
    GraphError error = GraphError::None;
    explicit operator bool() const noexcept { return error == GraphError::None; } // true == success
};

struct RecomputeReport {
    std::vector<ObjectId> recomputed;         // in execution order
    std::vector<ObjectId> failed;             // Failed this pass, incl. propagated
    std::vector<ObjectId> skippedSuppressed;  // Suppressed nodes that would have run
};

// Generic dependency graph over ObjectId nodes with per-node ComputeState,
// dirty propagation, cycle rejection, and topological recompute of affected
// nodes only. Standalone: knows nothing about Parameters/Features/Bodies.
// Single-threaded use assumed; no threading guarantees.
//
// EDGE DIRECTION: an edge points prerequisite -> dependent (data flows
// downstream along edges). addDependency(dependent, prerequisite) records
// "dependent consumes prerequisite".
class DependencyGraph {
public:
    using ComputeFn = std::function<bool(ObjectId)>; // returns success

    // Adds a node with initial state Dirty. Rejects kInvalidObjectId
    // (NodeNotFound) and duplicates (NodeAlreadyExists).
    GraphResult addNode(ObjectId id);

    // Removes the node and all incident edges in both directions. Former
    // direct dependents are markDirty'd (propagates transitively).
    GraphResult removeNode(ObjectId id);

    bool hasNode(ObjectId id) const noexcept;
    std::size_t nodeCount() const noexcept;

    // Rejects unknown ids (NodeNotFound), self-edges and any cycle
    // (WouldCreateCycle); the graph is unchanged on failure. A duplicate
    // edge is a no-op success and dirties nothing. An actual edge insert
    // marks the dependent Dirty (propagates transitively): its input set
    // changed, so any held result is stale.
    GraphResult addDependency(ObjectId dependent, ObjectId prerequisite);

    // Rejects unknown ids (NodeNotFound). Removing a non-existent edge is a
    // no-op success and dirties nothing. An actual removal marks the former
    // dependent Dirty (propagates transitively): its input set changed.
    GraphResult removeDependency(ObjectId dependent, ObjectId prerequisite);

    std::vector<ObjectId> dependentsOf(ObjectId id) const;     // direct downstream
    std::vector<ObjectId> prerequisitesOf(ObjectId id) const;  // direct upstream

    // Precondition: hasNode(id). Asserts in debug; returns ComputeState::Failed
    // for an unknown id in release.
    ComputeState state(ObjectId id) const;

    // Direct set, no propagation (test/integration hook).
    GraphResult setState(ObjectId id, ComputeState s);

    // Sets the node Dirty (unless it is Suppressed, in which case its state is
    // unchanged) and marks all transitive dependents Dirty. Propagation passes
    // THROUGH Suppressed nodes without changing their state.
    GraphResult markDirty(ObjectId id);

    // Suppress or unsuppress a node; unsuppress -> Dirty (mirrors
    // Feature::setSuppressed).
    GraphResult setSuppressed(ObjectId id, bool suppressed);

    // Recomputes affected nodes in topological order (all prerequisites before
    // dependents), each affected node invoked exactly once. Only Dirty nodes
    // are invoked; Valid/Suppressed nodes never are (Suppressed nodes that
    // would have run go to skippedSuppressed). fn returning false marks the
    // node Failed and sets ALL transitive dependents Failed without invoking
    // fn; Suppressed nodes stay Suppressed downstream of a failure but
    // propagation continues through them. A Failed node persists as a
    // downstream barrier across passes: any Dirty node with a Failed
    // prerequisite (from this pass or a previous one) is set Failed instead of
    // being invoked, until the failed node is re-marked Dirty and recomputes
    // successfully. Long-standing failures are not re-reported on idle passes;
    // only nodes whose state actually transitions this pass appear in failed.
    // Ties in the order are broken by node
    // insertion order, so results are deterministic. fn must be non-null:
    // asserts in debug; returns an empty RecomputeReport, graph unchanged, if
    // null in release.
    RecomputeReport recompute(const ComputeFn& fn);

private:
    struct Node {
        ComputeState state = ComputeState::Dirty;
        std::vector<ObjectId> prerequisites; // direct upstream
        std::vector<ObjectId> dependents;    // direct downstream
    };

    Node* findNode(ObjectId id) noexcept;
    const Node* findNode(ObjectId id) const noexcept;
    // True if a directed path along dependent edges leads from 'from' to 'to'.
    bool reaches(ObjectId from, ObjectId to) const;
    std::vector<ObjectId> topologicalOrder() const;

    std::unordered_map<ObjectId, Node> nodes_;
    std::vector<ObjectId> insertionOrder_;
};

} // namespace paramcad
