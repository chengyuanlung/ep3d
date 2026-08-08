#include "Core/Dependency/DependencyGraph.h"
#include "Core/Feature/Feature.h"
#include "Core/Parameter/Parameter.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <stdexcept>
#include <string_view>
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
    EXPECT_NE(it, ids.end()) << "id " << id << " not found in vector";
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

    std::string_view typeName() const noexcept override { return "Test"; }

    bool recompute() override {
        setState(succeeds_ ? ComputeState::Valid : ComputeState::Failed);
        return succeeds_;
    }

    void markFailed() { setState(ComputeState::Failed); }

private:
    bool succeeds_;
};

TEST(DependencyGraphTests, AddNode) {
    DependencyGraph graph;
    const ObjectId a = newId();
    EXPECT_TRUE(graph.addNode(a));
    EXPECT_TRUE(graph.hasNode(a));
    EXPECT_EQ(graph.nodeCount(), 1u);
    EXPECT_EQ(graph.state(a), ComputeState::Dirty);

    const GraphResult duplicate = graph.addNode(a);
    EXPECT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error, GraphError::NodeAlreadyExists);

    const GraphResult invalid = graph.addNode(kInvalidObjectId);
    EXPECT_FALSE(invalid);
    EXPECT_EQ(invalid.error, GraphError::NodeNotFound);
    EXPECT_FALSE(graph.hasNode(kInvalidObjectId));
    EXPECT_EQ(graph.nodeCount(), 1u);
}

TEST(DependencyGraphTests, AddDependencyErrors) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId unknown = newId();
    EXPECT_TRUE(graph.addNode(a));

    GraphResult result = graph.addDependency(a, unknown);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, GraphError::NodeNotFound);
    result = graph.addDependency(unknown, a);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, GraphError::NodeNotFound);

    result = graph.addDependency(a, a);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, GraphError::WouldCreateCycle);
    EXPECT_TRUE(graph.dependentsOf(a).empty());
    EXPECT_TRUE(graph.prerequisitesOf(a).empty());
}

TEST(DependencyGraphTests, CycleRejection) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    EXPECT_TRUE(graph.addDependency(b, a)); // a -> b
    EXPECT_TRUE(graph.addDependency(c, b)); // b -> c

    const auto dependentsBefore = graph.dependentsOf(c);
    const auto prerequisitesBefore = graph.prerequisitesOf(a);
    const ComputeState stateA = graph.state(a);
    const ComputeState stateC = graph.state(c);

    const GraphResult cycle = graph.addDependency(a, c); // would close c -> a
    EXPECT_FALSE(cycle);
    EXPECT_EQ(cycle.error, GraphError::WouldCreateCycle);

    // Graph unchanged: edge lists identical, states untouched.
    EXPECT_EQ(graph.dependentsOf(c), dependentsBefore);
    EXPECT_EQ(graph.prerequisitesOf(a), prerequisitesBefore);
    EXPECT_EQ(graph.dependentsOf(a), std::vector<ObjectId>{b});
    EXPECT_EQ(graph.prerequisitesOf(c), std::vector<ObjectId>{b});
    EXPECT_EQ(graph.state(a), stateA);
    EXPECT_EQ(graph.state(c), stateC);
}

TEST(DependencyGraphTests, DirtyPropagation) {
    DependencyGraph graph;
    const ObjectId p = newId();
    const ObjectId s = newId();
    const ObjectId f = newId();
    const ObjectId u = newId();
    EXPECT_TRUE(graph.addNode(p) && graph.addNode(s) && graph.addNode(f) && graph.addNode(u));
    EXPECT_TRUE(graph.addDependency(s, p)); // p -> s
    EXPECT_TRUE(graph.addDependency(f, s)); // s -> f

    graph.recompute(alwaysOk()); // bring every node to Valid
    EXPECT_EQ(graph.state(u), ComputeState::Valid);

    EXPECT_TRUE(graph.markDirty(p));
    EXPECT_EQ(graph.state(p), ComputeState::Dirty);
    EXPECT_EQ(graph.state(s), ComputeState::Dirty);
    EXPECT_EQ(graph.state(f), ComputeState::Dirty);
    EXPECT_EQ(graph.state(u), ComputeState::Valid);
}

