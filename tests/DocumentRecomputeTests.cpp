#include "Core/Document/PartDocument.h"
#include "Core/Recompute/IRecomputable.h"
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace paramcad;

// Test-only recompute double (spec 16): owns its ObjectId, counts executions,
// can be forced to fail, and can append to a shared execution log.
class CountingRecomputable final : public IRecomputable {
public:
    explicit CountingRecomputable(std::string name = {})
        : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

    ObjectId id() const noexcept override { return id_; }

    RecomputeResult recompute(const RecomputeContext&) override {
        ++recomputeCount;
        if (executionLog != nullptr) executionLog->push_back(id_);
        if (shouldFail) return {RecomputeStatus::Failed, name_ + " forced failure"};
        return {RecomputeStatus::Success, {}};
    }

    int recomputeCount = 0;
    bool shouldFail = false;
    std::vector<ObjectId>* executionLog = nullptr;

private:
    ObjectId id_;
    std::string name_;
};

const RecomputeItemReport* findItem(const DocumentRecomputeReport& report, ObjectId id) {
    for (const auto& item : report.items)
        if (item.id == id) return &item;
    return nullptr;
}

std::size_t indexOf(const std::vector<ObjectId>& ids, ObjectId id) {
    for (std::size_t i = 0; i < ids.size(); ++i)
        if (ids[i] == id) return i;
    ADD_FAILURE() << "id " << id << " not found in execution log";
    return ids.size();
}

ComputeState nodeState(const PartDocument& document, ObjectId id) {
    return document.dependencyGraph().state(id);
}

// --- Dirty propagation ------------------------------------------------------

TEST(DocumentRecomputeTest, M2_DIRTY_001_LinearChain_MarkSourceDirtiesWholeChain) {
    // Spec 15 minimum: Parameter P -> stub A -> stub B.
    PartDocument document("Doc");
    Parameter& p = document.addParameter("P", 1.0, UnitType::Millimeter);
    CountingRecomputable a("A"), b("B");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addDependency(a.id(), p.id())); // P -> A
    ASSERT_TRUE(document.addDependency(b.id(), a.id())); // A -> B

    EXPECT_TRUE(document.recompute().success); // everything Valid

    EXPECT_TRUE(document.markDirty(p.id()));
    EXPECT_EQ(nodeState(document, p.id()), ComputeState::Dirty);
    EXPECT_EQ(nodeState(document, a.id()), ComputeState::Dirty);
    EXPECT_EQ(nodeState(document, b.id()), ComputeState::Dirty);
    EXPECT_EQ(p.state(), ParameterState::Dirty); // ADR-011 bridge
}

TEST(DocumentRecomputeTest, M2_DIRTY_002_MidChain_UpstreamUnchanged) {
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), b.id()));
    EXPECT_TRUE(document.recompute().success);

    EXPECT_TRUE(document.markDirty(b.id()));
    EXPECT_EQ(nodeState(document, a.id()), ComputeState::Valid); // unchanged
    EXPECT_EQ(nodeState(document, b.id()), ComputeState::Dirty);
    EXPECT_EQ(nodeState(document, c.id()), ComputeState::Dirty);
}

TEST(DocumentRecomputeTest, M2_DIRTY_003_Branch_BothDependentsDirtied) {
    PartDocument document("Doc");
    Parameter& a = document.addParameter("A", 1.0, UnitType::Unitless);
    CountingRecomputable b("B"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), a.id()));
    EXPECT_TRUE(document.recompute().success);

    EXPECT_TRUE(document.markDirty(a.id()));
    EXPECT_EQ(nodeState(document, b.id()), ComputeState::Dirty);
    EXPECT_EQ(nodeState(document, c.id()), ComputeState::Dirty);
}

TEST(DocumentRecomputeTest, M2_DIRTY_004_UnrelatedNodeStaysClean) {
    PartDocument document("Doc");
    Parameter& a = document.addParameter("A", 1.0, UnitType::Unitless);
    CountingRecomputable b("B"), x("X");
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(x));
    ASSERT_TRUE(document.addDependency(b.id(), a.id())); // A -> B; X unrelated
    EXPECT_TRUE(document.recompute().success);

    EXPECT_TRUE(document.markDirty(a.id()));
    EXPECT_EQ(nodeState(document, b.id()), ComputeState::Dirty);
    EXPECT_EQ(nodeState(document, x.id()), ComputeState::Valid);
}

// --- Parameter dirty-source integration (spec 15, ADR-011) ------------------

