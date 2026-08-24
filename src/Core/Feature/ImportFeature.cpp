#include "Core/Feature/ImportFeature.h"

#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/RecomputeContext.h"

#include <cstdint>
#include <string>
#include <utility>

namespace paramcad {

ImportFeature::ImportFeature(std::string name, std::string path, ObjectId materialId)
    : Feature(std::move(name)), path_(std::move(path)), materialId_(materialId) {}

ImportFeature::ImportFeature(ObjectId id, std::string name, ComputeState state, std::string path,
                             ObjectId materialId)
    : Feature(id, std::move(name), state), path_(std::move(path)), materialId_(materialId) {}

bool ImportFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult ImportFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");
    if (path_.empty()) return fail("this import names no file");

    // READ AGAIN, EVERY REBUILD. Not cached: the point of storing the path is
    // that the file is the source of truth, so a source that was re-exported
    // shows up here and a source that went away stops the feature.
    //
    // The cost is a file read per recompute, which is real. It is paid because
    // the alternative -- caching and only re-reading when something says the
    // file changed -- needs a second thing to be right about when that is, and
    // the two would disagree the first time somebody edited the file without
    // touching the model.
    ShapeResult result = context.kernel->importStep(path_);
    if (!result)
        return fail(result.message.empty() ? "could not import '" + path_ + "'"
                                           : result.message);
    if (!result.shape.isValid()) return fail("kernel returned an invalid imported shape");

    // Like a Pad, an import starts from nothing: every face is its own.
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, KernelShape{},
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
