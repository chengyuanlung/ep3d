#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Recompute/IRecomputable.h"

namespace paramcad {

// Document-singleton recompute node that turns a BoxFeature's current shape
// plus an assigned Material's density into PartDocument::massProperties()
// (ADR-M3-005). NOT a Feature: it carries no user-facing/persisted identity
// -- it is auto-constructed fresh in every PartDocument (both the plain and
// restore constructors) and is never serialized, exactly like the Origin
// frame (ADR-009 D6 extended).
//
// M3 scoping note (ADR-M3-005): hard-wires to a single active BoxFeature --
// PartDocument::addBoxFeature/restoreBoxFeature call setSource and rewire the
// graph edges each time a box is (re)established. A general multi-feature
// shape-source model is explicit, deferred M4+ work, not an oversight.
class MassPropertiesNode final : public IRecomputable {
public:
    MassPropertiesNode();

    ObjectId id() const noexcept override { return id_; }

    // Which BoxFeature/Material this node reads from at recompute time
    // (kInvalidObjectId means "not configured"). Graph edges are wired
    // separately by the caller (PartDocument); this only updates what
    // recompute() reads.
    void setSource(ObjectId boxFeatureId, ObjectId materialId) noexcept;
    ObjectId boxFeatureId() const noexcept { return boxFeatureId_; }
    ObjectId materialId() const noexcept { return materialId_; }

    // Resolves the source BoxFeature/Material through the registry, applies
    // the density policy (ADR-M3-005: finite and non-negative; zero is
    // valid), and performs the single traceable mm->m unit conversion
    // (ADR-M3-002) before committing into context.document.massProperties().
    // Commits ONLY on full success (transactional, spec 12): no failure path
    // overwrites the document's last valid numbers; they only strip the
    // `valid` flag marking those numbers current (ADR-M3-004).
    RecomputeResult recompute(const RecomputeContext& context) override;

private:
    // A member rather than a file-local helper so it reads as part of this
    // node's failure protocol (see the definition for what it does and why the
    // document-level clear in PartDocument is the primary guarantee).
    static RecomputeResult failAndMarkStale(const RecomputeContext& context, std::string message);

    ObjectId id_;
    ObjectId boxFeatureId_ = kInvalidObjectId;
    ObjectId materialId_ = kInvalidObjectId;
};

} // namespace paramcad