TEST(DocumentRecomputeTest, M2_PARAM_001_SetParameterValueDirtiesDependentsAndAutoValidates) {
    PartDocument document("Doc");
    Parameter& width = document.addParameter("Width", 100.0, UnitType::Millimeter);
    CountingRecomputable sketch("Sketch");
    ASSERT_TRUE(document.addRecomputableNode(sketch));
    ASSERT_TRUE(document.addDependency(sketch.id(), width.id()));
    EXPECT_TRUE(document.recompute().success);
    EXPECT_EQ(width.state(), ParameterState::Valid); // auto-validated dirty source

    ASSERT_TRUE(document.setParameterValue(width.id(), 120.0));
    EXPECT_EQ(width.value(), 120.0);
    EXPECT_EQ(width.state(), ParameterState::Dirty); // evaluation validity
    EXPECT_EQ(nodeState(document, width.id()), ComputeState::Dirty);
    EXPECT_EQ(nodeState(document, sketch.id()), ComputeState::Dirty);

    const DocumentRecomputeReport report = document.recompute();
    EXPECT_TRUE(report.success);
    const RecomputeItemReport* item = findItem(report, width.id());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->status, RecomputeStatus::Success);
    EXPECT_EQ(item->message, "dirty source");
    EXPECT_EQ(width.state(), ParameterState::Valid);
    EXPECT_EQ(nodeState(document, width.id()), ComputeState::Valid);

    // Unknown id and non-parameter id are rejected.
    EXPECT_FALSE(document.setParameterValue(ObjectIdGenerator::Next(), 1.0));
    EXPECT_FALSE(document.setParameterValue(sketch.id(), 1.0));
}

// --- Recompute ordering -----------------------------------------------------

TEST(DocumentRecomputeTest, M2_ORDER_001_LinearTopologicalOrder) {
    PartDocument document("Doc");
    std::vector<ObjectId> log;
    CountingRecomputable a("A"), b("B"), c("C");
    a.executionLog = b.executionLog = c.executionLog = &log;
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), b.id()));
    EXPECT_TRUE(document.recompute().success);
    log.clear();

    EXPECT_TRUE(document.markDirty(a.id()));
    EXPECT_TRUE(document.recompute().success);
    EXPECT_EQ(log, (std::vector<ObjectId>{a.id(), b.id(), c.id()}));
}

TEST(DocumentRecomputeTest, M2_ORDER_002_DependencyBeforeDependent) {
    // Diamond A -> (B, C) -> D: for every executed edge X -> Y,
    // index(X) < index(Y).
    PartDocument document("Doc");
    std::vector<ObjectId> log;
    CountingRecomputable a("A"), b("B"), c("C"), d("D");
    a.executionLog = b.executionLog = c.executionLog = d.executionLog = &log;
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addRecomputableNode(d));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), a.id()));
    ASSERT_TRUE(document.addDependency(d.id(), b.id()));
    ASSERT_TRUE(document.addDependency(d.id(), c.id()));
    EXPECT_TRUE(document.recompute().success);
    log.clear();

    EXPECT_TRUE(document.markDirty(a.id()));
    EXPECT_TRUE(document.recompute().success);
    ASSERT_EQ(log.size(), 4u);
    EXPECT_LT(indexOf(log, a.id()), indexOf(log, b.id()));
    EXPECT_LT(indexOf(log, a.id()), indexOf(log, c.id()));
    EXPECT_LT(indexOf(log, b.id()), indexOf(log, d.id()));
    EXPECT_LT(indexOf(log, c.id()), indexOf(log, d.id()));
}

TEST(DocumentRecomputeTest, M2_ORDER_003_CleanNodeNotExecuted) {
    PartDocument document("Doc");
    CountingRecomputable a1("A1"), a2("A2"), b1("B1"), b2("B2");
    for (CountingRecomputable* stub : {&a1, &a2, &b1, &b2})
        ASSERT_TRUE(document.addRecomputableNode(*stub));
    ASSERT_TRUE(document.addDependency(a2.id(), a1.id())); // chain A
    ASSERT_TRUE(document.addDependency(b2.id(), b1.id())); // chain B
    EXPECT_TRUE(document.recompute().success);
    const int b1Count = b1.recomputeCount;
    const int b2Count = b2.recomputeCount;

    EXPECT_TRUE(document.markDirty(a1.id()));
    EXPECT_TRUE(document.recompute().success);
    EXPECT_EQ(a1.recomputeCount, 2);
    EXPECT_EQ(a2.recomputeCount, 2);
    EXPECT_EQ(b1.recomputeCount, b1Count); // clean branch untouched
    EXPECT_EQ(b2.recomputeCount, b2Count);
}

