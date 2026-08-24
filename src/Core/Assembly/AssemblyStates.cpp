#include "Core/Assembly/AssemblyStates.h"

#include "Core/Geometry/Transform.h"

namespace paramcad {

NamedPosition::NamedPosition(std::string name, std::vector<MateSetting> mates,
                             std::vector<LooseSetting> loose)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), mates_(std::move(mates)),
      loose_(std::move(loose)) {}

NamedPosition::NamedPosition(ObjectId id, std::string name, std::vector<MateSetting> mates,
                             std::vector<LooseSetting> loose)
    : id_(RestoreObjectId(id)), name_(std::move(name)), mates_(std::move(mates)),
      loose_(std::move(loose)) {}

ExplodeView::ExplodeView(std::string name, std::vector<ExplodeStep> steps)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), steps_(std::move(steps)) {}

ExplodeView::ExplodeView(ObjectId id, std::string name, std::vector<ExplodeStep> steps,
                         std::size_t previewCut)
    : id_(RestoreObjectId(id)), name_(std::move(name)), steps_(std::move(steps)),
      preview_(previewCut) {}

Transform3D ExplodeView::displacementOf(ObjectId instanceId) const noexcept {
    // IN STEP ORDER, and composed rather than added: two steps that each turn
    // a part have to turn it twice about the axis it has after the first one,
    // which is what composition means and what adding would get wrong.
    //
    // Only the steps up to the preview position count. That is the whole point
    // of the position: walking it forward is watching the explosion happen.
    Transform3D total = Transform3D::Identity();
    const std::size_t shown = stepsShown();
    for (std::size_t i = 0; i < shown; ++i) {
        if (steps_[i].instanceId != instanceId) continue;
        total = Compose(total, steps_[i].displacement);
    }
    return total;
}

} // namespace paramcad