TEST(DependencyGraphTests, DiamondRecompute) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    const ObjectId d = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b) && graph.addNode(c) && graph.addNode(d));
    EXPECT_TRUE(graph.addDependency(b, a));
    EXPECT_TRUE(graph.addDependency(c, a));
    EXPECT_TRUE(graph.addDependency(d, b));
    EXPECT_TRUE(graph.addDependency(d, c));

    graph.recompute(alwaysOk());
    EXPECT_TRUE(graph.markDirty(a));

    std::unordered_map<ObjectId, int> callCount;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        ++callCount[id];
        return true;
    });

    ASSERT_EQ(report.recomputed.size(), 4u);
    EXPECT_TRUE(report.failed.empty());
    EXPECT_TRUE(report.skippedSuppressed.empty());
    for (ObjectId id : {a, b, c, d})
        EXPECT_EQ(callCount[id], 1);
    const auto& order = report.recomputed;
    EXPECT_LT(indexOf(order, a), indexOf(order, b));
    EXPECT_LT(indexOf(order, a), indexOf(order, c));
    EXPECT_LT(indexOf(order, b), indexOf(order, d));
    EXPECT_LT(indexOf(order, c), indexOf(order, d));
    for (ObjectId id : {a, b, c, d})
        EXPECT_EQ(graph.state(id), ComputeState::Valid);
}

TEST(DependencyGraphTests, OnlyAffectedNodesRecompute) {
    DependencyGraph graph;
    const ObjectId a1 = newId();
    const ObjectId a2 = newId();
    const ObjectId b1 = newId();
    const ObjectId b2 = newId();
    EXPECT_TRUE(graph.addNode(a1) && graph.addNode(a2) && graph.addNode(b1) && graph.addNode(b2));
    EXPECT_TRUE(graph.addDependency(a2, a1)); // chain A: a1 -> a2
    EXPECT_TRUE(graph.addDependency(b2, b1)); // chain B: b1 -> b2

    graph.recompute(alwaysOk());
    EXPECT_TRUE(graph.markDirty(a1));

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });

    EXPECT_EQ(invoked.size(), 2u);
    EXPECT_TRUE(contains(invoked, a1) && contains(invoked, a2));
    EXPECT_TRUE(!contains(invoked, b1) && !contains(invoked, b2));
    EXPECT_EQ(report.recomputed, invoked);
    EXPECT_EQ(graph.state(b1), ComputeState::Valid);
    EXPECT_EQ(graph.state(b2), ComputeState::Valid);
}

TEST(DependencyGraphTests, FailurePropagation) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    EXPECT_TRUE(graph.addDependency(b, a));
    EXPECT_TRUE(graph.addDependency(c, b));

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return id != b; // fail at b
    });

    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(graph.state(c), ComputeState::Failed);
    EXPECT_FALSE(contains(invoked, c)); // c's callback never invoked
    EXPECT_EQ(report.recomputed, std::vector<ObjectId>{a});
    EXPECT_EQ(report.failed, (std::vector<ObjectId>{b, c}));
}

TEST(DependencyGraphTests, Suppressed) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId s = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(s) && graph.addNode(c));
    EXPECT_TRUE(graph.addDependency(s, a)); // a -> s
    EXPECT_TRUE(graph.addDependency(c, s)); // s -> c

    graph.recompute(alwaysOk());
    EXPECT_TRUE(graph.setSuppressed(s, true));
    EXPECT_EQ(graph.state(s), ComputeState::Suppressed);

    // Dirty propagation passes through the Suppressed middle node.
    EXPECT_TRUE(graph.markDirty(a));
    EXPECT_EQ(graph.state(a), ComputeState::Dirty);
    EXPECT_EQ(graph.state(s), ComputeState::Suppressed);
    EXPECT_EQ(graph.state(c), ComputeState::Dirty);

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    EXPECT_FALSE(contains(invoked, s)); // suppressed node never invoked
    EXPECT_EQ(graph.state(s), ComputeState::Suppressed);
    EXPECT_EQ(report.skippedSuppressed, std::vector<ObjectId>{s});
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(c), ComputeState::Valid);

    EXPECT_TRUE(graph.setSuppressed(s, false));
    EXPECT_EQ(graph.state(s), ComputeState::Dirty); // unsuppress -> Dirty
}