// --- Failure ----------------------------------------------------------------

TEST(DocumentRecomputeTest, M2_FAIL_001_NodeFailureReported) {
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), b.id()));
    b.shouldFail = true;

    const DocumentRecomputeReport report = document.recompute();
    EXPECT_FALSE(report.success);
    const RecomputeItemReport* item = findItem(report, b.id());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->status, RecomputeStatus::Failed); // callback ran and failed
    EXPECT_EQ(item->message, "B forced failure");
    EXPECT_EQ(nodeState(document, b.id()), ComputeState::Failed);
}

TEST(DocumentRecomputeTest, M2_FAIL_002_DownstreamBlockedNotExecuted) {
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), b.id()));
    b.shouldFail = true;

    const DocumentRecomputeReport report = document.recompute();
    EXPECT_FALSE(report.success);
    EXPECT_EQ(c.recomputeCount, 0); // never invoked
    const RecomputeItemReport* item = findItem(report, c.id());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->status, RecomputeStatus::BlockedByDependency);
    // Diagnostic names the first failed direct prerequisite (spec 21).
    EXPECT_NE(item->message.find("blocked by failed prerequisite"), std::string::npos)
        << item->message;
    EXPECT_NE(item->message.find(std::to_string(b.id())), std::string::npos) << item->message;
    EXPECT_EQ(nodeState(document, c.id()), ComputeState::Failed);
}

TEST(DocumentRecomputeTest, M2_FAIL_003_RecoveryAfterFix) {
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), b.id()));
    b.shouldFail = true;
    EXPECT_FALSE(document.recompute().success); // 1. B fails

    b.shouldFail = false;                    // 2. fix B
    EXPECT_TRUE(document.markDirty(b.id())); // 3. mark dirty / retry
    const DocumentRecomputeReport retry = document.recompute();
    EXPECT_TRUE(retry.success); // 6. final report succeeds
    EXPECT_EQ(b.recomputeCount, 2); // 4. B succeeds
    EXPECT_EQ(c.recomputeCount, 1); // 5. C can execute
    EXPECT_EQ(nodeState(document, b.id()), ComputeState::Valid);
    EXPECT_EQ(nodeState(document, c.id()), ComputeState::Valid);
}

TEST(DocumentRecomputeTest, M2_FAIL_004_IdlePassStillReportsPersistedFailure) {
    // Review fix regression: a node fails in pass 1. Calling recompute() AGAIN
    // with nothing newly dirtied and the failure not fixed must still report
    // success == false, with the failed node present as a Failed item -- not
    // silently dropped just because the graph itself has nothing new to say
    // on an idle pass (DependencyGraph::recompute only reports TRANSITIONS,
    // which is correct at the generic-graph level per ADR-007; the document
    // engine must not let that idle-pass silence leak into `success`).
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    b.shouldFail = true;

    const DocumentRecomputeReport first = document.recompute();
    EXPECT_FALSE(first.success);
    const RecomputeItemReport* firstItem = findItem(first, b.id());
    ASSERT_NE(firstItem, nullptr);
    EXPECT_EQ(firstItem->status, RecomputeStatus::Failed);

    const int bCountAfterFirstPass = b.recomputeCount;
    const DocumentRecomputeReport idle = document.recompute(); // nothing newly dirtied
    EXPECT_FALSE(idle.success);
    const RecomputeItemReport* idleItem = findItem(idle, b.id());
    ASSERT_NE(idleItem, nullptr) << "persisted Failed node vanished from an idle-pass report";
    EXPECT_EQ(idleItem->status, RecomputeStatus::Failed);
    EXPECT_EQ(b.recomputeCount, bCountAfterFirstPass); // not re-invoked
    EXPECT_EQ(nodeState(document, b.id()), ComputeState::Failed);
}

