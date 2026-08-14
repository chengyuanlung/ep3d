// M7.3 — Radius and Diameter reconstruction (spec 12, 14).
//
// The distinction that matters here: Radius and Diameter are two VIEWS of one
// quantity, not two quantities. The model has a single geometric radius, and a
// Diameter constraint drives it as radius = diameter / 2. Anything that treats
// them as separate state is the "duplicated geometry state" spec 12 forbids.

#include "Core/Document/PartDocument.h"
#include "Core/Reconstruction/SketchReconstructor.h"
#include "Core/Sketch/Sketch.h"
#include <gtest/gtest.h>
#include <cmath>
#include <string>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

std::size_t CountKind(const ReconstructionPlan& plan, const char* kindName) {
    std::size_t count = 0;
    for (const PlannedConstraint& constraint : plan.constraints)
        if (std::string(ConstraintKindName(constraint.data)) == kindName) ++count;
    return count;
}

const PlannedParameter* FindParameter(const ReconstructionPlan& plan, const std::string& name) {
    for (const PlannedParameter& parameter : plan.parameters)
        if (parameter.name == name) return &parameter;
    return nullptr;
}

bool HasSkip(const ReconstructionPlan& plan, ReconstructionSkipReason reason) {
    for (const ReconstructionSkip& skip : plan.skipped)
        if (skip.reason == reason) return true;
    return false;
}

// Fixture C (spec 22): centre (25,30), radius 10.
struct CircleFixture {
    PartDocument document{"M7Circle"};
    Sketch* sketch = nullptr;
    SketchEntityId circle{};

    explicit CircleFixture(double radius = 10.0) {
        Sketch& s = document.addSketch("Imported");
        sketch = &s;
        circle = s.addCircle(Vec2{25, 30}, radius);
    }
};

ImportedDimension2D Radial(Vec2 center, double radius) {
    ImportedDimension2D dimension;
    dimension.kind = ImportedDimensionKind::Radius;
    dimension.measureFrom = center;
    // A point on the rim. The direction is whatever ray the leader was drawn
    // along and carries no intent, so a diagonal one is used here deliberately
    // -- a reader that assumed the leader ran along +u would still pass with an
    // axis-aligned point.
    const double k = radius / std::sqrt(2.0);
    dimension.measureTo = Vec2{center.x + k, center.y + k};
    return dimension;
}

ImportedDimension2D Diametric(Vec2 center, double diameter) {
    ImportedDimension2D dimension;
    dimension.kind = ImportedDimensionKind::Diameter;
    const double k = diameter * 0.5 / std::sqrt(2.0);
    dimension.measureFrom = Vec2{center.x - k, center.y - k};
    dimension.measureTo = Vec2{center.x + k, center.y + k};
    return dimension;
}

// --- The measurement itself -------------------------------------------------

TEST(M7Curve, RadiusAndDiameterMeasureTheirOwnPoints) {
    const ImportedDimension2D radial = Radial(Vec2{25, 30}, 10.0);
    EXPECT_NEAR(MeasuredValueMm(radial), 10.0, 1e-12);

    const ImportedDimension2D diametric = Diametric(Vec2{25, 30}, 20.0);
    EXPECT_NEAR(MeasuredValueMm(diametric), 20.0, 1e-12);
}

TEST(M7Curve, ADiametricDimensionsCentreIsDerivedNotRead) {
    const ImportedDimension2D diametric = Diametric(Vec2{25, 30}, 20.0);

    // measureFrom is a point ON the circle, a full radius from the centre.
    // Reading it as the centre -- the obvious mistake, and the one that makes
    // radial and diametric look like they could share a code path -- would put
    // this at about (17.9, 22.9) and match no circle at all.
    const Vec2 center = DimensionCenter(diametric);
    EXPECT_NEAR(center.x, 25.0, 1e-12);
    EXPECT_NEAR(center.y, 30.0, 1e-12);
    EXPECT_GT(std::hypot(diametric.measureFrom.x - 25.0, diametric.measureFrom.y - 30.0), 9.9);
}

// --- Reconstruction ---------------------------------------------------------

