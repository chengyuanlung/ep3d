#include "Core/Dependency/DependencyGraph.h"
#include "Core/Feature/Feature.h"
#include "Core/Parameter/Parameter.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {

using namespace paramcad;

ObjectId newId() { return ObjectIdGenerator::Next(); }

bool contains(const std::vector<ObjectId>& ids, ObjectId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::size_t indexOf(const std::vector<ObjectId>& ids, ObjectId id) {
    const auto it = std::find(ids.begin(), ids.end(), id);
    assert(it != ids.end());
    return static_cast<std::size_t>(it - ids.begin());
}

DependencyGraph::ComputeFn alwaysOk() {
    return [](ObjectId) { return true; };
}

// Concrete Feature used by the bridge test; recompute succeeds or fails on
// demand and exposes a hook to mark propagated failures.
class TestFeature final : public Feature {
public:
    TestFeature(std::string name, bool succeeds)
        : Feature(std::move(name)), succeeds_(succeeds) {}

    bool recompute() override {
        setState(succeeds_ ? ComputeState::Valid : ComputeState::Failed);
        return succeeds_;
    }

    void markFailed() { setState(ComputeState::Failed); }

private:
    bool succeeds_;
};

void testAddNode() {
    DependencyGraph graph;
    const ObjectId a = newId();
    assert(graph.addNode(a));
    assert(graph.hasNode(a));
    assert(graph.nodeCount() == 1);
    assert(graph.state(a) == ComputeState::Dirty);

    const GraphResult duplicate = graph.addNode(a);
    assert(!duplicate);
    assert(duplicate.error == GraphError::NodeAlreadyExists);

    const GraphResult invalid = graph.addNode(kInvalidObjectId);
    assert(!invalid);
    assert(invalid.error == GraphError::NodeNotFound);
    assert(!graph.hasNode(kInvalidObjectId));
    assert(graph.nodeCount() == 1);
}

void testAddDependencyErrors() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId unknown = newId();
    assert(graph.addNode(a));

    GraphResult result = graph.addDependency(a, unknown);
    assert(!result && result.error == GraphError::NodeNotFound);
    result = graph.addDependency(unknown, a);
    assert(!result && result.error == GraphError::NodeNotFound);

    result = graph.addDependency(a, a);
    assert(!result && result.error == GraphError::WouldCreateCycle);
    assert(graph.dependentsOf(a).empty());
    assert(graph.prerequisitesOf(a).empty());
}

void testCycleRejection() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    assert(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    assert(graph.addDependency(b, a)); // a -> b
    assert(graph.addDependency(c, b)); // b -> c

    const auto dependentsBefore = graph.dependentsOf(c);
    const auto prerequisitesBefore = graph.prerequisitesOf(a);
    const ComputeState stateA = graph.state(a);
    const ComputeState stateC = graph.state(c);

    const GraphResult cycle = graph.addDependency(a, c); // would close c -> a
    assert(!cycle && cycle.error == GraphError::WouldCreateCycle);

    // Graph unchanged: edge lists identical, states untouched.
    assert(graph.dependentsOf(c) == dependentsBefore);
    assert(graph.prerequisitesOf(a) == prerequisitesBefore);
    assert(graph.dependentsOf(a) == std::vector<ObjectId>{b});
    assert(graph.prerequisitesOf(c) == std::vector<ObjectId>{b});
    assert(graph.state(a) == stateA);
    assert(graph.state(c) == stateC);
}

void testDirtyPropagation() {
    DependencyGraph graph;
    const ObjectId p = newId();
    const ObjectId s = newId();
    const ObjectId f = newId();
    const ObjectId u = newId();
    assert(graph.addNode(p) && graph.addNode(s) && graph.addNode(f) && graph.addNode(u));
    assert(graph.addDependency(s, p)); // p -> s
    assert(graph.addDependency(f, s)); // s -> f

    graph.recompute(alwaysOk()); // bring every node to Valid
    assert(graph.state(u) == ComputeState::Valid);

    assert(graph.markDirty(p));
    assert(graph.state(p) == ComputeState::Dirty);
    assert(graph.state(s) == ComputeState::Dirty);
    assert(graph.state(f) == ComputeState::Dirty);
    assert(graph.state(u) == ComputeState::Valid);
}