TEST(DocumentRecomputeTest, M2_FAIL_005_IdlePassStillReportsBlockedDownstream) {
    // Same defect, with a downstream node: on the idle pass, the blocked
    // dependent must still appear as BlockedByDependency, not disappear.
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), b.id()));
    b.shouldFail = true;

    const DocumentRecomputeReport first = document.recompute();
    EXPECT_FALSE(first.success);
    ASSERT_NE(findItem(first, c.id()), nullptr);
    EXPECT_EQ(findItem(first, c.id())->status, RecomputeStatus::BlockedByDependency);

    const int cCountAfterFirstPass = c.recomputeCount; // should be 0 throughout
    const DocumentRecomputeReport idle = document.recompute(); // still nothing dirtied
    EXPECT_FALSE(idle.success);
    const RecomputeItemReport* bItem = findItem(idle, b.id());
    ASSERT_NE(bItem, nullptr) << "persisted Failed node vanished from an idle-pass report";
    EXPECT_EQ(bItem->status, RecomputeStatus::Failed);
    const RecomputeItemReport* cItem = findItem(idle, c.id());
    ASSERT_NE(cItem, nullptr) << "persisted BlockedByDependency node vanished from an idle-pass report";
    EXPECT_EQ(cItem->status, RecomputeStatus::BlockedByDependency);
    EXPECT_NE(cItem->message.find(std::to_string(b.id())), std::string::npos) << cItem->message;
    EXPECT_EQ(c.recomputeCount, cCountAfterFirstPass); // never invoked, still 0
    EXPECT_EQ(nodeState(document, b.id()), ComputeState::Failed);
    EXPECT_EQ(nodeState(document, c.id()), ComputeState::Failed);
}

// --- Cycle ------------------------------------------------------------------

TEST(DocumentRecomputeTest, M2_CYCLE_001_RejectCycle) {
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id())); // A -> B
    ASSERT_TRUE(document.addDependency(c.id(), b.id())); // B -> C

    const GraphResult cycle = document.addDependency(a.id(), c.id()); // C -> A closes cycle
    EXPECT_FALSE(cycle);
    EXPECT_EQ(cycle.error, GraphError::WouldCreateCycle);

    const GraphResult selfEdge = document.addDependency(a.id(), a.id());
    EXPECT_FALSE(selfEdge);
    EXPECT_EQ(selfEdge.error, GraphError::WouldCreateCycle);
}

TEST(DocumentRecomputeTest, M2_CYCLE_002_GraphUnchangedAfterRejection) {
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), b.id()));

    EXPECT_FALSE(document.addDependency(a.id(), c.id()));
    // Valid edges remain, no illegal edge exists.
    const DependencyGraph& graph = document.dependencyGraph();
    EXPECT_EQ(graph.dependentsOf(a.id()), std::vector<ObjectId>{b.id()});
    EXPECT_EQ(graph.dependentsOf(c.id()), std::vector<ObjectId>{});
    EXPECT_EQ(graph.prerequisitesOf(a.id()), std::vector<ObjectId>{});
    // Recompute still works on the intact graph.
    EXPECT_TRUE(document.recompute().success);
    EXPECT_EQ(nodeState(document, c.id()), ComputeState::Valid);
}

// --- Deletion ---------------------------------------------------------------

TEST(DocumentRecomputeTest, M2_DEL_001_DeleteIsolatedObject) {
    PartDocument document("Doc");
    Parameter& parameter = document.addParameter("P", 1.0, UnitType::Unitless);
    const ObjectId id = parameter.id();

    EXPECT_TRUE(document.removeObject(id));
    EXPECT_FALSE(document.objectRegistry().contains(id));
    EXPECT_FALSE(document.dependencyGraph().hasNode(id));
    EXPECT_EQ(document.parameters().findById(id), nullptr); // owner erased
    EXPECT_FALSE(document.removeObject(id)); // second delete: controlled false
}

TEST(DocumentRecomputeTest, M2_DEL_002_DeleteConnectedObjectCleansEdges) {
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), b.id()));
    EXPECT_TRUE(document.recompute().success);

    EXPECT_TRUE(document.removeObject(b.id()));
    EXPECT_FALSE(document.objectRegistry().contains(b.id()));
    EXPECT_FALSE(document.dependencyGraph().hasNode(b.id()));
    // Incoming and outgoing edges are gone on both former neighbors.
    EXPECT_TRUE(document.dependencyGraph().dependentsOf(a.id()).empty());
    EXPECT_TRUE(document.dependencyGraph().prerequisitesOf(c.id()).empty());
    // Former dependent re-dirtied (its input set changed, ADR-007).
    EXPECT_EQ(nodeState(document, c.id()), ComputeState::Dirty);
}

