#include "Core/Document/ResolveObject.h"
#include "Core/Feature/HoleFeature.h"

#include "Core/Document/ObjectRegistry.h"
#include "Core/Document/PartDocument.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Recompute/RecomputeContext.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace paramcad {

namespace {

HoleSizes sizesOf(const HoleFeature& hole, double typedDiameterMm, double depthMm) {
    return hole.sizes(typedDiameterMm, depthMm);
}

} // namespace

HoleFeature::HoleFeature(std::string name, ObjectId baseFeatureId, ObjectId sketchId,
                         ObjectId diameterParameterId, ObjectId depthParameterId,
                         ObjectId materialId)
    : Feature(std::move(name)), baseFeatureId_(baseFeatureId), sketchId_(sketchId),
      diameterParameterId_(diameterParameterId), depthParameterId_(depthParameterId),
      materialId_(materialId) {}

HoleFeature::HoleFeature(ObjectId id, std::string name, ComputeState state,
                         ObjectId baseFeatureId, ObjectId sketchId, ObjectId diameterParameterId,
                         ObjectId depthParameterId, ObjectId materialId)
    : Feature(id, std::move(name), state), baseFeatureId_(baseFeatureId), sketchId_(sketchId),
      diameterParameterId_(diameterParameterId), depthParameterId_(depthParameterId),
      materialId_(materialId) {}

bool HoleFeature::recompute() {
    return state() != ComputeState::Failed;
}