TEST(M7Curve, ARadialDimensionBecomesANativeRadiusConstraint) {
    CircleFixture fx;
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {Radial(Vec2{25, 30}, 10.0)});

    EXPECT_EQ(CountKind(plan, "Radius"), 1u);
    EXPECT_EQ(CountKind(plan, "Diameter"), 0u);
    ASSERT_NE(FindParameter(plan, "Radius"), nullptr);
    EXPECT_DOUBLE_EQ(FindParameter(plan, "Radius")->value, 10.0);
    EXPECT_EQ(FindParameter(plan, "Radius")->unit, UnitType::Millimeter);
}

TEST(M7Curve, ADiametricDimensionBecomesANativeDiameterConstraintCarryingTheDiameter) {
    CircleFixture fx; // radius 10
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {Diametric(Vec2{25, 30}, 20.0)});

    EXPECT_EQ(CountKind(plan, "Diameter"), 1u);
    EXPECT_EQ(CountKind(plan, "Radius"), 0u);
    ASSERT_NE(FindParameter(plan, "Diameter"), nullptr);
    // 20, not 10. The Parameter shows what the DRAWING shows, because that is
    // the number the user will read and edit; the constraint kind is what tells
    // the solver to halve it. Storing 10 here would silently redraw the user's
    // diameter callout as a radius.
    EXPECT_DOUBLE_EQ(FindParameter(plan, "Diameter")->value, 20.0);
}

TEST(M7Curve, ARadialDimensionOnAnArcWorksToo) {
    PartDocument document{"M7Fillet"};
    Sketch& sketch = document.addSketch("Imported");
    sketch.addArc(Vec2{25, 30}, 10.0, 0.0, kPi / 2.0, true);

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(document, sketch.id(), {Radial(Vec2{25, 30}, 10.0)});

    // A radial dimension on a fillet is ordinary draughting. Reconstructing a
    // drawing's holes and ignoring its rounds would be an odd place to stop.
    EXPECT_EQ(CountKind(plan, "Radius"), 1u);
}

TEST(M7Curve, TheRadiusIsCrossCheckedAgainstTheCurveItNames) {
    CircleFixture fx; // radius 10
    // Centred right, sized wrong -- the drawing contradicts itself.
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {Radial(Vec2{25, 30}, 40.0)});

    EXPECT_EQ(CountKind(plan, "Radius"), 0u);
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::NoTargetGeometry));
}

TEST(M7Curve, ADimensionCentredOnNothingIsSkipped) {
    CircleFixture fx;
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {Radial(Vec2{0, 0}, 10.0)});

    EXPECT_EQ(CountKind(plan, "Radius"), 0u);
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::NoTargetGeometry));
}

TEST(M7Curve, TwoConcentricCurvesOfTheSameSizeAreAmbiguous) {
    CircleFixture fx;
    fx.sketch->addCircle(Vec2{25, 30}, 10.0); // a duplicate export

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {Radial(Vec2{25, 30}, 10.0)});

    EXPECT_EQ(CountKind(plan, "Radius"), 0u);
    EXPECT_TRUE(HasSkip(plan, ReconstructionSkipReason::AmbiguousTarget));
}

TEST(M7Curve, ConcentricCurvesOfDIFFERENTSizesAreNotAmbiguous) {
    CircleFixture fx;                          // radius 10 at (25,30)
    fx.sketch->addCircle(Vec2{25, 30}, 18.0);  // a counterbore around it

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {Radial(Vec2{25, 30}, 18.0)});

    // The centre alone matches both, which is why the radius cross-check is
    // part of the association rather than a later sanity test. A counterbored
    // hole is ordinary, and refusing it as ambiguous would be a false refusal.
    EXPECT_EQ(CountKind(plan, "Radius"), 1u);
    ASSERT_NE(FindParameter(plan, "Radius"), nullptr);
    EXPECT_DOUBLE_EQ(FindParameter(plan, "Radius")->value, 18.0);
}