TEST(DocumentRecomputeTest, M2_DEL_003_RecomputeAfterDeletion) {
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    ASSERT_TRUE(document.addDependency(c.id(), b.id()));
    EXPECT_TRUE(document.recompute().success);
    ASSERT_TRUE(document.removeObject(b.id()));

    // No dangling access: the remaining graph recomputes normally.
    const DocumentRecomputeReport report = document.recompute();
    EXPECT_TRUE(report.success);
    EXPECT_EQ(a.recomputeCount, 1); // untouched clean node
    EXPECT_EQ(c.recomputeCount, 2); // re-ran after losing its prerequisite
    EXPECT_EQ(findItem(report, b.id()), nullptr); // deleted node never appears
    EXPECT_EQ(nodeState(document, c.id()), ComputeState::Valid);
}

// --- Suppression (documented rule, contract 5) ------------------------------

TEST(DocumentRecomputeTest, M2_SUP_001_SuppressedNodeNotExecuted) {
    PartDocument document("Doc");
    Parameter& p = document.addParameter("P", 1.0, UnitType::Unitless);
    CountingRecomputable s("S"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(s));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(s.id(), p.id()));
    ASSERT_TRUE(document.addDependency(c.id(), s.id()));
    EXPECT_TRUE(document.recompute().success);
    ASSERT_TRUE(document.setSuppressed(s.id(), true));

    EXPECT_TRUE(document.markDirty(p.id()));
    const DocumentRecomputeReport report = document.recompute();
    EXPECT_EQ(s.recomputeCount, 1); // not executed while suppressed
    const RecomputeItemReport* item = findItem(report, s.id());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->status, RecomputeStatus::SkippedSuppressed);
    EXPECT_EQ(nodeState(document, s.id()), ComputeState::Suppressed);
}

TEST(DocumentRecomputeTest, M2_SUP_002_DownstreamOfSuppressedExecutes) {
    // Documented M2 rule: dirtiness propagates THROUGH a suppressed node and
    // its Dirty dependents execute normally (M2 nodes have no output
    // contract, so no false success is expressible; real cached-output
    // semantics arrive with CAD features in M3).
    PartDocument document("Doc");
    Parameter& p = document.addParameter("P", 1.0, UnitType::Unitless);
    CountingRecomputable s("S"), c("C");
    ASSERT_TRUE(document.addRecomputableNode(s));
    ASSERT_TRUE(document.addRecomputableNode(c));
    ASSERT_TRUE(document.addDependency(s.id(), p.id()));
    ASSERT_TRUE(document.addDependency(c.id(), s.id()));
    EXPECT_TRUE(document.recompute().success);
    ASSERT_TRUE(document.setSuppressed(s.id(), true));

    EXPECT_TRUE(document.markDirty(p.id()));
    EXPECT_EQ(nodeState(document, c.id()), ComputeState::Dirty); // through S
    const DocumentRecomputeReport report = document.recompute();
    EXPECT_TRUE(report.success);
    EXPECT_EQ(c.recomputeCount, 2); // downstream executed
    const RecomputeItemReport* item = findItem(report, c.id());
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(item->status, RecomputeStatus::Success);

    // Unsuppress -> Dirty -> next recompute executes the node again.
    ASSERT_TRUE(document.setSuppressed(s.id(), false));
    EXPECT_EQ(nodeState(document, s.id()), ComputeState::Dirty);
    EXPECT_TRUE(document.recompute().success);
    EXPECT_EQ(s.recomputeCount, 2);
}

// --- Dependency direction (contract 3; names document the rule) -------------

TEST(DocumentRecomputeTest, M2_DIR_001_EdgePointsPrerequisiteToDependent_DirtyFlowsDownstream) {
    PartDocument document("Doc");
    Parameter& width = document.addParameter("Width", 1.0, UnitType::Millimeter);
    CountingRecomputable sketch("Sketch");
    ASSERT_TRUE(document.addRecomputableNode(sketch));
    // Width -> Sketch: Sketch depends on Width.
    ASSERT_TRUE(document.addDependency(sketch.id(), width.id()));
    EXPECT_TRUE(document.recompute().success);

    // Dirtiness flows downstream (Width to Sketch) ...
    EXPECT_TRUE(document.markDirty(width.id()));
    EXPECT_EQ(nodeState(document, sketch.id()), ComputeState::Dirty);
    EXPECT_TRUE(document.recompute().success);
    // ... and never upstream (Sketch to Width).
    EXPECT_TRUE(document.markDirty(sketch.id()));
    EXPECT_EQ(nodeState(document, width.id()), ComputeState::Valid);
}

