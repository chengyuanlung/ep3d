#include "Core/Assembly/PartInstance.h"

#include "Core/Body/Body.h"
#include "Core/Connector/Connector.h"
#include "Core/Document/DocumentBase.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include <utility>

namespace paramcad {

PartInstance::PartInstance(std::string name, std::string sourcePath, std::string bodyName,
                           ObjectId frameId)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), sourcePath_(std::move(sourcePath)),
      bodyName_(std::move(bodyName)), frameId_(frameId) {}

PartInstance::PartInstance(ObjectId id, std::string name, ComputeState state,
                           std::string sourcePath, std::string bodyName, ObjectId frameId)
    : id_(RestoreObjectId(id)), name_(std::move(name)), sourcePath_(std::move(sourcePath)),
      bodyName_(std::move(bodyName)), frameId_(frameId), state_(state) {}

const PartInstance::MateConnector* PartInstance::findConnector(
    const std::string& name) const noexcept {
    for (const MateConnector& one : connectors_)
        if (one.name == name) return &one;
    return nullptr;
}

RecomputeResult PartInstance::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        state_ = ComputeState::Failed;
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");
    if (sourcePath_.empty()) return fail("this instance names no part file");

    // READ AGAIN, EVERY REBUILD, and rebuilt from its own features rather than
    // trusted as saved -- an .ep3d holds no geometry at all (ADR-M4-004), so
    // there is nothing else it could mean. The cost is a load and a part
    // recompute per instance per pass, which is real; it is paid for the
    // reason ADR-M22-003 gives, that a cache is a second thing that has to be
    // right about when the file changed.
    LoadResult loaded = loadPartDocumentFromFile(sourcePath_);
    if (!loaded)
        return fail("could not open '" + sourcePath_ + "': " +
                    (loaded.message.empty() ? std::string("not a readable part") : loaded.message));

    PartDocument& part = *loaded.document;
    part.setGeometryKernel(context.kernel);
    part.setSketchSolver(context.sketchSolver);
    const DocumentRecomputeReport built = part.recompute();
    if (!built.success) {
        // NAMED, not "the part failed". The user's next move is to open that
        // file and fix something, and which feature it was is the whole
        // difference between doing that and guessing.
        std::string why;
        for (const RecomputeItemReport& item : built.items) {
            if (item.status == RecomputeStatus::Success || item.message.empty()) continue;
            why += (why.empty() ? "" : "; ") + part.objectName(item.id) + ": " + item.message;
        }
        return fail("'" + sourcePath_ + "' does not build" + (why.empty() ? "" : " -- " + why));
    }

    // WHICH BODY. An empty name means "the only one", and several bodies with
    // no name given is refused WITH THE NAMES -- taking the first would make
    // this instance silently mean a different part the day someone added a
    // body to that file, which is position-as-identity (ADR-M4-004).
    const Body* chosen = nullptr;
    if (bodyName_.empty()) {
        if (part.bodies().size() != 1) {
            std::string names;
            for (const auto& body : part.bodies())
                names += (names.empty() ? "" : ", ") + body->name();
            return fail("'" + sourcePath_ + "' holds " + std::to_string(part.bodies().size()) +
                        " bodies, so this instance has to name one of them: " + names);
        }
        chosen = part.bodies().front().get();
    } else {
        for (const auto& body : part.bodies())
            if (body->name() == bodyName_) chosen = body.get();
        if (chosen == nullptr)
            return fail("'" + sourcePath_ + "' has no body called '" + bodyName_ + "'");
    }

    // The chain TIP: the last solid feature, which is the body as it stands
    // after everything that consumed anything.
    const ISolidFeature* tip = nullptr;
    for (const auto& feature : chosen->features())
        if (const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get()))
            tip = solid;
    if (tip == nullptr || !tip->currentShape().isValid())
        return fail("'" + chosen->name() + "' in '" + sourcePath_ + "' has no solid");

    // WHAT IT OFFERS TO BE MATED BY. The part's own connectors, in the
    // part's own coordinates. Rebuilt from the file every time alongside the
    // solid, for the same reason: the part file is the truth, so a connector
    // the part no longer has must stop existing here too rather than linger
    // as a stale place a mate could still land on.
    connectors_.clear();
    for (const Connector* connector : part.connectors())
        connectors_.push_back(MateConnector{connector->name(),
                                            part.worldTransform(connector->frameId())});

    // WHERE. The frame's world transform, composed by the document -- this
    // holds no transform of its own, so there is no second answer to keep in
    // step (ADR-M10-002).
    const Transform3D placement = context.document.worldTransform(frameId_);
    ShapeResult placed = context.kernel->placeShape(tip->currentShape(), placement);
    if (!placed)
        return fail(placed.message.empty() ? "could not place '" + name_ + "'" : placed.message);

    currentShape_ = std::move(placed.shape);
    state_ = ComputeState::Valid;
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
