// M18 -- MEASURING (roadmap 50.2).
//
// Every expected value here is the raw formula, never a call into the thing
// under test: measuring is the tool that will CHECK other work from now on
// (roadmap 50.3.1), so a test that asked it to confirm itself would leave the
// whole validation chain resting on nothing.

#include "Core/Document/PartDocument.h"
#include "Core/Measure/SketchMeasure.h"
#include "Core/Sketch/Sketch.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kPi = 3.14159265358979323846;

const MeasureItem* ItemNamed(const MeasureResult& result, const std::string& label) {
    for (const MeasureItem& item : result.items)
        if (item.label == label) return &item;
    return nullptr;
}

struct Doc {
    PartDocument document{"MeasureDoc"};
    ObjectId sketchId{kInvalidObjectId};
    Doc() { sketchId = document.addSketch("Sketch001").id(); }
    const Sketch& sketch() const { return *document.findSketch(sketchId); }
    SketchEntityId add(SketchGeometry geometry) {
        return document.addSketchEntity(sketchId, std::move(geometry));
    }
};

TEST(SketchMeasureTest, M18_MEA_001_ALinesLengthAndAngle) {
    Doc doc;
    const SketchEntityId line = doc.add(SketchLine{Vec2{10, 10}, Vec2{50, 40}});

    const MeasureResult result = MeasureSketch(doc.sketch(), {SketchElementRef{line}});
    ASSERT_TRUE(result.ok) << result.message;

    ASSERT_NE(ItemNamed(result, "length"), nullptr);
    EXPECT_NEAR(ItemNamed(result, "length")->value, 50.0, 1e-9); // 3-4-5, scaled by ten
    EXPECT_NEAR(ItemNamed(result, "angle")->value, std::atan2(30.0, 40.0), 1e-12);
    EXPECT_FALSE(ItemNamed(result, "length")->approximate);
}

TEST(SketchMeasureTest, M18_MEA_002_ACirclesAreaIsSQUAREMillimetres) {
    // The unit, not the number. Reusing UnitType::Millimeter for an area
    // printed "2827.4 mm" for a 30 mm circle -- a plausible number with the
    // wrong unit on it, which is worse than no number at all.
    Doc doc;
    const SketchEntityId circle = doc.add(SketchCircle{Vec2{0, 0}, 30.0});

    const MeasureResult result = MeasureSketch(doc.sketch(), {SketchElementRef{circle}});
    ASSERT_TRUE(result.ok) << result.message;

    const MeasureItem* area = ItemNamed(result, "area");
    ASSERT_NE(area, nullptr);
    EXPECT_NEAR(area->value, kPi * 900.0, 1e-9);
    EXPECT_EQ(area->unit, MeasureUnit::SquareMillimetre);
    EXPECT_STREQ(MeasureUnitSuffix(area->unit), "mm^2");

    const MeasureItem* perimeter = ItemNamed(result, "perimeter");
    ASSERT_NE(perimeter, nullptr);
    EXPECT_EQ(perimeter->unit, MeasureUnit::Millimetre);
}

TEST(SketchMeasureTest, M18_MEA_003_AnArcsLengthIsRadiusTimesSweep) {
    // A quarter circle of radius 40: length is 40 * pi/2, computed here from
    // the definition rather than from the sweep the measurement reports.
    Doc doc;
    const SketchEntityId arc = doc.add(SketchArc{Vec2{0, 0}, 40.0, 0.0, kPi / 2.0, true});

    const MeasureResult result = MeasureSketch(doc.sketch(), {SketchElementRef{arc}});
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NEAR(ItemNamed(result, "length")->value, 40.0 * kPi / 2.0, 1e-9);
    EXPECT_NEAR(ItemNamed(result, "sweep")->value, kPi / 2.0, 1e-9);
    EXPECT_FALSE(ItemNamed(result, "length")->approximate);
}

TEST(SketchMeasureTest, M18_MEA_004_ACLOCKWISEArcMeasuresTHATPiece) {
    // The same two angles traversed the other way is the OTHER 270 degrees of
    // the same circle. Walking from the smaller angle to the larger would give
    // the right number for a shape nobody drew.
    Doc doc;
    const SketchEntityId arc = doc.add(SketchArc{Vec2{0, 0}, 40.0, 0.0, kPi / 2.0, false});

    const MeasureResult result = MeasureSketch(doc.sketch(), {SketchElementRef{arc}});
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NEAR(ItemNamed(result, "sweep")->value, 3.0 * kPi / 2.0, 1e-9);
    EXPECT_NEAR(ItemNamed(result, "length")->value, 40.0 * 3.0 * kPi / 2.0, 1e-9);
}

TEST(SketchMeasureTest, M18_MEA_005_AnEllipsesPerimeterIsMarkedAPPROXIMATE) {
    // It has no elementary closed form, so it is walked -- and saying so is the
    // point of the flag. A reader told 254.8 mm without being told it is
    // approximate will use it as though it were not.
    //
    // Checked against Ramanujan's approximation, which is a DIFFERENT
    // approximation than the sampling under test: agreeing to within a
    // thousandth of a percent means both are near the true value, whereas
    // comparing the sampler to itself would mean nothing.
    Doc doc;
    const double a = 60.0;
    const double b = 25.0;
    const SketchEntityId oval = doc.add(SketchEllipse{Vec2{0, 0}, a, b, 0.4});

    const MeasureResult result = MeasureSketch(doc.sketch(), {SketchElementRef{oval}});
    ASSERT_TRUE(result.ok) << result.message;
    const MeasureItem* perimeter = ItemNamed(result, "perimeter");
    ASSERT_NE(perimeter, nullptr);
    EXPECT_TRUE(perimeter->approximate);

    const double h = ((a - b) * (a - b)) / ((a + b) * (a + b));
    const double ramanujan = kPi * (a + b) * (1.0 + (3.0 * h) / (10.0 + std::sqrt(4.0 - 3.0 * h)));
    // Ramanujan's second approximation is itself good to about one part in
    // 10^10 at this eccentricity, so agreeing with it to one part in a million
    // means the integration is at least that good too.
    EXPECT_NEAR(perimeter->value, ramanujan, ramanujan * 1e-6);
}

