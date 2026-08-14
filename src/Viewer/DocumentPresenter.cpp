#include "Viewer/DocumentPresenter.h"
#include "Core/Body/Body.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/ISolidFeature.h"
#include <set>

namespace paramcad {

std::vector<ObjectId> DocumentPresenter::displayableSolids() const {
    std::vector<ObjectId> ids;
    for (const auto& body : document_->bodies()) {
        // The chain rule (M8, ADR-M8-003): a solid CONSUMED by a valid
        // downstream feature is an intermediate result, not a part. Drawing the
        // pad underneath its own pocketed successor would overlap two versions
        // of the same material and visually erase the pocket. Collected per
        // body, by capability -- consumedSolidId() -- never by concrete type.
        std::set<ObjectId> consumed;
        for (const auto& feature : body->features()) {
            const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get());
            if (solid == nullptr) continue;
            if (solid->currentState() != ComputeState::Valid) continue;
            if (solid->consumedSolidId() != kInvalidObjectId)
                consumed.insert(solid->consumedSolidId());
        }
        for (const auto& feature : body->features()) {
            // Depend on the CAPABILITY, not on a concrete type: any feature
            // that produces a solid is displayable (ADR-M3-007's rule, the same
            // reason MassPropertiesNode resolves ISolidFeature).
            const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get());
            if (solid == nullptr) continue;
            if (solid->currentState() != ComputeState::Valid) continue;
            if (!solid->currentShape().isValid()) continue;
            if (isHidden(feature->id())) continue; // hidden: computed, not drawn
            if (consumed.count(feature->id()) != 0) continue; // chain intermediate
            ids.push_back(feature->id());
        }
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