TEST(DependencyGraphTests, RemoveNode) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId m = newId();
    const ObjectId c = newId();
    const ObjectId d = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(m) && graph.addNode(c) && graph.addNode(d));
    EXPECT_TRUE(graph.addDependency(m, a)); // a -> m
    EXPECT_TRUE(graph.addDependency(c, m)); // m -> c
    EXPECT_TRUE(graph.addDependency(d, c)); // c -> d

    graph.recompute(alwaysOk());
    EXPECT_TRUE(graph.removeNode(m));
    EXPECT_FALSE(graph.hasNode(m));
    EXPECT_EQ(graph.nodeCount(), 3u);

    // Edges cleaned in both directions on former neighbors.
    EXPECT_TRUE(graph.dependentsOf(a).empty());
    EXPECT_TRUE(graph.prerequisitesOf(c).empty());

    // Former direct dependents become Dirty transitively.
    EXPECT_EQ(graph.state(c), ComputeState::Dirty);
    EXPECT_EQ(graph.state(d), ComputeState::Dirty);
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
}

TEST(DependencyGraphTests, RemoveDependency) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b));
    EXPECT_TRUE(graph.addDependency(b, a));

    graph.recompute(alwaysOk());
    EXPECT_TRUE(graph.removeDependency(b, a));
    EXPECT_TRUE(graph.dependentsOf(a).empty());
    EXPECT_TRUE(graph.prerequisitesOf(b).empty());
    EXPECT_EQ(graph.state(b), ComputeState::Dirty); // actual removal dirties the former dependent
    graph.recompute(alwaysOk()); // re-validate b so the markDirty below is what's under test

    EXPECT_TRUE(graph.markDirty(a));
    EXPECT_EQ(graph.state(a), ComputeState::Dirty);
    EXPECT_EQ(graph.state(b), ComputeState::Valid); // former dependent no longer dirtied
}

TEST(DependencyGraphTests, Determinism) {
    DependencyGraph graph;
    const ObjectId root = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    const ObjectId d = newId();
    const ObjectId e = newId();
    EXPECT_TRUE(graph.addNode(root) && graph.addNode(b) && graph.addNode(c));
    EXPECT_TRUE(graph.addNode(d) && graph.addNode(e));
    EXPECT_TRUE(graph.addDependency(b, root));
    EXPECT_TRUE(graph.addDependency(c, root));
    EXPECT_TRUE(graph.addDependency(d, b));
    EXPECT_TRUE(graph.addDependency(e, c));

    const RecomputeReport first = graph.recompute(alwaysOk());
    EXPECT_TRUE(graph.markDirty(root));
    const RecomputeReport second = graph.recompute(alwaysOk());

    EXPECT_EQ(first.recomputed.size(), 5u);
    EXPECT_EQ(first.recomputed, second.recomputed);
    EXPECT_EQ(first.failed, second.failed);
    EXPECT_EQ(first.skippedSuppressed, second.skippedSuppressed);
}

TEST(DependencyGraphTests, FeatureParameterBridge) {
    // Bridge pattern: graph nodes use ids of real Core objects; the callback
    // drives their state. No Core class owns the graph.
    Parameter width("Width", 25.0, UnitType::Millimeter);
    TestFeature base("Base", /*succeeds=*/false);
    TestFeature shell("Shell", /*succeeds=*/true);

    DependencyGraph graph;
    EXPECT_TRUE(graph.addNode(width.id()));
    EXPECT_TRUE(graph.addNode(base.id()));
    EXPECT_TRUE(graph.addNode(shell.id()));
    EXPECT_TRUE(graph.addDependency(base.id(), width.id()));  // width -> base
    EXPECT_TRUE(graph.addDependency(shell.id(), base.id()));  // base -> shell

    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        if (id == width.id()) return true;
        if (id == base.id()) return base.recompute();
        if (id == shell.id()) return shell.recompute();
        return false;
    });

    EXPECT_EQ(base.state(), ComputeState::Failed);
    EXPECT_EQ(shell.state(), ComputeState::Dirty); // never invoked; failure propagated in graph
    EXPECT_EQ(graph.state(shell.id()), ComputeState::Failed);
    EXPECT_TRUE(contains(report.failed, base.id()));
    EXPECT_TRUE(contains(report.failed, shell.id()));

    // Mirror propagated failures back onto the Feature objects.
    for (ObjectId id : report.failed) {
        if (id == shell.id()) shell.markFailed();
    }
    EXPECT_EQ(shell.state(), ComputeState::Failed);
}