TEST(M7Curve, IdenticalHolesElsewhereInTheDrawingAreNotAmbiguous) {
    CircleFixture fx;                         // radius 10 at (25,30)
    fx.sketch->addCircle(Vec2{80, 30}, 10.0); // the same hole, further along

    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {Radial(Vec2{25, 30}, 10.0)});

    // Same size, different place. Matching on radius alone would call every
    // hole pattern ambiguous and reconstruct nothing in the most ordinary
    // drawing there is -- the parallel case to the concentric test above, and
    // the reason association needs BOTH centre and size.
    EXPECT_EQ(CountKind(plan, "Radius"), 1u);
}

// --- Placement (spec 14: "deterministic placement policy if required") ------

TEST(M7Curve, ASketchOfOnlyCirclesStillGetsAFix) {
    CircleFixture fx;
    const ReconstructionPlan plan =
        AnalyzeForReconstruction(fx.document, fx.sketch->id(), {Radial(Vec2{25, 30}, 10.0)});

    // A circle has no endpoints, so the endpoint rule places nothing and this
    // sketch could never reach DOF 0 however many radii were reconstructed.
    // Its centre is the only anchor it has.
    EXPECT_EQ(CountKind(plan, "Fix"), 1u);
    for (const PlannedConstraint& constraint : plan.constraints) {
        const auto* fix = std::get_if<FixConstraint>(&constraint.data);
        if (fix == nullptr) continue;
        EXPECT_EQ(fix->target.entityId, fx.circle);
        EXPECT_EQ(fix->target.subElement, SketchSubElement::CenterPoint);
    }
}

TEST(M7Curve, AddingACircleDoesNotMoveTheFixOffAnExistingEndpoint) {
    PartDocument document{"M7Mixed"};
    Sketch& sketch = document.addSketch("Imported");
    // A circle centred at the origin, and a rectangle further out. The circle
    // centre sorts first lexicographically, so a rule that simply took the
    // smallest anchor of any kind would move the Fix -- relocating every
    // previously-imported part that happens to gain a hole.
    sketch.addCircle(Vec2{-50, -50}, 5.0);
    sketch.addLine(Vec2{0, 0}, Vec2{60, 0});
    sketch.addLine(Vec2{60, 0}, Vec2{60, 40});
    sketch.addLine(Vec2{60, 40}, Vec2{0, 40});
    sketch.addLine(Vec2{0, 40}, Vec2{0, 0});

    const ReconstructionPlan plan = AnalyzeForReconstruction(document, sketch.id(), {});

    EXPECT_EQ(CountKind(plan, "Fix"), 1u);
    for (const PlannedConstraint& constraint : plan.constraints) {
        const auto* fix = std::get_if<FixConstraint>(&constraint.data);
        if (fix == nullptr) continue;
        EXPECT_NE(fix->target.subElement, SketchSubElement::CenterPoint)
            << "the Fix moved onto a circle centre when endpoints were available";
    }
}

// --- One quantity, two views (spec 12) --------------------------------------

TEST(M7Curve, RadiusAndDiameterOnOneCurveDoNotCreateSeparateState) {
    CircleFixture fx; // radius 10
    const ReconstructionPlan plan = AnalyzeForReconstruction(
        fx.document, fx.sketch->id(),
        {Radial(Vec2{25, 30}, 10.0), Diametric(Vec2{25, 30}, 20.0)});

    // Both reconstruct, and both name the SAME entity. They are consistent
    // here (10 and 20), so this is a redundant-but-agreeing pair, not a
    // conflict -- and the model still holds one radius. What must never happen
    // is a second geometric quantity appearing to hold the diameter.
    ASSERT_EQ(plan.parameters.size(), 2u);
    SketchEntityId radiusTarget = kInvalidSketchEntityId;
    SketchEntityId diameterTarget = kInvalidSketchEntityId;
    for (const PlannedConstraint& constraint : plan.constraints) {
        if (const auto* r = std::get_if<RadiusConstraint>(&constraint.data)) radiusTarget = r->curve;
        if (const auto* d = std::get_if<DiameterConstraint>(&constraint.data))
            diameterTarget = d->curve;
    }
    EXPECT_EQ(radiusTarget, fx.circle);
    EXPECT_EQ(diameterTarget, fx.circle);
}

} // namespace