void testDiamondRecompute() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    const ObjectId d = newId();
    assert(graph.addNode(a) && graph.addNode(b) && graph.addNode(c) && graph.addNode(d));
    assert(graph.addDependency(b, a));
    assert(graph.addDependency(c, a));
    assert(graph.addDependency(d, b));
    assert(graph.addDependency(d, c));

    graph.recompute(alwaysOk());
    assert(graph.markDirty(a));

    std::unordered_map<ObjectId, int> callCount;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        ++callCount[id];
        return true;
    });

    assert(report.recomputed.size() == 4);
    assert(report.failed.empty());
    assert(report.skippedSuppressed.empty());
    for (ObjectId id : {a, b, c, d})
        assert(callCount[id] == 1);
    const auto& order = report.recomputed;
    assert(indexOf(order, a) < indexOf(order, b));
    assert(indexOf(order, a) < indexOf(order, c));
    assert(indexOf(order, b) < indexOf(order, d));
    assert(indexOf(order, c) < indexOf(order, d));
    for (ObjectId id : {a, b, c, d})
        assert(graph.state(id) == ComputeState::Valid);
}

void testOnlyAffectedNodesRecompute() {
    DependencyGraph graph;
    const ObjectId a1 = newId();
    const ObjectId a2 = newId();
    const ObjectId b1 = newId();
    const ObjectId b2 = newId();
    assert(graph.addNode(a1) && graph.addNode(a2) && graph.addNode(b1) && graph.addNode(b2));
    assert(graph.addDependency(a2, a1)); // chain A: a1 -> a2
    assert(graph.addDependency(b2, b1)); // chain B: b1 -> b2

    graph.recompute(alwaysOk());
    assert(graph.markDirty(a1));

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });

    assert(invoked.size() == 2);
    assert(contains(invoked, a1) && contains(invoked, a2));
    assert(!contains(invoked, b1) && !contains(invoked, b2));
    assert(report.recomputed == invoked);
    assert(graph.state(b1) == ComputeState::Valid);
    assert(graph.state(b2) == ComputeState::Valid);
}

void testFailurePropagation() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    assert(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    assert(graph.addDependency(b, a));
    assert(graph.addDependency(c, b));

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return id != b; // fail at b
    });

    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Failed);
    assert(graph.state(c) == ComputeState::Failed);
    assert(!contains(invoked, c)); // c's callback never invoked
    assert(report.recomputed == std::vector<ObjectId>{a});
    assert(report.failed == (std::vector<ObjectId>{b, c}));
}

void testSuppressed() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId s = newId();
    const ObjectId c = newId();
    assert(graph.addNode(a) && graph.addNode(s) && graph.addNode(c));
    assert(graph.addDependency(s, a)); // a -> s
    assert(graph.addDependency(c, s)); // s -> c

    graph.recompute(alwaysOk());
    assert(graph.setSuppressed(s, true));
    assert(graph.state(s) == ComputeState::Suppressed);

    // Dirty propagation passes through the Suppressed middle node.
    assert(graph.markDirty(a));
    assert(graph.state(a) == ComputeState::Dirty);
    assert(graph.state(s) == ComputeState::Suppressed);
    assert(graph.state(c) == ComputeState::Dirty);

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    assert(!contains(invoked, s)); // suppressed node never invoked
    assert(graph.state(s) == ComputeState::Suppressed);
    assert(report.skippedSuppressed == std::vector<ObjectId>{s});
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(c) == ComputeState::Valid);

    assert(graph.setSuppressed(s, false));
    assert(graph.state(s) == ComputeState::Dirty); // unsuppress -> Dirty
}