// ---------------------------------------------------------------------------
// Additional edge-case tests (added by TEST agent verification pass).
// ---------------------------------------------------------------------------

TEST(DependencyGraphTests, GraphResultOperatorBool) {
    const GraphResult ok{};
    EXPECT_TRUE(static_cast<bool>(ok));
    EXPECT_EQ(ok.error, GraphError::None);

    const GraphResult notFound{GraphError::NodeNotFound};
    EXPECT_FALSE(static_cast<bool>(notFound));
    const GraphResult exists{GraphError::NodeAlreadyExists};
    EXPECT_FALSE(static_cast<bool>(exists));
    const GraphResult cycle{GraphError::WouldCreateCycle};
    EXPECT_FALSE(static_cast<bool>(cycle));
}

TEST(DependencyGraphTests, RecomputeEmptyGraph) {
    DependencyGraph graph;
    bool invoked = false;
    const RecomputeReport report = graph.recompute([&](ObjectId) {
        invoked = true;
        return true;
    });
    EXPECT_FALSE(invoked);
    EXPECT_TRUE(report.recomputed.empty());
    EXPECT_TRUE(report.failed.empty());
    EXPECT_TRUE(report.skippedSuppressed.empty());
}

TEST(DependencyGraphTests, RecomputeAllValidNoOp) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b));
    EXPECT_TRUE(graph.addDependency(b, a));
    graph.recompute(alwaysOk()); // everything Valid

    bool invoked = false;
    const RecomputeReport report = graph.recompute([&](ObjectId) {
        invoked = true;
        return true;
    });
    EXPECT_FALSE(invoked); // no Dirty nodes -> callback never runs
    EXPECT_TRUE(report.recomputed.empty());
    EXPECT_TRUE(report.failed.empty());
    EXPECT_TRUE(report.skippedSuppressed.empty());
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Valid);
}

TEST(DependencyGraphTests, UnknownIdErrors) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId unknown = newId();
    EXPECT_TRUE(graph.addNode(a));
    graph.recompute(alwaysOk());
    EXPECT_EQ(graph.state(a), ComputeState::Valid);

    // markDirty on unknown id: NodeNotFound, existing states untouched.
    GraphResult result = graph.markDirty(unknown);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, GraphError::NodeNotFound);
    EXPECT_EQ(graph.state(a), ComputeState::Valid);

    // removeNode on unknown id: NodeNotFound, node count unchanged.
    result = graph.removeNode(unknown);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, GraphError::NodeNotFound);
    EXPECT_EQ(graph.nodeCount(), 1u);

    // removeDependency with unknown ids on either side: NodeNotFound.
    result = graph.removeDependency(a, unknown);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, GraphError::NodeNotFound);
    result = graph.removeDependency(unknown, a);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, GraphError::NodeNotFound);

    // setState / setSuppressed on unknown id: NodeNotFound.
    result = graph.setState(unknown, ComputeState::Valid);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, GraphError::NodeNotFound);
    result = graph.setSuppressed(unknown, true);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, GraphError::NodeNotFound);
}

TEST(DependencyGraphTests, RemoveDependencyNonExistentEdgeIsNoOp) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b));
    // No edge exists; removal between known nodes is a no-op success.
    EXPECT_TRUE(graph.removeDependency(b, a));
    EXPECT_TRUE(graph.dependentsOf(a).empty());
    EXPECT_TRUE(graph.prerequisitesOf(b).empty());
}

TEST(DependencyGraphTests, DuplicateEdgeIsNoOp) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b));
    EXPECT_TRUE(graph.addDependency(b, a));
    EXPECT_TRUE(graph.addDependency(b, a)); // duplicate: no-op success
    EXPECT_EQ(graph.dependentsOf(a).size(), 1u);
    EXPECT_EQ(graph.prerequisitesOf(b).size(), 1u);
}

