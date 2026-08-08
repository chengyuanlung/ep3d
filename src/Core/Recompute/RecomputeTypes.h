#pragma once

#include "Core/Document/ObjectId.h"
#include <string>
#include <vector>

namespace paramcad {

// Status of one document node in a recompute pass (ADR-011).
// Failed              = the node's recompute callback ran and reported failure.
// BlockedByDependency = never invoked because a prerequisite failed this pass.
// SkippedSuppressed   = suppressed node inside the affected region; not invoked.
// (Spec 6.3 suggested {Success, Failed, Skipped, Suppressed}; "Skipped" alone
// is ambiguous between blocked and suppressed, so the spec 10 extension is
// adopted -- recorded in ADR-011.)
enum class RecomputeStatus { Success, Failed, BlockedByDependency, SkippedSuppressed };

// Returned by IRecomputable::recompute; success is explicit, no
// exception-based protocol, diagnostic text is retained.
struct RecomputeResult {
    RecomputeStatus status = RecomputeStatus::Success;
    std::string message;
};

struct RecomputeItemReport {
    ObjectId id = kInvalidObjectId;
    RecomputeStatus status = RecomputeStatus::Success;
    std::string message;
};

// Engine-level report. Named DocumentRecomputeReport because RecomputeReport
// already names the generic graph-level report in DependencyGraph.h.
// Item order: executed items in execution order, then blocked, then
// suppressed, then any persisted failure left over from a PRIOR pass (each
// deterministic per ADR-007 ordering; the last group is in graph node
// insertion order).
//
// IMPORTANT: `success` means "the document currently has no unresolved
// Failed or BlockedByDependency node" -- NOT merely "nothing failed during
// this call". DependencyGraph::recompute() (generic, M1/ADR-007, unchanged
// and correct at that layer) only reports failures that TRANSITION during
// the current pass; an idle recompute() call over an already-Failed,
// still-unfixed node reports nothing for it. DocumentRecomputeEngine folds
// such persisted failures back into every report (see DocumentRecomputeEngine
// ::run()) specifically so `success` stays a reliable "is the document
// healthy right now" signal even on a pass where nothing new happened.
struct DocumentRecomputeReport {
    bool success = true;
    std::vector<RecomputeItemReport> items;
};

} // namespace paramcad