void testRemoveNode() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId m = newId();
    const ObjectId c = newId();
    const ObjectId d = newId();
    assert(graph.addNode(a) && graph.addNode(m) && graph.addNode(c) && graph.addNode(d));
    assert(graph.addDependency(m, a)); // a -> m
    assert(graph.addDependency(c, m)); // m -> c
    assert(graph.addDependency(d, c)); // c -> d

    graph.recompute(alwaysOk());
    assert(graph.removeNode(m));
    assert(!graph.hasNode(m));
    assert(graph.nodeCount() == 3);

    // Edges cleaned in both directions on former neighbors.
    assert(graph.dependentsOf(a).empty());
    assert(graph.prerequisitesOf(c).empty());

    // Former direct dependents become Dirty transitively.
    assert(graph.state(c) == ComputeState::Dirty);
    assert(graph.state(d) == ComputeState::Dirty);
    assert(graph.state(a) == ComputeState::Valid);
}

void testRemoveDependency() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    assert(graph.addNode(a) && graph.addNode(b));
    assert(graph.addDependency(b, a));

    graph.recompute(alwaysOk());
    assert(graph.removeDependency(b, a));
    assert(graph.dependentsOf(a).empty());
    assert(graph.prerequisitesOf(b).empty());
    assert(graph.state(b) == ComputeState::Dirty); // actual removal dirties the former dependent
    graph.recompute(alwaysOk()); // re-validate b so the markDirty below is what's under test

    assert(graph.markDirty(a));
    assert(graph.state(a) == ComputeState::Dirty);
    assert(graph.state(b) == ComputeState::Valid); // former dependent no longer dirtied
}

void testDeterminism() {
    DependencyGraph graph;
    const ObjectId root = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    const ObjectId d = newId();
    const ObjectId e = newId();
    assert(graph.addNode(root) && graph.addNode(b) && graph.addNode(c));
    assert(graph.addNode(d) && graph.addNode(e));
    assert(graph.addDependency(b, root));
    assert(graph.addDependency(c, root));
    assert(graph.addDependency(d, b));
    assert(graph.addDependency(e, c));

    const RecomputeReport first = graph.recompute(alwaysOk());
    assert(graph.markDirty(root));
    const RecomputeReport second = graph.recompute(alwaysOk());

    assert(first.recomputed.size() == 5);
    assert(first.recomputed == second.recomputed);
    assert(first.failed == second.failed);
    assert(first.skippedSuppressed == second.skippedSuppressed);
}

void testFeatureParameterBridge() {
    // Bridge pattern: graph nodes use ids of real Core objects; the callback
    // drives their state. No Core class owns the graph.
    Parameter width("Width", 25.0, UnitType::Millimeter);
    TestFeature base("Base", /*succeeds=*/false);
    TestFeature shell("Shell", /*succeeds=*/true);

    DependencyGraph graph;
    assert(graph.addNode(width.id()));
    assert(graph.addNode(base.id()));
    assert(graph.addNode(shell.id()));
    assert(graph.addDependency(base.id(), width.id()));  // width -> base
    assert(graph.addDependency(shell.id(), base.id()));  // base -> shell

    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        if (id == width.id()) return true;
        if (id == base.id()) return base.recompute();
        if (id == shell.id()) return shell.recompute();
        return false;
    });

    assert(base.state() == ComputeState::Failed);
    assert(shell.state() == ComputeState::Dirty); // never invoked; failure propagated in graph
    assert(graph.state(shell.id()) == ComputeState::Failed);
    assert(contains(report.failed, base.id()));
    assert(contains(report.failed, shell.id()));

    // Mirror propagated failures back onto the Feature objects.
    for (ObjectId id : report.failed) {
        if (id == shell.id()) shell.markFailed();
    }
    assert(shell.state() == ComputeState::Failed);
}

// ---------------------------------------------------------------------------
// Additional edge-case tests (added by TEST agent verification pass).
// ---------------------------------------------------------------------------

void testGraphResultOperatorBool() {
    const GraphResult ok{};
    assert(static_cast<bool>(ok));
    assert(ok.error == GraphError::None);

    const GraphResult notFound{GraphError::NodeNotFound};
    assert(!static_cast<bool>(notFound));
    const GraphResult exists{GraphError::NodeAlreadyExists};
    assert(!static_cast<bool>(exists));
    const GraphResult cycle{GraphError::WouldCreateCycle};
    assert(!static_cast<bool>(cycle));
}

