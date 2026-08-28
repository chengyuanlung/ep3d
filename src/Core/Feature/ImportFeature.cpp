#include "Core/Feature/ImportFeature.h"

#include "Core/Export/ExchangeFormat.h"

#include <fstream>

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
    // WHICH FORMAT THE FILE ACTUALLY IS, read from its own first lines and not
    // from its name (M57).
    //
    // This is IsAssemblySourceFile's rule applied where it bites hardest.
    // Exchange files are renamed more than any other kind in a shop: a supplier
    // sends `housing.stp` that is IGES inside, or `part.igs` that a
    // pass-through wrote as STEP. Trusting the name means reporting a STEP
    // syntax error about a perfectly good IGES file, and the reader then goes
    // looking for a fault in the file rather than in the name.
    // A FILE THAT IS NOT THERE AND A FILE THAT IS NOT A CAD FILE ARE DIFFERENT
    // SENTENCES, and the reader's next move differs: one goes looking for the
    // file, the other goes back to whoever sent it. Found by M22's own test,
    // which had been checking for "could not read" and got told about formats.
    const std::optional<ExchangeFormat> format = FormatOfContents(path_);
    if (!format) {
        std::ifstream probe(path_, std::ios::binary);
        if (!probe) return fail("could not read '" + path_ + "': it is not there");
        return fail("'" + path_ + "' is not a file this can read: it is neither STEP nor IGES");
    }
    if (!CanImport(*format))
        return fail(WhyNameRefused(path_, false));

    ShapeResult result = *format == ExchangeFormat::Iges ? context.kernel->importIges(path_)
                                                         : context.kernel->importStep(path_);
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