TEST(DependencyGraphTests, LongCycleRejection) {
    // 4-node chain a -> b -> c -> d; closing the long cycle must be rejected.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    const ObjectId d = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b) && graph.addNode(c) && graph.addNode(d));
    EXPECT_TRUE(graph.addDependency(b, a));
    EXPECT_TRUE(graph.addDependency(c, b));
    EXPECT_TRUE(graph.addDependency(d, c));

    const GraphResult cycle = graph.addDependency(a, d); // a consumes d -> 4-cycle
    EXPECT_FALSE(cycle);
    EXPECT_EQ(cycle.error, GraphError::WouldCreateCycle);
    // Graph unchanged.
    EXPECT_TRUE(graph.prerequisitesOf(a).empty());
    EXPECT_TRUE(graph.dependentsOf(d).empty());

    // Closing a shorter back-edge inside the chain is also rejected.
    const GraphResult midCycle = graph.addDependency(b, d);
    EXPECT_FALSE(midCycle);
    EXPECT_EQ(midCycle.error, GraphError::WouldCreateCycle);
}

TEST(DependencyGraphTests, DiamondPartialFailure) {
    // a -> (b, c) -> d; b fails, c succeeds. d must be Failed, c stays Valid.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    const ObjectId d = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b) && graph.addNode(c) && graph.addNode(d));
    EXPECT_TRUE(graph.addDependency(b, a));
    EXPECT_TRUE(graph.addDependency(c, a));
    EXPECT_TRUE(graph.addDependency(d, b));
    EXPECT_TRUE(graph.addDependency(d, c));

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return id != b; // only b fails
    });

    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(graph.state(c), ComputeState::Valid);
    EXPECT_EQ(graph.state(d), ComputeState::Failed);
    EXPECT_FALSE(contains(invoked, d)); // d never invoked once b failed
    EXPECT_TRUE(contains(report.recomputed, a) && contains(report.recomputed, c));
    EXPECT_TRUE(contains(report.failed, b) && contains(report.failed, d));
    EXPECT_EQ(report.failed.size(), 2u);
}

TEST(DependencyGraphTests, MarkDirtyOnAlreadyDirtyPropagates) {
    // The start node's current state must not gate propagation: markDirty on a
    // node that is already Dirty still dirties its Valid dependents.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b));
    EXPECT_TRUE(graph.addDependency(b, a));

    graph.recompute(alwaysOk()); // both Valid
    EXPECT_TRUE(graph.setState(a, ComputeState::Dirty)); // direct set, no propagation
    EXPECT_EQ(graph.state(b), ComputeState::Valid);

    EXPECT_TRUE(graph.markDirty(a)); // a already Dirty
    EXPECT_EQ(graph.state(a), ComputeState::Dirty);
    EXPECT_EQ(graph.state(b), ComputeState::Dirty); // propagation still happened
}

TEST(DependencyGraphTests, RecoveryAfterFailure) {
    // A failed pass leaves b and c Failed. Fixing the callback and issuing
    // markDirty(b) + recompute must restore every node to Valid: markDirty
    // must convert Failed dependents to Dirty so recompute re-runs them.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    EXPECT_TRUE(graph.addDependency(b, a));
    EXPECT_TRUE(graph.addDependency(c, b));

    graph.recompute([&](ObjectId id) { return id != b; }); // b fails
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(graph.state(c), ComputeState::Failed);

    // "Fix" the model, mark the failed node dirty, recompute.
    EXPECT_TRUE(graph.markDirty(b));
    EXPECT_EQ(graph.state(b), ComputeState::Dirty);
    EXPECT_EQ(graph.state(c), ComputeState::Dirty); // Failed dependent re-dirtied

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    EXPECT_TRUE(contains(invoked, b) && contains(invoked, c));
    EXPECT_FALSE(contains(invoked, a)); // untouched upstream not re-run
    EXPECT_TRUE(report.failed.empty());
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Valid);
    EXPECT_EQ(graph.state(c), ComputeState::Valid);
}

TEST(DependencyGraphTests, MarkDirtyOnSuppressedNode) {
    // markDirty on a Suppressed node leaves it Suppressed but still dirties
    // its dependents.
    DependencyGraph graph;
    const ObjectId s = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(s) && graph.addNode(c));
    EXPECT_TRUE(graph.addDependency(c, s));
    graph.recompute(alwaysOk());
    EXPECT_TRUE(graph.setSuppressed(s, true));
    EXPECT_TRUE(graph.setState(c, ComputeState::Valid));

    EXPECT_TRUE(graph.markDirty(s));
    EXPECT_EQ(graph.state(s), ComputeState::Suppressed); // unchanged
    EXPECT_EQ(graph.state(c), ComputeState::Dirty);      // dependent still dirtied
}