void testRecomputeEmptyGraph() {
    DependencyGraph graph;
    bool invoked = false;
    const RecomputeReport report = graph.recompute([&](ObjectId) {
        invoked = true;
        return true;
    });
    assert(!invoked);
    assert(report.recomputed.empty());
    assert(report.failed.empty());
    assert(report.skippedSuppressed.empty());
}

void testRecomputeAllValidNoOp() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    assert(graph.addNode(a) && graph.addNode(b));
    assert(graph.addDependency(b, a));
    graph.recompute(alwaysOk()); // everything Valid

    bool invoked = false;
    const RecomputeReport report = graph.recompute([&](ObjectId) {
        invoked = true;
        return true;
    });
    assert(!invoked); // no Dirty nodes -> callback never runs
    assert(report.recomputed.empty());
    assert(report.failed.empty());
    assert(report.skippedSuppressed.empty());
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Valid);
}

void testUnknownIdErrors() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId unknown = newId();
    assert(graph.addNode(a));
    graph.recompute(alwaysOk());
    assert(graph.state(a) == ComputeState::Valid);

    // markDirty on unknown id: NodeNotFound, existing states untouched.
    GraphResult result = graph.markDirty(unknown);
    assert(!result && result.error == GraphError::NodeNotFound);
    assert(graph.state(a) == ComputeState::Valid);

    // removeNode on unknown id: NodeNotFound, node count unchanged.
    result = graph.removeNode(unknown);
    assert(!result && result.error == GraphError::NodeNotFound);
    assert(graph.nodeCount() == 1);

    // removeDependency with unknown ids on either side: NodeNotFound.
    result = graph.removeDependency(a, unknown);
    assert(!result && result.error == GraphError::NodeNotFound);
    result = graph.removeDependency(unknown, a);
    assert(!result && result.error == GraphError::NodeNotFound);

    // setState / setSuppressed on unknown id: NodeNotFound.
    result = graph.setState(unknown, ComputeState::Valid);
    assert(!result && result.error == GraphError::NodeNotFound);
    result = graph.setSuppressed(unknown, true);
    assert(!result && result.error == GraphError::NodeNotFound);
}

void testRemoveDependencyNonExistentEdgeIsNoOp() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    assert(graph.addNode(a) && graph.addNode(b));
    // No edge exists; removal between known nodes is a no-op success.
    assert(graph.removeDependency(b, a));
    assert(graph.dependentsOf(a).empty());
    assert(graph.prerequisitesOf(b).empty());
}

void testDuplicateEdgeIsNoOp() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    assert(graph.addNode(a) && graph.addNode(b));
    assert(graph.addDependency(b, a));
    assert(graph.addDependency(b, a)); // duplicate: no-op success
    assert(graph.dependentsOf(a).size() == 1);
    assert(graph.prerequisitesOf(b).size() == 1);
}

void testLongCycleRejection() {
    // 4-node chain a -> b -> c -> d; closing the long cycle must be rejected.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    const ObjectId d = newId();
    assert(graph.addNode(a) && graph.addNode(b) && graph.addNode(c) && graph.addNode(d));
    assert(graph.addDependency(b, a));
    assert(graph.addDependency(c, b));
    assert(graph.addDependency(d, c));

    const GraphResult cycle = graph.addDependency(a, d); // a consumes d -> 4-cycle
    assert(!cycle && cycle.error == GraphError::WouldCreateCycle);
    // Graph unchanged.
    assert(graph.prerequisitesOf(a).empty());
    assert(graph.dependentsOf(d).empty());

    // Closing a shorter back-edge inside the chain is also rejected.
    const GraphResult midCycle = graph.addDependency(b, d);
    assert(!midCycle && midCycle.error == GraphError::WouldCreateCycle);
}

