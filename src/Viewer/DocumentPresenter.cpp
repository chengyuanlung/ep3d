#include "Viewer/DocumentPresenter.h"
#include "Core/Body/Body.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/Feature.h"
#include "Core/Feature/ISolidFeature.h"

namespace paramcad {

std::vector<ObjectId> DocumentPresenter::displayableSolids() const {
    std::vector<ObjectId> ids;
    for (const auto& body : document_->bodies()) {
        for (const auto& feature : body->features()) {
            // Depend on the CAPABILITY, not on a concrete type: any feature
            // that produces a solid is displayable (ADR-M3-007's rule, the same
            // reason MassPropertiesNode resolves ISolidFeature).
            const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get());
            if (solid == nullptr) continue;
            if (solid->currentState() != ComputeState::Valid) continue;
            if (!solid->currentShape().isValid()) continue;
            if (isHidden(feature->id())) continue; // hidden: computed, not drawn
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
