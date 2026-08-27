#include "Core/Feature/SheetContourFeature.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/RecomputeContext.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace paramcad {

namespace {

const Parameter* resolveParameter(const ObjectRegistry& registry, ObjectId id) {
    const std::optional<ObjectRegistry::ConstObjectRef> ref = registry.find(id);
    if (!ref) return nullptr;
    auto* const* parameter = std::get_if<const Parameter*>(&*ref);
    return parameter != nullptr ? *parameter : nullptr;
}

} // namespace

SheetContourFeature::SheetContourFeature(std::string name, SheetContour contour,
                                         ObjectId widthParameterId, ObjectId materialId)
    : Feature(std::move(name)), contour_(std::move(contour)),
      widthParameterId_(widthParameterId), materialId_(materialId) {}

SheetContourFeature::SheetContourFeature(ObjectId id, std::string name, ComputeState state,
                                         SheetContour contour, ObjectId widthParameterId,
                                         ObjectId materialId)
    : Feature(id, std::move(name), state), contour_(std::move(contour)),
      widthParameterId_(widthParameterId), materialId_(materialId) {}

bool SheetContourFeature::recompute() { return false; }

RecomputeResult SheetContourFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    const Parameter* width = resolveParameter(context.registry, widthParameterId_);
    if (width == nullptr) return fail("this sheet contour's width parameter is not here");
    if (!(width->value() > 0.0))
        return fail("a sheet contour with no width is a section, not a part");

    // THE THICKNESS COMES FROM THE PART, every rebuild.
    //
    // Not held here, so setting the part to 3 mm cannot leave this feature
    // building 2 mm walls -- which would fold to a blank the part says is a
    // different length, with both answers self-consistent.
    const SheetMetalSettings& sheet = context.part().sheetMetal();
    if (!sheet.isSheetMetal)
        return fail("this part is not sheet metal, so there is no thickness to build with -- "
                    "say what it is made of first");

    const ContourProfileResult profile =
        ContourProfile(contour_, sheet.material, sheet.thicknessMm);
    if (!profile.ok) return fail(profile.why);

    PlanarProfileDefinition definition;
    // ON THE PART'S OWN XY, looking along Z. A contour is a SECTION: it has no
    // sketch of its own to sit on, because the chain of lengths and angles IS
    // the drawing. Putting it on a sketch would invite the two to disagree
    // about which came first.
    definition.plane = ProfilePlane{};
    definition.segments = profile.segments;

    ShapeResult result = context.kernel->extrudeProfile(definition, width->value());
    if (!result)
        return fail(result.message.empty() ? "the kernel could not build this section"
                                           : result.message);
    if (!result.shape.isValid()) return fail("the kernel returned an invalid shape");

    // WHO MADE THIS (ADR-M17-035). A contour starts from nothing, so every
    // face is its own.
    currentShape_ = context.kernel->tagCreatedFaces(result.shape, KernelShape{},
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