TEST(DependencyGraphTests, FailurePropagatesThroughSuppressed) {
    // a -> s(Suppressed) -> c: a fails; s stays Suppressed (and is reported in
    // skippedSuppressed), c becomes Failed.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId s = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(s) && graph.addNode(c));
    EXPECT_TRUE(graph.addDependency(s, a));
    EXPECT_TRUE(graph.addDependency(c, s));
    EXPECT_TRUE(graph.setSuppressed(s, true));

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return false; // a fails (only a is invokable)
    });

    EXPECT_EQ(invoked, std::vector<ObjectId>{a});
    EXPECT_EQ(graph.state(a), ComputeState::Failed);
    EXPECT_EQ(graph.state(s), ComputeState::Suppressed);
    EXPECT_EQ(graph.state(c), ComputeState::Failed);
    EXPECT_EQ(report.skippedSuppressed, std::vector<ObjectId>{s});
    EXPECT_EQ(report.failed, (std::vector<ObjectId>{a, c}));
}

TEST(DependencyGraphTests, CallbackExceptionTreatedAsFailure) {
    // A throwing callback must not escape recompute; the node is Failed and
    // downstream propagation behaves as for a normal failure.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b));
    EXPECT_TRUE(graph.addDependency(b, a));

    const RecomputeReport report = graph.recompute([&](ObjectId id) -> bool {
        if (id == a) throw std::runtime_error("boom");
        return true;
    });
    EXPECT_EQ(graph.state(a), ComputeState::Failed);
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(report.failed, (std::vector<ObjectId>{a, b}));
    EXPECT_TRUE(report.recomputed.empty());
}

// ---------------------------------------------------------------------------
// Review-fix regression tests (stale-failure gating, edge-change dirtying).
// ---------------------------------------------------------------------------

TEST(DependencyGraphTests, StaleFailureGatesRecompute) {
    // a -> c and b -> c. Pass 1 fails b (so b and c are Failed). markDirty(a)
    // re-dirties c. Pass 2 must NOT invoke c: its prerequisite b is still
    // Failed from the previous pass, so c is set Failed and reported again.
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    EXPECT_TRUE(graph.addDependency(c, a)); // a -> c
    EXPECT_TRUE(graph.addDependency(c, b)); // b -> c

    graph.recompute([&](ObjectId id) { return id != b; }); // pass 1: b fails
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(graph.state(c), ComputeState::Failed);

    EXPECT_TRUE(graph.markDirty(a)); // re-dirties a and c; b stays Failed

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    EXPECT_FALSE(contains(invoked, c)); // c gated by stale-Failed prerequisite b
    EXPECT_EQ(invoked, std::vector<ObjectId>{a});
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(graph.state(c), ComputeState::Failed);
    EXPECT_EQ(report.failed, std::vector<ObjectId>{c});
}

TEST(DependencyGraphTests, StaleFailureGatesThroughSuppressed) {
    // b(Failed from a prior pass) -> s(Suppressed) -> c(Dirty): recompute must
    // mark c Failed without invoking it; s untouched (stays Suppressed, not
    // invoked, not re-reported since nothing upstream ran this pass).
    DependencyGraph graph;
    const ObjectId b = newId();
    const ObjectId s = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(b) && graph.addNode(s) && graph.addNode(c));
    EXPECT_TRUE(graph.addDependency(s, b)); // b -> s
    EXPECT_TRUE(graph.addDependency(c, s)); // s -> c
    EXPECT_TRUE(graph.setSuppressed(s, true));

    graph.recompute([&](ObjectId) { return false; }); // pass 1: b fails
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(graph.state(s), ComputeState::Suppressed);
    EXPECT_EQ(graph.state(c), ComputeState::Failed);

    EXPECT_TRUE(graph.setState(c, ComputeState::Dirty)); // c dirty again; b failure is stale

    std::vector<ObjectId> invoked;
    const RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    EXPECT_TRUE(invoked.empty()); // neither b (Failed) nor s (Suppressed) nor c (gated)
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(graph.state(s), ComputeState::Suppressed);
    EXPECT_EQ(graph.state(c), ComputeState::Failed);
    EXPECT_EQ(report.failed, std::vector<ObjectId>{c});
    EXPECT_TRUE(report.recomputed.empty());
    EXPECT_TRUE(report.skippedSuppressed.empty()); // s untouched: nothing upstream ran
}