TEST(DocumentRecomputeTest, M2_DIR_002_AddDependencyArgs_DependentFirst_PrerequisiteSecond) {
    PartDocument document("Doc");
    Parameter& width = document.addParameter("Width", 1.0, UnitType::Millimeter);
    CountingRecomputable sketch("Sketch");
    ASSERT_TRUE(document.addRecomputableNode(sketch));

    // addDependency(dependent, prerequisite): "dependent consumes prerequisite".
    ASSERT_TRUE(document.addDependency(/*dependent=*/sketch.id(), /*prerequisite=*/width.id()));
    EXPECT_EQ(document.dependencyGraph().dependentsOf(width.id()),
              std::vector<ObjectId>{sketch.id()});
    EXPECT_EQ(document.dependencyGraph().prerequisitesOf(sketch.id()),
              std::vector<ObjectId>{width.id()});
}

// --- Release gate (spec 26, verbatim steps 1-13) ----------------------------

TEST(IntegrationTest, M2_GATE_ReleaseScenario) {
    PartDocument document("GatePart");
    Parameter& width = document.addParameter("Width", 100.0, UnitType::Millimeter);
    Parameter& height = document.addParameter("Height", 50.0, UnitType::Millimeter);
    Parameter& unrelated = document.addParameter("Unrelated", 1.0, UnitType::Unitless);
    CountingRecomputable sketch("SketchStub"), pad("PadStub"), mass("MassStub"), other("OtherStub");
    ASSERT_TRUE(document.addRecomputableNode(sketch));
    ASSERT_TRUE(document.addRecomputableNode(pad));
    ASSERT_TRUE(document.addRecomputableNode(mass));
    ASSERT_TRUE(document.addRecomputableNode(other));
    // Width -> Sketch <- Height; Sketch -> Pad -> Mass; Unrelated -> Other.
    ASSERT_TRUE(document.addDependency(sketch.id(), width.id()));
    ASSERT_TRUE(document.addDependency(sketch.id(), height.id()));
    ASSERT_TRUE(document.addDependency(pad.id(), sketch.id()));
    ASSERT_TRUE(document.addDependency(mass.id(), pad.id()));
    ASSERT_TRUE(document.addDependency(other.id(), unrelated.id()));

    // 1. Initial recompute. 2. Record counters.
    EXPECT_TRUE(document.recompute().success);
    const int sketch0 = sketch.recomputeCount;
    const int pad0 = pad.recomputeCount;
    const int mass0 = mass.recomputeCount;
    const int other0 = other.recomputeCount;
    EXPECT_EQ(sketch0, 1);
    EXPECT_EQ(other0, 1);

    // 3-4. Change Width and mark it dirty (one facade call).
    ASSERT_TRUE(document.setParameterValue(width.id(), 120.0));
    EXPECT_EQ(width.value(), 120.0);

    // 5. Recompute. 6. Expected counter deltas.
    EXPECT_TRUE(document.recompute().success);
    EXPECT_EQ(sketch.recomputeCount, sketch0 + 1);
    EXPECT_EQ(pad.recomputeCount, pad0 + 1);
    EXPECT_EQ(mass.recomputeCount, mass0 + 1);
    EXPECT_EQ(other.recomputeCount, other0); // unchanged

    // 7. Force PadStub failure. 8. Change Width again. 9. Recompute.
    pad.shouldFail = true;
    ASSERT_TRUE(document.setParameterValue(width.id(), 140.0));
    const int massBeforeFailure = mass.recomputeCount;
    const DocumentRecomputeReport failedPass = document.recompute();

    // 10. Sketch executes, Pad fails, Mass does not execute successfully.
    EXPECT_FALSE(failedPass.success);
    EXPECT_EQ(sketch.recomputeCount, sketch0 + 2);
    const RecomputeItemReport* padItem = findItem(failedPass, pad.id());
    ASSERT_NE(padItem, nullptr);
    EXPECT_EQ(padItem->status, RecomputeStatus::Failed);
    EXPECT_EQ(padItem->message, "PadStub forced failure");
    EXPECT_EQ(mass.recomputeCount, massBeforeFailure); // never invoked
    const RecomputeItemReport* massItem = findItem(failedPass, mass.id());
    ASSERT_NE(massItem, nullptr);
    EXPECT_EQ(massItem->status, RecomputeStatus::BlockedByDependency);
    EXPECT_NE(massItem->message.find(std::to_string(pad.id())), std::string::npos)
        << massItem->message;
    EXPECT_EQ(nodeState(document, pad.id()), ComputeState::Failed);
    EXPECT_EQ(nodeState(document, mass.id()), ComputeState::Failed);

    // 11. Remove failure. 12. Retry from the failed node. 13. Full success.
    pad.shouldFail = false;
    const DocumentRecomputeReport retry = document.recomputeFrom(pad.id());
    EXPECT_TRUE(retry.success);
    EXPECT_EQ(pad.recomputeCount, pad0 + 3);            // second pass + failed pass + retry
    EXPECT_EQ(mass.recomputeCount, massBeforeFailure + 1);
    EXPECT_EQ(sketch.recomputeCount, sketch0 + 2);       // not re-run by the retry
    EXPECT_EQ(other.recomputeCount, other0);             // still untouched
    for (ObjectId id : {width.id(), height.id(), sketch.id(), pad.id(), mass.id()})
        EXPECT_EQ(nodeState(document, id), ComputeState::Valid);
}