TEST(SketchMeasureTest, M18_MEA_006_ASplinesLengthIsAtLeastItsChords) {
    // A curve through points is never shorter than the straight path through
    // them, and never much longer for a gentle one. Bounds rather than a
    // number: the exact length depends on the interpolation, and asserting a
    // constant here would be asserting the sampler's own answer back at it.
    Doc doc;
    const std::vector<Vec2> points{Vec2{0, 0}, Vec2{40, 30}, Vec2{90, 10}};
    const SketchEntityId spline = doc.add(SketchSpline{points, false});

    double chords = 0.0;
    for (std::size_t i = 1; i < points.size(); ++i)
        chords += std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);

    const MeasureResult result = MeasureSketch(doc.sketch(), {SketchElementRef{spline}});
    ASSERT_TRUE(result.ok) << result.message;
    const MeasureItem* length = ItemNamed(result, "length");
    ASSERT_NE(length, nullptr);
    EXPECT_TRUE(length->approximate);
    EXPECT_GT(length->value, chords);
    EXPECT_LT(length->value, chords * 1.2);
    EXPECT_NEAR(ItemNamed(result, "points")->value, 3.0, 1e-12);
}

TEST(SketchMeasureTest, M18_MEA_007_TwoPointsGiveASeparationWithItsComponents) {
    Doc doc;
    const SketchEntityId line = doc.add(SketchLine{Vec2{10, 10}, Vec2{50, 40}});

    const MeasureResult result =
        MeasureSketch(doc.sketch(), {SketchElementRef{line, SketchSubElement::StartPoint},
                                     SketchElementRef{line, SketchSubElement::EndPoint}});
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NEAR(ItemNamed(result, "distance")->value, 50.0, 1e-9);
    EXPECT_NEAR(ItemNamed(result, "du")->value, 40.0, 1e-9);
    EXPECT_NEAR(ItemNamed(result, "dv")->value, 30.0, 1e-9);
}

TEST(SketchMeasureTest, M18_MEA_008_ASPLINEPointCanBeMeasured) {
    // Nameable since M17.30, so measurable now. A measure that could not reach
    // an interior point would be a second place that knows less about a spline
    // than the constraint model does.
    Doc doc;
    const SketchEntityId spline =
        doc.add(SketchSpline{{Vec2{0, 0}, Vec2{40, 30}, Vec2{90, 10}}, false});

    const MeasureResult result = MeasureSketch(
        doc.sketch(), {SketchElementRef{spline, SketchSubElement::SplinePoint, 1}});
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NEAR(ItemNamed(result, "u")->value, 40.0, 1e-12);
    EXPECT_NEAR(ItemNamed(result, "v")->value, 30.0, 1e-12);
}

TEST(SketchMeasureTest, M18_MEA_009_TwoLinesGiveTheAngleBetweenThem) {
    Doc doc;
    const SketchEntityId a = doc.add(SketchLine{Vec2{0, 0}, Vec2{100, 0}});
    const SketchEntityId b = doc.add(SketchLine{Vec2{0, 0}, Vec2{0, 100}});

    const MeasureResult result =
        MeasureSketch(doc.sketch(), {SketchElementRef{a}, SketchElementRef{b}});
    ASSERT_TRUE(result.ok) << result.message;
    EXPECT_NEAR(ItemNamed(result, "angle")->value, kPi / 2.0, 1e-12);
}

TEST(SketchMeasureTest, M18_MEA_010_ThreeSelectionsAreREFUSEDNotTruncated) {
    // Answering about the first two would report a number about geometry the
    // caller did not ask about, and it would be a plausible number.
    Doc doc;
    const SketchEntityId a = doc.add(SketchLine{Vec2{0, 0}, Vec2{100, 0}});
    const SketchEntityId b = doc.add(SketchLine{Vec2{0, 0}, Vec2{0, 100}});
    const SketchEntityId c = doc.add(SketchLine{Vec2{0, 0}, Vec2{50, 50}});

    const MeasureResult result = MeasureSketch(
        doc.sketch(), {SketchElementRef{a}, SketchElementRef{b}, SketchElementRef{c}});
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.message.find("one entity or two"), std::string::npos) << result.message;
}

TEST(SketchMeasureTest, M18_MEA_011_MeasuringADDSNothingToTheDocument) {
    // The difference between measuring and a driven dimension (roadmap 50.3.4):
    // one is a query and the other is a persistent annotation. A measure that
    // left anything behind would turn looking into editing, and it would show
    // up in the undo stack as a step the user never took.
    Doc doc;
    const SketchEntityId line = doc.add(SketchLine{Vec2{10, 10}, Vec2{50, 40}});
    const std::size_t entitiesBefore = doc.sketch().entities().size();
    const std::size_t constraintsBefore = doc.sketch().constraints().size();

    const MeasureResult result = MeasureSketch(doc.sketch(), {SketchElementRef{line}});
    ASSERT_TRUE(result.ok) << result.message;

    EXPECT_EQ(doc.sketch().entities().size(), entitiesBefore);
    EXPECT_EQ(doc.sketch().constraints().size(), constraintsBefore);
}

} // namespace
