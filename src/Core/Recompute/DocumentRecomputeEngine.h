#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Recompute/RecomputeTypes.h"

namespace paramcad {

class PartDocument;

// Document-level recompute driver. Wraps the generic DependencyGraph
// recompute with CAD semantics: resolves node ids through the document's
// ObjectRegistry, executes IRecomputable objects, auto-validates dirty-source
// nodes (Parameters), and translates the graph report into a
// DocumentRecomputeReport that distinguishes Failed (callback ran and
// reported failure) from BlockedByDependency (never invoked because a
// prerequisite failed; message names the first failed direct prerequisite so
// a UI can explain the block without re-running anything).
// Mutation entry points live on the PartDocument facade (markDirty etc.);
// the engine only exposes recompute entry points (ADR-011).
class DocumentRecomputeEngine {
public:
    explicit DocumentRecomputeEngine(PartDocument& document) noexcept;

    // Recomputes the current dirty set of the document graph.
    DocumentRecomputeReport recompute();

    // markDirty(id) followed by a full dirty-set recompute. Any OTHER
    // already-dirty nodes also run: the graph recomputes the dirty set, which
    // is the correct incremental behavior -- a strict descendants-only filter
    // would leave stale dirty nodes behind (ADR-011).
    DocumentRecomputeReport recomputeFrom(ObjectId id);

private:
    DocumentRecomputeReport run();

    PartDocument& document_;
};

} // namespace paramcad
