#include "Core/Recompute/DocumentRecomputeEngine.h"
#include "Core/Dependency/DependencyGraph.h"
#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/IRecomputable.h"
#include "Core/Recompute/RecomputeContext.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace paramcad {

namespace {

// First failed direct prerequisite of a blocked node, resolvable without
// re-running anything (spec 21); prerequisites are in insertion order, so the
// answer is deterministic.
ObjectId firstFailedPrerequisite(const DependencyGraph& graph, ObjectId id) {
    for (ObjectId prerequisite : graph.prerequisitesOf(id))
        if (graph.state(prerequisite) == ComputeState::Failed) return prerequisite;
    return kInvalidObjectId;
}

} // namespace

DocumentRecomputeEngine::DocumentRecomputeEngine(PartDocument& document) noexcept
    : document_(document) {}

DocumentRecomputeReport DocumentRecomputeEngine::recompute() {
    return run();
}

DocumentRecomputeReport DocumentRecomputeEngine::recomputeFrom(ObjectId id) {
    if (!document_.markDirty(id)) {
        DocumentRecomputeReport report;
        report.success = false;
        report.items.push_back({id, RecomputeStatus::Failed, "unknown node id"});
        return report;
    }
    return run();
}

DocumentRecomputeReport DocumentRecomputeEngine::run() {
    // Sketch constraints can be mutated through a raw Sketch& (see
    // reconcileAllSketchParameterEdges), so the graph is made to agree with the
    // constraint set before the pass rather than assumed to already agree.
    document_.reconcileAllSketchParameterEdges();

    DependencyGraph& graph = document_.graph_;
    ObjectRegistry& registry = document_.registry_;
    const RecomputeContext context{document_, registry, document_.geometryKernel(),
                                   document_.sketchSolver()};

    // Invocation log: which node callbacks actually ran, in execution order.
    // This is what distinguishes Failed from BlockedByDependency afterwards.
    std::vector<ObjectId> invocationOrder;
    std::unordered_set<ObjectId> succeeded;
    std::unordered_map<ObjectId, std::string> messages;

    const RecomputeReport graphReport = graph.recompute([&](ObjectId id) {
        invocationOrder.push_back(id);
        if (IRecomputable* recomputable = registry.findRecomputable(id)) {
            const RecomputeResult result = recomputable->recompute(context);
            messages[id] = result.message;
            if (result.status == RecomputeStatus::Success) {
                succeeded.insert(id);
                return true;
            }
            return false;
        }
        if (const ObjectRegistry::ObjectRef* ref = registry.find(id)) {
            // Registered but not recomputable: a dirty source (Parameter or
            // other input node). Trivially valid in M2 -- scalar parameters
            // have no evaluator; a non-empty expression string still
            // auto-validates (documented limitation, ADR-011).
            if (auto* const* parameter = std::get_if<Parameter*>(ref))
                (*parameter)->markEvaluated();
            messages[id] = "dirty source";
            succeeded.insert(id);
            return true;
        }
        // Unreachable via the document facade (defensive, spec 20).
        messages[id] = "missing registry object";
        return false;
    });

    const std::unordered_set<ObjectId> invoked(invocationOrder.begin(), invocationOrder.end());
    std::unordered_set<ObjectId> represented; // ids already placed into report.items

    DocumentRecomputeReport report;
    // Executed items (Success and Failed) in execution order.
    for (ObjectId id : invocationOrder) {
        represented.insert(id);
        if (succeeded.count(id) != 0) {
            report.items.push_back({id, RecomputeStatus::Success, messages[id]});
        } else {
            report.items.push_back({id, RecomputeStatus::Failed, messages[id]});
            report.success = false;
        }
    }
    // Blocked items: in the graph's failed set but never invoked THIS pass.
    for (ObjectId id : graphReport.failed) {
        if (invoked.count(id) != 0) continue;
        represented.insert(id);
        report.items.push_back(
            {id, RecomputeStatus::BlockedByDependency,
             "blocked by failed prerequisite " +
                 std::to_string(firstFailedPrerequisite(graph, id))});
        report.success = false;
    }
    // Suppressed items next.
    for (ObjectId id : graphReport.skippedSuppressed) {
        represented.insert(id);
        report.items.push_back({id, RecomputeStatus::SkippedSuppressed, "suppressed"});
    }

    // Persisted failures from a PRIOR pass. DependencyGraph::recompute() only
    // reports failures that TRANSITION during the current pass (ADR-007,
    // correct at the generic-graph level -- it deliberately does not re-report
    // long-standing failures on an idle pass). Surfaced unchanged, that would
    // make DocumentRecomputeReport.success misleading: a node whose
    // ComputeState is still Failed from an earlier pass, and that this pass
    // never touched, would silently vanish from the report. Fold every such
    // node in, re-deriving Failed vs BlockedByDependency with the same rule
    // as the live-pass logic above: a direct prerequisite that is itself
    // Failed (which also covers a prerequisite that was BlockedByDependency,
    // since that state is recorded as ComputeState::Failed too) makes this
    // node BlockedByDependency; otherwise it is a standalone Failed node.
    for (ObjectId id : graph.nodes()) {
        if (represented.count(id) != 0) continue;
        if (graph.state(id) != ComputeState::Failed) continue;
        const ObjectId blockingPrerequisite = firstFailedPrerequisite(graph, id);
        if (blockingPrerequisite != kInvalidObjectId) {
            report.items.push_back(
                {id, RecomputeStatus::BlockedByDependency,
                 "blocked by failed prerequisite " + std::to_string(blockingPrerequisite)});
        } else {
            report.items.push_back(
                {id, RecomputeStatus::Failed, "persisted failure from a previous recompute pass"});
        }
        report.success = false;
    }
    return report;
}

} // namespace paramcad