RecomputeResult HoleFeature::recompute(const RecomputeContext& context) {
    const auto fail = [this](std::string message) {
        setState(ComputeState::Failed);
        return RecomputeResult{RecomputeStatus::Failed, std::move(message)};
    };

    if (context.kernel == nullptr) return fail("no geometry kernel configured");

    const ISolidFeature* base = ResolveSolidFeature(
        context.registry, context.part().activeChainBase(baseFeatureId_));
    if (base == nullptr) return fail("hole base feature not found or does not produce a solid");
    if (base->currentState() != ComputeState::Valid)
        return fail("hole base feature is not in a valid state");
    if (!base->currentShape().isValid()) return fail("hole base feature has no valid shape");

    const Sketch* sketch = ResolveSketch(context.registry, sketchId_);
    if (sketch == nullptr) return fail("hole sketch not found");
    if (context.part().sketchSupportFrameIsMissing(sketch->id()))
        return fail("hole sketch's support frame is missing");

    const Parameter* diameter = ResolveParameter(context.registry, diameterParameterId_);
    if (diameter == nullptr) return fail("hole diameter parameter not found");
    const Parameter* depth = ResolveParameter(context.registry, depthParameterId_);
    if (depth == nullptr) return fail("hole depth parameter not found");
    // WHAT SIZE, FROM ONE PLACE (M39).
    //
    // The drilled diameter of a hole that names a screw is NOT its parameter:
    // an M8 tapped hole is drilled 6.8 and an M8 clearance hole 9.0. The same
    // call gives the drawing its callout, so a hole cannot be cut to one size
    // and called out at another -- which is the pair this project keeps
    // finding written twice.
    //
    // A depth this small means THROUGH, which the sizing has to know: a
    // through hole is called out THRU and has no depth for a counterbore to be
    // deeper than.
    const bool through = std::fabs(depth->value()) < kMinExtrusionDistanceMm;
    const HoleSizes sizes =
        sizesOf(*this, diameter->value(), through ? 0.0 : std::fabs(depth->value()));
    if (!sizes.ok) return fail(sizes.why);

    // WHERE, from the sketch's POINTS. Construction points count: a point is
    // reference geometry by nature, and a user who marks hole centres as
    // construction has said something about the drawing, not about the holes.
    std::vector<Vec2> centres;
    for (const SketchEntity& entity : sketch->entities())
        if (const auto* point = std::get_if<SketchPoint>(&entity.geometry))
            centres.push_back(point->position);
    if (centres.empty())
        return fail("this hole's sketch has no points, so it would drill nothing -- put a point "
                    "where each hole goes");

    // HOW DEEP. Zero means THROUGH ALL, and the depth is then taken from how
    // far the solid actually reaches rather than from a generous guess: a guess
    // works until somebody builds a part deeper than it, and then the hole
    // stops part-way with nothing to say.
    //
    // The extra millimetre is so the tool emerges cleanly on both faces. A cut
    // that ends exactly ON a face leaves OCCT deciding whether the two are
    // coincident, and a coplanar-face boolean is the one case it is worst at.
    double reach = depth->value();
    if (std::fabs(reach) < kMinExtrusionDistanceMm) {
        const KernelBoundsResult bounds = context.kernel->boundsOfShape(base->currentShape());
        if (!bounds.ok)
            return fail("a through hole needs to know how far the part reaches, and this kernel "
                        "cannot say: " +
                        bounds.message);
        const double span = std::hypot(std::hypot(bounds.max.x - bounds.min.x,
                                                  bounds.max.y - bounds.min.y),
                                       bounds.max.z - bounds.min.z);
        reach = span + 1.0;
    }

    // ONE TOOL PER POINT, cut in turn.
    //
    // Not one profile with several loops: a profile's inner loops are HOLES IN
    // IT, so two circles in one profile describe a ring, not two discs, and the
    // solid would come out with the material between them removed as well.
    ShapeResult carried{base->currentShape(), KernelError::None, {}};
    for (const Vec2& centre : centres) {
        PlanarProfileDefinition tool;
        tool.plane = PlaneOfSketchFrame(context.part().effectiveSketchFrame(sketch->id()));
        tool.segments = {ProfileCircleSegment{centre, sizes.drillDiameterMm / 2.0}};

        // A THROUGH hole runs A FULL SPAN EACH WAY from the sketch plane.
        //
        // Not half a span each way, which is what this did first and what a
        // two-metre part caught: the sketch is usually on the TOP face, so
        // half a span downwards stops in the middle -- and the result is a
        // blind hole that looks exactly like a through one from above.
        //
        // A full span each way covers the part wherever the plane sits on it,
        // because no two points of the part are further apart than that. It
        // also puts both ends clear of the material, which matters: a cut that
        // ends exactly ON a face leaves OCCT deciding whether the two are
        // coincident, and a coplanar-face boolean is the one case it is worst
        // at.
        if (through) {
            ShapeResult behind = context.kernel->extrudeProfile(tool, -reach);
            if (!behind)
                return fail(behind.message.empty() ? "kernel failed to build a hole tool"
                                                   : behind.message);
            ShapeResult ahead = context.kernel->extrudeProfile(tool, reach);
            if (!ahead)
                return fail(ahead.message.empty() ? "kernel failed to build a hole tool"
                                                  : ahead.message);
            ShapeResult joined = context.kernel->fuseShapes(behind.shape, ahead.shape);
            if (!joined)
                return fail(joined.message.empty() ? "kernel failed to join a hole tool"
                                                   : joined.message);
            ShapeResult cut = context.kernel->subtractShape(carried.shape, joined.shape);
            if (!cut) return fail(cut.message.empty() ? "kernel failed to drill a hole"
                                                      : cut.message);
            carried = std::move(cut);
            continue;
        }

        ShapeResult bit = context.kernel->extrudeProfile(tool, reach);
        if (!bit)
            return fail(bit.message.empty() ? "kernel failed to build a hole tool" : bit.message);
        ShapeResult cut = context.kernel->subtractShape(carried.shape, bit.shape);
        if (!cut)
            return fail(cut.message.empty() ? "kernel failed to drill a hole" : cut.message);
        carried = std::move(cut);
    }

    // --- AND THE RECESS AT THE MOUTH (M39) -----------------------------------
    //
    // Cut after every hole, not interleaved, so a counterbore can never be cut
    // into a face another hole has not been drilled through yet.
    if (kind_ != HoleKind::Simple) {
        // WHICH WAY THE RECESS FACES.
        //
        // The same way the hole is drilled, which for a blind hole is the sign
        // of its depth. A THROUGH hole has no sign -- its tool goes both ways
        // -- so the side is worked out from where the MATERIAL is: a sketch
        // made on a face has its normal pointing out of the solid, so the body
        // of the part lies on the other side of the plane from the normal.
        //
        // Guessing instead would fail invisibly. A counterbore cut on the side
        // with no material removes nothing at all, and the drawing still says
        // there is one.
        const ProfilePlane plane =
            PlaneOfSketchFrame(context.part().effectiveSketchFrame(sketch->id()));
        double towardsMaterial = depth->value() < 0.0 ? -1.0 : 1.0;
        if (through) {
            const KernelBoundsResult bounds = context.kernel->boundsOfShape(base->currentShape());
            if (!bounds.ok)
                return fail("a recess on a through hole needs to know which side the material "
                            "is on, and this kernel cannot say: " + bounds.message);
            const Vec3 middle{(bounds.min.x + bounds.max.x) / 2.0,
                              (bounds.min.y + bounds.max.y) / 2.0,
                              (bounds.min.z + bounds.max.z) / 2.0};
            const double along = (middle.x - plane.origin.x) * plane.normal.x +
                                 (middle.y - plane.origin.y) * plane.normal.y +
                                 (middle.z - plane.origin.z) * plane.normal.z;
            towardsMaterial = along < 0.0 ? -1.0 : 1.0;
        }

        // The mouth of the tool starts just OUTSIDE the face. A cut that ends
        // exactly on a face leaves OCCT deciding whether the two are
        // coincident, and a coplanar-face boolean is the one case it is worst
        // at -- the same reason the through tool overshoots at both ends.
        constexpr double kClearOfTheFace = 0.5;
        const Vec3 mouthShift{plane.normal.x * -towardsMaterial * kClearOfTheFace,
                              plane.normal.y * -towardsMaterial * kClearOfTheFace,
                              plane.normal.z * -towardsMaterial * kClearOfTheFace};

        for (const Vec2& centre : centres) {
            PlanarProfileDefinition mouth;
            mouth.plane = plane;
            ShapeResult recess{KernelShape{}, KernelError::None, {}};

            if (kind_ == HoleKind::Counterbore) {
                mouth.segments = {
                    ProfileCircleSegment{centre, sizes.counterboreDiameterMm / 2.0}};
                recess = context.kernel->extrudeProfile(
                    mouth, towardsMaterial * (sizes.counterboreDepthMm + kClearOfTheFace));
            } else {
                // A COUNTERSINK IS A CONE, built as a loft between two circles
                // -- the drill at the bottom and the head diameter at the
                // surface. How far down the cone reaches is fixed by its
                // ANGLE, which is why the angle is part of the sizing and not
                // an assumption made here.
                const double halfAngle = sizes.countersinkAngleDeg * 0.5 * 3.14159265358979323846 / 180.0;
                const double slope = std::tan(halfAngle);
                if (!(slope > 1e-9)) return fail("a countersink needs a real included angle");
                const double bottomRadius = sizes.drillDiameterMm / 2.0;
                const double topRadius = sizes.countersinkDiameterMm / 2.0;
                const double coneDepth = (topRadius - bottomRadius) / slope;
                if (!(coneDepth > 0.0))
                    return fail("this countersink is no wider than the hole it is on");

                PlanarProfileDefinition bottom;
                bottom.plane = plane;
                bottom.plane.origin =
                    Vec3{plane.origin.x + plane.normal.x * towardsMaterial * coneDepth,
                         plane.origin.y + plane.normal.y * towardsMaterial * coneDepth,
                         plane.origin.z + plane.normal.z * towardsMaterial * coneDepth};
                bottom.segments = {ProfileCircleSegment{centre, bottomRadius}};

                // The wide end is carried PAST the face, growing at the cone's
                // own slope so the angle stays exact.
                PlanarProfileDefinition top;
                top.plane = plane;
                top.plane.origin = Vec3{plane.origin.x + mouthShift.x, plane.origin.y + mouthShift.y,
                                        plane.origin.z + mouthShift.z};
                top.segments = {
                    ProfileCircleSegment{centre, topRadius + kClearOfTheFace * slope}};
                recess = context.kernel->loftProfiles({bottom, top});
            }

            if (!recess)
                return fail(recess.message.empty() ? "kernel failed to build a hole's recess"
                                                   : recess.message);
            if (kind_ == HoleKind::Counterbore) {
                ShapeResult moved = context.kernel->translateShape(recess.shape, mouthShift);
                if (!moved)
                    return fail(moved.message.empty() ? "kernel failed to place a counterbore"
                                                      : moved.message);
                recess = std::move(moved);
            }
            ShapeResult cut = context.kernel->subtractShape(carried.shape, recess.shape);
            if (!cut)
                return fail(cut.message.empty() ? "kernel failed to cut a hole's recess"
                                                : cut.message);
            carried = std::move(cut);
        }
    }

    if (!carried.shape.isValid()) return fail("kernel returned an invalid drilled shape");

    // The bores are this feature's; everything the base knew is carried forward.
    currentShape_ = context.kernel->tagCreatedFaces(carried.shape, base->currentShape(),
                                                    static_cast<std::uint64_t>(id()));
    setState(ComputeState::Valid);
    return {RecomputeStatus::Success, {}};
}

} // namespace paramcad
