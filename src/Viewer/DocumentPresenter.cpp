#include "Viewer/DocumentPresenter.h"
#include "Core/Body/Body.h"
#include "Core/Document/PartDocument.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/ISolidFeature.h"
#include <set>

namespace paramcad {

std::vector<ObjectId> DocumentPresenter::displayableSolids() const {
    std::vector<ObjectId> ids;
    // The chain rule (M8, ADR-M8-003): a solid CONSUMED by a downstream
    // feature is an intermediate result, not a part. Drawing the pad
    // underneath its own pocketed successor would overlap two versions of the
    // same material and visually erase the pocket. Collected DOCUMENT-wide,
    // by capability -- consumedSolidId() -- never by concrete type. The set
    // was originally per body; round 1 (R1-M2) reached a cross-body consumer
    // through the facade and both base and consumer displayed. Creation now
    // refuses cross-body bases, so the document-wide set is defense in depth.
    std::set<ObjectId> consumed;
    for (const auto& body : document_->bodies())
        for (const auto& feature : body->features()) {
            const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get());
            if (solid == nullptr) continue;
            // REGARDLESS of the consumer's state -- and Gate H had to fail to
            // teach this. The first version counted only Valid consumers, so a
            // FAILED pocket un-consumed its base and the viewer fell back to
            // the bare pad: a healthy-looking solid that is not the part, shown
            // precisely when the part is broken -- spec 34's "stale geometry
            // displayed as current" wearing a fresh coat. An intermediate is
            // structural: once something consumes a solid, that solid alone is
            // never again the thing to draw, whatever state its consumer is in.
            //
            // ...but an INACTIVE consumer does not consume (M9, ADR-M9-002).
            // The distinction from the Failed case above is the whole point:
            // a failed consumer's result is missing or wrong and the part IS
            // broken, so falling back to its base would hide that. A SUPPRESSED
            // or ROLLED-BACK consumer is switched off because the user switched
            // it off, and showing what the model produces without it is exactly
            // what they asked to see. Treating the two alike would either
            // resurrect the M8 defect or make a rolled-back body display
            // nothing at all.
            if (!document_->isFeatureActive(feature->id())) continue;
            if (solid->consumedSolidId() != kInvalidObjectId)
                consumed.insert(document_->activeChainBase(solid->consumedSolidId()));
        }
    for (const auto& body : document_->bodies()) {
        for (const auto& feature : body->features()) {
            // Depend on the CAPABILITY, not on a concrete type: any feature
            // that produces a solid is displayable (ADR-M3-007's rule, the same
            // reason MassPropertiesNode resolves ISolidFeature).
            const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get());
            if (solid == nullptr) continue;
            // A feature the document is not evaluating is not a part to draw.
            if (!document_->isFeatureActive(feature->id())) continue;
            if (solid->currentState() != ComputeState::Valid) continue;
            if (!solid->currentShape().isValid()) continue;
            if (isHidden(feature->id())) continue; // hidden: computed, not drawn
            if (consumed.count(feature->id()) != 0) continue; // chain intermediate
            ids.push_back(feature->id());
        }
    }
    return ids;
}

std::vector<ObjectId> DocumentPresenter::displayableSketches() const {
    std::vector<ObjectId> ids;
    for (const Sketch* sketch : document_->sketches()) {
        if (isHidden(sketch->id())) continue; // hidden: still solved, not drawn
        // An EMPTY sketch is not drawn. Every new sketch starts with an origin
        // point, so "empty" here means the user deleted even that -- and a
        // scene object consisting of one invisible marker is something they can
        // select and cannot see.
        if (sketch->entities().empty()) continue;
        ids.push_back(sketch->id());
    }
    return ids;
}

bool DocumentPresenter::recomputeForDisplay() {
    return document_->recompute().success;
}

void DocumentPresenter::setHidden(ObjectId id, bool hidden) {
    if (hidden) hidden_.insert(id);
    else hidden_.erase(id);
}

bool DocumentPresenter::isHidden(ObjectId id) const { return hidden_.count(id) != 0; }

} // namespace paramcad