void testDiamondPartialFailure() {
    // a -> (b, c) -> d; b fails, c succeeds. d must be Failed, c stays Valid.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    const ObjectId d = newId();
    assert(graph.addNode(a) && graph.addNode(b) && graph.addNode(c) && graph.addNode(d));
    assert(graph.addDependency(b, a));
    assert(graph.addDependency(c, a));
    assert(graph.addDependency(d, b));
    assert(graph.addDependency(d, c));

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return id != b; // only b fails
    });

    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Failed);
    assert(graph.state(c) == ComputeState::Valid);
    assert(graph.state(d) == ComputeState::Failed);
    assert(!contains(invoked, d)); // d never invoked once b failed
    assert(contains(report.recomputed, a) && contains(report.recomputed, c));
    assert(contains(report.failed, b) && contains(report.failed, d));
    assert(report.failed.size() == 2);
}

void testMarkDirtyOnAlreadyDirtyPropagates() {
    // The start node's current state must not gate propagation: markDirty on a
    // node that is already Dirty still dirties its Valid dependents.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    assert(graph.addNode(a) && graph.addNode(b));
    assert(graph.addDependency(b, a));

    graph.recompute(alwaysOk()); // both Valid
    assert(graph.setState(a, ComputeState::Dirty)); // direct set, no propagation
    assert(graph.state(b) == ComputeState::Valid);

    assert(graph.markDirty(a)); // a already Dirty
    assert(graph.state(a) == ComputeState::Dirty);
    assert(graph.state(b) == ComputeState::Dirty); // propagation still happened
}

void testRecoveryAfterFailure() {
    // A failed pass leaves b and c Failed. Fixing the callback and issuing
    // markDirty(b) + recompute must restore every node to Valid: markDirty
    // must convert Failed dependents to Dirty so recompute re-runs them.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    assert(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    assert(graph.addDependency(b, a));
    assert(graph.addDependency(c, b));

    graph.recompute([&](ObjectId id) { return id != b; }); // b fails
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Failed);
    assert(graph.state(c) == ComputeState::Failed);

    // "Fix" the model, mark the failed node dirty, recompute.
    assert(graph.markDirty(b));
    assert(graph.state(b) == ComputeState::Dirty);
    assert(graph.state(c) == ComputeState::Dirty); // Failed dependent re-dirtied

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    assert(contains(invoked, b) && contains(invoked, c));
    assert(!contains(invoked, a)); // untouched upstream not re-run
    assert(report.failed.empty());
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Valid);
    assert(graph.state(c) == ComputeState::Valid);
}

void testMarkDirtyOnSuppressedNode() {
    // markDirty on a Suppressed node leaves it Suppressed but still dirties
    // its dependents.
    DependencyGraph graph;
    const ObjectId s = newId();
    const ObjectId c = newId();
    assert(graph.addNode(s) && graph.addNode(c));
    assert(graph.addDependency(c, s));
    graph.recompute(alwaysOk());
    assert(graph.setSuppressed(s, true));
    assert(graph.setState(c, ComputeState::Valid));

    assert(graph.markDirty(s));
    assert(graph.state(s) == ComputeState::Suppressed); // unchanged
    assert(graph.state(c) == ComputeState::Dirty);      // dependent still dirtied
}

void testFailurePropagatesThroughSuppressed() {
    // a -> s(Suppressed) -> c: a fails; s stays Suppressed (and is reported in
    // skippedSuppressed), c becomes Failed.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId s = newId();
    const ObjectId c = newId();
    assert(graph.addNode(a) && graph.addNode(s) && graph.addNode(c));
    assert(graph.addDependency(s, a));
    assert(graph.addDependency(c, s));
    assert(graph.setSuppressed(s, true));

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return false; // a fails (only a is invokable)
    });

    assert(invoked == std::vector<ObjectId>{a});
    assert(graph.state(a) == ComputeState::Failed);
    assert(graph.state(s) == ComputeState::Suppressed);
    assert(graph.state(c) == ComputeState::Failed);
    assert(report.skippedSuppressed == std::vector<ObjectId>{s});
    assert(report.failed == (std::vector<ObjectId>{a, c}));
}