// ---------------------------------------------------------------------------
// Probes added by TEST agent independent verification pass (coordinator
// point 7): cases the required matrix does not explicitly exercise.
// ---------------------------------------------------------------------------

TEST(DocumentRecomputeTest, M2_PROBE_001_MarkDirtyOnIsolatedNodeWithNoDependents) {
    PartDocument document("Doc");
    CountingRecomputable isolated("Isolated");
    ASSERT_TRUE(document.addRecomputableNode(isolated));
    EXPECT_TRUE(document.recompute().success);
    ASSERT_EQ(nodeState(document, isolated.id()), ComputeState::Valid);

    // No dependents, no dependencies: markDirty must still succeed and not
    // touch anything else.
    EXPECT_TRUE(document.markDirty(isolated.id()));
    EXPECT_EQ(nodeState(document, isolated.id()), ComputeState::Dirty);
    const DocumentRecomputeReport report = document.recompute();
    EXPECT_TRUE(report.success);
    EXPECT_EQ(isolated.recomputeCount, 2);

    // Same probe through the Parameter/ADR-011 bridge: a parameter with no
    // graph neighbors.
    Parameter& lonely = document.addParameter("Lonely", 1.0, UnitType::Unitless);
    EXPECT_TRUE(document.recompute().success);
    EXPECT_EQ(lonely.state(), ParameterState::Valid);
    EXPECT_TRUE(document.markDirty(lonely.id()));
    EXPECT_EQ(nodeState(document, lonely.id()), ComputeState::Dirty);
    EXPECT_EQ(lonely.state(), ParameterState::Dirty); // bridge fires even with no dependents
}

TEST(DocumentRecomputeTest, M2_PROBE_002_RecomputeFromAlreadyValidNodeForcesRerun) {
    // Documented on DocumentRecomputeEngine::recomputeFrom: it is
    // markDirty(id) + a full dirty-set recompute, unconditionally -- not a
    // no-op when the node is already Valid.
    PartDocument document("Doc");
    CountingRecomputable a("A"), b("B");
    ASSERT_TRUE(document.addRecomputableNode(a));
    ASSERT_TRUE(document.addRecomputableNode(b));
    ASSERT_TRUE(document.addDependency(b.id(), a.id()));
    EXPECT_TRUE(document.recompute().success);
    ASSERT_EQ(nodeState(document, a.id()), ComputeState::Valid);
    const int aCountBefore = a.recomputeCount;
    const int bCountBefore = b.recomputeCount;

    const DocumentRecomputeReport report = document.recomputeFrom(a.id());
    EXPECT_TRUE(report.success);
    EXPECT_EQ(a.recomputeCount, aCountBefore + 1); // re-run despite being Valid
    EXPECT_EQ(b.recomputeCount, bCountBefore + 1); // dependent re-run too
    EXPECT_EQ(nodeState(document, a.id()), ComputeState::Valid);
    EXPECT_EQ(nodeState(document, b.id()), ComputeState::Valid);
}

TEST(DocumentRecomputeTest, M2_PROBE_003_RecomputeFromUnknownIdReportsFailureWithoutTouchingGraph) {
    PartDocument document("Doc");
    CountingRecomputable a("A");
    ASSERT_TRUE(document.addRecomputableNode(a));
    EXPECT_TRUE(document.recompute().success);
    const int countBefore = a.recomputeCount;

    const ObjectId unknown = ObjectIdGenerator::Next();
    const DocumentRecomputeReport report = document.recomputeFrom(unknown);
    EXPECT_FALSE(report.success);
    ASSERT_EQ(report.items.size(), 1u);
    EXPECT_EQ(report.items[0].id, unknown);
    EXPECT_EQ(report.items[0].status, RecomputeStatus::Failed);
    // Unrelated live node is untouched by the failed recomputeFrom call.
    EXPECT_EQ(a.recomputeCount, countBefore);
    EXPECT_EQ(nodeState(document, a.id()), ComputeState::Valid);
}

