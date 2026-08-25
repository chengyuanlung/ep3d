#include "Core/Assembly/Instance.h"

#include "Core/Body/Body.h"
#include "Core/Connector/Connector.h"
#include "Core/Document/DocumentBase.h"
#include "Core/Document/SourceShapeResolver.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Serialization/AssemblyDocumentSerializer.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include <utility>

namespace paramcad {

Instance::Instance(std::string name, std::string sourcePath, std::string bodyName,
                           ObjectId frameId)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), sourcePath_(std::move(sourcePath)),
      bodyName_(std::move(bodyName)), frameId_(frameId) {}

Instance::Instance(ObjectId id, std::string name, ComputeState state,
                           std::string sourcePath, std::string bodyName, ObjectId frameId)
    : id_(RestoreObjectId(id)), name_(std::move(name)), sourcePath_(std::move(sourcePath)),
      bodyName_(std::move(bodyName)), frameId_(frameId), state_(state) {}

const Instance::MateConnector* Instance::findConnector(
    const std::string& name) const noexcept {
    for (const MateConnector& one : connectors_)
        if (one.name == name) return &one;
    return nullptr;
}

RecomputeResult Instance::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        state_ = ComputeState::Failed;
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    // "THAT BODY, IN THAT FILE" -- answered by the shared resolver (M32.2).
    //
    // This used to be a hundred lines here: open the file, decide part or
    // assembly, rebuild it, find the body, take the chain tip. A DRAWING VIEW
    // needs the same sentence answered the same way, and a second copy is how
    // the two would begin to disagree about what an empty body name means.
    //
    // WHAT THE CONNECTORS ARE is still this instance's business, so they are
    // harvested from the document while it is still open -- rebuilt from the
    // file every pass, for the same reason the solid is: the file is the
    // truth, so a connector the part no longer has must stop existing here
    // rather than linger as a stale place a mate could still land on.
    connectors_.clear();
    bool sawAssembly = false;
    const SourceShapeResult resolved = ResolveSourceShape(
        sourcePath_, bodyName_, context, [this, &sawAssembly](const DocumentBase& source) {
            // AN ASSEMBLY'S OWN connectors, not the ones its parts brought in:
            // those belong to the parts and are already spoken for by the
            // mates inside. That distinction falls out for free because
            // connectors live on DocumentBase -- roadmap 21's reuse rule, one
            // level up.
            for (const Connector* connector : source.connectors())
                connectors_.push_back(
                    MateConnector{connector->name(),
                                  source.worldTransform(connector->frameId())});
            sawAssembly = source.type() == DocumentType::Assembly;
        });
    if (!resolved) return fail(resolved.message);

    // WHERE. The frame's world transform, composed by the document -- this
    // holds no transform of its own, so there is no second answer to keep in
    // step (ADR-M10-002).
    const Transform3D placement = context.document.worldTransform(frameId_);
    ShapeResult placed = context.kernel->placeShape(resolved.shape, placement);
    if (!placed)
        return fail(placed.message.empty() ? "could not place '" + name_ + "'" : placed.message);

    currentShape_ = std::move(placed.shape);
    subAssembly_ = sawAssembly;
    state_ = ComputeState::Valid;
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