TEST(DependencyGraphTests, EdgeChangeDirtiesDependent) {
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    EXPECT_TRUE(graph.addDependency(c, b)); // b -> c
    graph.recompute(alwaysOk());            // all Valid

    // Actual edge insert dirties the dependent and its transitive dependents.
    EXPECT_TRUE(graph.addDependency(b, a)); // a -> b
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Dirty);
    EXPECT_EQ(graph.state(c), ComputeState::Dirty);
    graph.recompute(alwaysOk()); // all Valid again

    // Duplicate edge is a no-op: dirties nothing.
    EXPECT_TRUE(graph.addDependency(b, a));
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Valid);
    EXPECT_EQ(graph.state(c), ComputeState::Valid);

    // Actual edge removal dirties the former dependent and its dependents.
    EXPECT_TRUE(graph.removeDependency(b, a));
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Dirty);
    EXPECT_EQ(graph.state(c), ComputeState::Dirty);
    graph.recompute(alwaysOk()); // all Valid again

    // Absent-edge removal is a no-op: dirties nothing.
    EXPECT_TRUE(graph.removeDependency(b, a));
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Valid);
    EXPECT_EQ(graph.state(c), ComputeState::Valid);
}

TEST(DependencyGraphTests, IdleRecomputeDoesNotRereportPersistedFailure) {
    // a -> b -> c; b fails once. A subsequent recompute with nothing Dirty
    // must invoke nothing, change no states, and re-report no failures
    // (persisted Failed nodes are gates, not fresh failures).
    DependencyGraph graph;
    const ObjectId a = newId();
    const ObjectId b = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(a) && graph.addNode(b) && graph.addNode(c));
    EXPECT_TRUE(graph.addDependency(b, a));
    EXPECT_TRUE(graph.addDependency(c, b));

    graph.recompute([&](ObjectId id) { return id != b; }); // b fails
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(graph.state(c), ComputeState::Failed);

    bool invoked = false;
    const RecomputeReport idle = graph.recompute([&](ObjectId) {
        invoked = true;
        return true;
    });
    EXPECT_FALSE(invoked);
    EXPECT_TRUE(idle.recomputed.empty());
    EXPECT_TRUE(idle.failed.empty()); // long-standing failures not re-reported
    EXPECT_TRUE(idle.skippedSuppressed.empty());
    EXPECT_EQ(graph.state(a), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(graph.state(c), ComputeState::Failed);
}

TEST(DependencyGraphTests, EdgeRewireAcrossFailedPrerequisite) {
    // Interaction of the two review fixes: wiring a Valid node onto a Failed
    // prerequisite dirties it (edge-change fix), then recompute gates it
    // Failed without invoking it (stale-failure fix); unwiring dirties it
    // again and it recovers to Valid.
    DependencyGraph graph;
    const ObjectId b = newId();
    const ObjectId c = newId();
    EXPECT_TRUE(graph.addNode(b) && graph.addNode(c));
    graph.recompute([&](ObjectId id) { return id != b; }); // b Failed, c Valid
    EXPECT_EQ(graph.state(b), ComputeState::Failed);
    EXPECT_EQ(graph.state(c), ComputeState::Valid);

    // Attach c to the Failed b: c is dirtied by the edge change.
    EXPECT_TRUE(graph.addDependency(c, b));
    EXPECT_EQ(graph.state(c), ComputeState::Dirty);

    std::vector<ObjectId> invoked;
    RecomputeReport report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    EXPECT_TRUE(invoked.empty()); // c gated by Failed prerequisite, never invoked
    EXPECT_EQ(graph.state(c), ComputeState::Failed);
    EXPECT_EQ(report.failed, std::vector<ObjectId>{c});

    // Detach c from b: c is dirtied again and recomputes to Valid even though
    // b remains Failed elsewhere in the graph.
    EXPECT_TRUE(graph.removeDependency(c, b));
    EXPECT_EQ(graph.state(c), ComputeState::Dirty);

    invoked.clear();
    report = graph.recompute([&](ObjectId id) {
        invoked.push_back(id);
        return true;
    });
    EXPECT_EQ(invoked, std::vector<ObjectId>{c});
    EXPECT_EQ(graph.state(c), ComputeState::Valid);
    EXPECT_EQ(graph.state(b), ComputeState::Failed); // untouched
    EXPECT_TRUE(report.failed.empty());
}

} // namespace