void testCallbackExceptionTreatedAsFailure() {
    // A throwing callback must not escape recompute; the node is Failed and
    // downstream propagation behaves as for a normal failure.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    assert(graph.addNode(a) && graph.addNode(b));
    assert(graph.addDependency(b, a));

    const RecomputeReport report = graph.recompute([&](ObjectId id) -> bool {
        if (id == a) throw std::runtime_error("boom");
        return true;
    });
    assert(graph.state(a) == ComputeState::Failed);
    assert(graph.state(b) == ComputeState::Failed);
    assert(report.failed == (std::vector<ObjectId>{a, b}));
    assert(report.recomputed.empty());
}

// ---------------------------------------------------------------------------
// Review-fix regression tests (stale-failure gating, edge-change dirtying).
// ---------------------------------------------------------------------------

void testStaleFailureGatesRecompute() {
    // a -> c and b -> c. Pass 1 fails b (so b and c are Failed). markDirty(a)
    // re-dirties c. Pass 2 must NOT invoke c: its prerequisite b is still
    // Failed from the previous pass, so c is set Failed and reported again.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    assert(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    assert(graph.addDependency(c, a)); // a -> c
    assert(graph.addDependency(c, b)); // b -> c

    graph.recompute([&](ObjectId id) { return id != b; }); // pass 1: b fails
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Failed);
    assert(graph.state(c) == ComputeState::Failed);

    assert(graph.markDirty(a)); // re-dirties a and c; b stays Failed

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    assert(!contains(invoked, c)); // c gated by stale-Failed prerequisite b
    assert(invoked == std::vector<ObjectId>{a});
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Failed);
    assert(graph.state(c) == ComputeState::Failed);
    assert(report.failed == std::vector<ObjectId>{c});
}

void testStaleFailureGatesThroughSuppressed() {
    // b(Failed from a prior pass) -> s(Suppressed) -> c(Dirty): recompute must
    // mark c Failed without invoking it; s untouched (stays Suppressed, not
    // invoked, not re-reported since nothing upstream ran this pass).
    DependencyGraph graph;
    const ObjectId b = newId();
    const ObjectId s = newId();
    const ObjectId c = newId();
    assert(graph.addNode(b) && graph.addNode(s) && graph.addNode(c));
    assert(graph.addDependency(s, b)); // b -> s
    assert(graph.addDependency(c, s)); // s -> c
    assert(graph.setSuppressed(s, true));

    graph.recompute([&](ObjectId) { return false; }); // pass 1: b fails
    assert(graph.state(b) == ComputeState::Failed);
    assert(graph.state(s) == ComputeState::Suppressed);
    assert(graph.state(c) == ComputeState::Failed);

    assert(graph.setState(c, ComputeState::Dirty)); // c dirty again; b failure is stale

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    assert(invoked.empty()); // neither b (Failed) nor s (Suppressed) nor c (gated)
    assert(graph.state(b) == ComputeState::Failed);
    assert(graph.state(s) == ComputeState::Suppressed);
    assert(graph.state(c) == ComputeState::Failed);
    assert(report.failed == std::vector<ObjectId>{c});
    assert(report.recomputed.empty());
    assert(report.skippedSuppressed.empty()); // s untouched: nothing upstream ran
}

void testEdgeChangeDirtiesDependent() {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    assert(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    assert(graph.addDependency(c, b)); // b -> c
    graph.recompute(alwaysOk());       // all Valid

    // Actual edge insert dirties the dependent and its transitive dependents.
    assert(graph.addDependency(b, a)); // a -> b
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Dirty);
    assert(graph.state(c) == ComputeState::Dirty);
    graph.recompute(alwaysOk()); // all Valid again

    // Duplicate edge is a no-op: dirties nothing.
    assert(graph.addDependency(b, a));
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Valid);
    assert(graph.state(c) == ComputeState::Valid);

    // Actual edge removal dirties the former dependent and its dependents.
    assert(graph.removeDependency(b, a));
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Dirty);
    assert(graph.state(c) == ComputeState::Dirty);
    graph.recompute(alwaysOk()); // all Valid again

    // Absent-edge removal is a no-op: dirties nothing.
    assert(graph.removeDependency(b, a));
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Valid);
    assert(graph.state(c) == ComputeState::Valid);
}

