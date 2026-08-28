#include "Core/Feature/ImportFeature.h"

#include "Core/Document/ResolveObject.h"
#include "Core/Export/ExchangeFormat.h"
#include "Core/Parameter/Parameter.h"

#include <fstream>

#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Recompute/RecomputeContext.h"

#include <cstdint>
#include <string>
#include <utility>

namespace paramcad {

ImportFeature::ImportFeature(std::string name, std::string path, ObjectId materialId,
                             ObjectId thicknessParameterId)
    : Feature(std::move(name)), path_(std::move(path)), materialId_(materialId),
      thicknessParameterId_(thicknessParameterId) {}

ImportFeature::ImportFeature(ObjectId id, std::string name, ComputeState state, std::string path,
                             ObjectId materialId, ObjectId thicknessParameterId)
    : Feature(id, std::move(name), state), path_(std::move(path)), materialId_(materialId),
      thicknessParameterId_(thicknessParameterId) {}

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

    // NO SOLID IN THE FILE, BUT A THICKNESS TO GIVE ITS SURFACES (M59).
    //
    // Tried only when the solid path has already failed, so a file that has a
    // real solid is never approximated by an offset of its skin -- and a file
    // that gains one later stops being approximated without anybody clearing a
    // field.
    if (!result && thicknessParameterId_ != kInvalidObjectId) {
        const Parameter* thickness = ResolveParameter(context.registry, thicknessParameterId_);
        if (thickness == nullptr)
            return fail("the thickness this import offsets its surfaces by is gone");
        const ShapeResult skin = context.kernel->importSurfaces(path_);
        if (!skin)
            return fail(skin.message.empty()
                            ? "'" + path_ + "' has neither a solid nor surfaces in it"
                            : skin.message);
        // A CLOSED SKIN IS A SOLID WAITING TO BE TOLD SO, and an open one is
        // what the thickness is for. Which it is comes from the file rather
        // than from a second field, because it is a fact about the geometry
        // and not a choice: a supplier's closed surface model wants to be the
        // part it already describes, and a surface model with holes in it
        // wants a wall.
        result = context.kernel->solidFromSkin(skin.shape);
        if (!result) result = context.kernel->thickenSurface(skin.shape, thickness->value());
        if (!result)
            return fail(result.message.empty() ? "the surfaces in '" + path_ +
                                                     "' could not be made into a part"
                                               : result.message);
    }
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