TEST(DocumentRecomputeTest, M2_PROBE_004_AddRecomputableNodeTwiceWithSameObjectRejectedDeterministically) {
    PartDocument document("Doc");
    CountingRecomputable a("A");
    ASSERT_TRUE(document.addRecomputableNode(a));
    EXPECT_TRUE(document.objectRegistry().contains(a.id()));
    EXPECT_TRUE(document.dependencyGraph().hasNode(a.id()));

    // Second registration of the SAME still-alive object: rejected, and the
    // first registration is left completely intact (no partial unregister).
    const GraphResult second = document.addRecomputableNode(a);
    EXPECT_FALSE(second);
    EXPECT_EQ(second.error, GraphError::NodeAlreadyExists);
    EXPECT_TRUE(document.objectRegistry().contains(a.id()));
    EXPECT_TRUE(document.dependencyGraph().hasNode(a.id()));
    EXPECT_EQ(document.objectRegistry().findRecomputable(a.id()), &a);

    // Recompute still behaves normally afterwards (registry/graph not corrupted).
    EXPECT_TRUE(document.recompute().success);
    EXPECT_EQ(a.recomputeCount, 1);

    // After removeObject, the same object CAN be re-registered (fresh slot).
    ASSERT_TRUE(document.removeObject(a.id()));
    const GraphResult reAdd = document.addRecomputableNode(a);
    EXPECT_TRUE(reAdd);
    EXPECT_TRUE(document.objectRegistry().contains(a.id()));
}

TEST(DocumentRecomputeTest, M2_PROBE_005_SuppressedParameterEvaluationStateObservation) {
    // Observation (not a required-matrix item): suppressing a Parameter's
    // graph node is not addressed by spec 9's suppression rule, which is
    // written for document/CAD nodes. Because a Suppressed node's callback is
    // never invoked (per DependencyGraph::recompute), the ADR-011
    // auto-validation bridge (which only fires from inside that callback)
    // never runs for a suppressed parameter. Result: setParameterValue on a
    // suppressed parameter sets ParameterState::Dirty but the graph node
    // stays ComputeState::Suppressed (markDirty does not change a Suppressed
    // node's own state), and ParameterState remains stuck at Dirty forever
    // across recompute passes -- ParameterState and ComputeState visibly
    // diverge for as long as the parameter stays suppressed. This is a
    // documented behavior probe, not a DISABLED defect test: nothing here
    // crashes or corrupts the graph, and suppressing a bare Parameter is not
    // a scenario the spec's CAD-node suppression rule contemplates.
    PartDocument document("Doc");
    Parameter& width = document.addParameter("Width", 1.0, UnitType::Millimeter);
    EXPECT_TRUE(document.recompute().success);
    ASSERT_EQ(width.state(), ParameterState::Valid);

    ASSERT_TRUE(document.setSuppressed(width.id(), true));
    ASSERT_EQ(nodeState(document, width.id()), ComputeState::Suppressed);

    ASSERT_TRUE(document.setParameterValue(width.id(), 2.0));
    EXPECT_EQ(width.value(), 2.0);
    EXPECT_EQ(width.state(), ParameterState::Dirty);
    // markDirty does not disturb a Suppressed node's own state.
    EXPECT_EQ(nodeState(document, width.id()), ComputeState::Suppressed);

    const DocumentRecomputeReport report = document.recompute();
    EXPECT_TRUE(report.success); // suppressed nodes never fail the pass
    // ParameterState never returns to Valid while the node stays suppressed:
    // the auto-validation bridge only runs from inside the graph callback,
    // which a Suppressed node never receives.
    EXPECT_EQ(width.state(), ParameterState::Dirty);
    EXPECT_EQ(nodeState(document, width.id()), ComputeState::Suppressed);

    // Unsuppressing releases it: next recompute auto-validates normally.
    ASSERT_TRUE(document.setSuppressed(width.id(), false));
    EXPECT_TRUE(document.recompute().success);
    EXPECT_EQ(width.state(), ParameterState::Valid);
}

} // namespace