void testIdleRecomputeDoesNotRereportPersistedFailure() {
    // a -> b -> c; b fails once. A subsequent recompute with nothing Dirty
    // must invoke nothing, change no states, and re-report no failures
    // (persisted Failed nodes are gates, not fresh failures).
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    assert(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    assert(graph.addDependency(b, a));
    assert(graph.addDependency(c, b));

    graph.recompute([&](ObjectId id) { return id != b; }); // b fails
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Failed);
    assert(graph.state(c) == ComputeState::Failed);

    bool invoked = false;
    const RecomputeReport idle = graph.recompute([&](ObjectId) {
        invoked = true;
        return true;
    });
    assert(!invoked);
    assert(idle.recomputed.empty());
    assert(idle.failed.empty()); // long-standing failures not re-reported
    assert(idle.skippedSuppressed.empty());
    assert(graph.state(a) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Failed);
    assert(graph.state(c) == ComputeState::Failed);
}

void testEdgeRewireAcrossFailedPrerequisite() {
    // Interaction of the two review fixes: wiring a Valid node onto a Failed
    // prerequisite dirties it (edge-change fix), then recompute gates it
    // Failed without invoking it (stale-failure fix); unwiring dirties it
    // again and it recovers to Valid.
    DependencyGraph graph;
    const ObjectId b = newId();
    const ObjectId c = newId();
    assert(graph.addNode(b) && graph.addNode(c));
    graph.recompute([&](ObjectId id) { return id != b; }); // b Failed, c Valid
    assert(graph.state(b) == ComputeState::Failed);
    assert(graph.state(c) == ComputeState::Valid);

    // Attach c to the Failed b: c is dirtied by the edge change.
    assert(graph.addDependency(c, b));
    assert(graph.state(c) == ComputeState::Dirty);

    std::vector<ObjectId> invoked;
    RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    assert(invoked.empty()); // c gated by Failed prerequisite, never invoked
    assert(graph.state(c) == ComputeState::Failed);
    assert(report.failed == std::vector<ObjectId>{c});

    // Detach c from b: c is dirtied again and recomputes to Valid even though
    // b remains Failed elsewhere in the graph.
    assert(graph.removeDependency(c, b));
    assert(graph.state(c) == ComputeState::Dirty);

    invoked.clear();
    report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    assert(invoked == std::vector<ObjectId>{c});
    assert(graph.state(c) == ComputeState::Valid);
    assert(graph.state(b) == ComputeState::Failed); // untouched
    assert(report.failed.empty());
}

} // namespace

int main() {
    testAddNode();
    testAddDependencyErrors();
    testCycleRejection();
    testDirtyPropagation();
    testDiamondRecompute();
    testOnlyAffectedNodesRecompute();
    testFailurePropagation();
    testSuppressed();
    testRemoveNode();
    testRemoveDependency();
    testDeterminism();
    testFeatureParameterBridge();

    // Edge-case tests added by TEST agent verification pass.
    testGraphResultOperatorBool();
    testRecomputeEmptyGraph();
    testRecomputeAllValidNoOp();
    testUnknownIdErrors();
    testRemoveDependencyNonExistentEdgeIsNoOp();
    testDuplicateEdgeIsNoOp();
    testLongCycleRejection();
    testDiamondPartialFailure();
    testMarkDirtyOnAlreadyDirtyPropagates();
    testRecoveryAfterFailure();
    testMarkDirtyOnSuppressedNode();
    testFailurePropagatesThroughSuppressed();
    testCallbackExceptionTreatedAsFailure();

    // Review-fix regression tests.
    testStaleFailureGatesRecompute();
    testStaleFailureGatesThroughSuppressed();
    testEdgeChangeDirtiesDependent();

    // TEST agent re-verification of review fixes.
    testIdleRecomputeDoesNotRereportPersistedFailure();
    testEdgeRewireAcrossFailedPrerequisite();

    std::cout << "Dependency graph tests passed.\n";
    return 0;
}
