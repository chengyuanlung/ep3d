// M33 -- what a user draws on the sheet, ported from EasyCad.
//
// Every test here asks a GEOMETRIC question with a checkable answer -- where a
// point landed, how long something is, which snap won -- rather than that a
// call returned true. The behaviour being ported was already proved once
// against a working AutoCAD-alike; what has to be proved again is that it
// survived the crossing.

#include "Core/Drawing/DrawingDocument.h"
#include "Core/Drawing/ObjectSnap.h"
#include "Core/Serialization/DrawingDocumentSerializer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>

namespace {

using namespace paramcad;

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi = 3.14159265358979323846;

std::string SaveToString(const DrawingDocument& document) {
    std::ostringstream out;
    EXPECT_TRUE(saveDrawingDocument(document, out));
    return out.str();
}

DrawingLoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadDrawingDocument(in);
}

double Length(Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

} // namespace

// =============================================================================
// The shapes answer for themselves
// =============================================================================

TEST(DrawingEntityTest, M33_GEO_001_AnArcsExtentIsTheArcNotItsCircle) {
    // The same rule ProjectedGeometry follows, and for the same reason: taking
    // the whole circle's box makes every arc claim more room than it uses,
    // which shows up as "this does not fit on the sheet" when it plainly does.
    DrawArc quarter;
    quarter.centre = Vec2{0.0, 0.0};
    quarter.radius = 50.0;
    quarter.startAngle = 0.0;
    quarter.endAngle = kTwoPi / 4.0;
    const Box2D box = BoundsOf(DrawShape{quarter});
    EXPECT_NEAR(box.min.x, 0.0, 1e-9);
    EXPECT_NEAR(box.min.y, 0.0, 1e-9);
    EXPECT_NEAR(box.max.x, 50.0, 1e-9);
    EXPECT_NEAR(box.max.y, 50.0, 1e-9);
}

TEST(DrawingEntityTest, M33_GEO_002_ACircleIsPickedByItsRIMNotItsMiddle) {
    // A circle is a curve. Clicking its middle should select whatever is under
    // the middle -- which on a drawing is usually the thing the circle is a
    // hole in.
    DrawCircle circle;
    circle.centre = Vec2{0.0, 0.0};
    circle.radius = 20.0;
    EXPECT_NEAR(DistanceFrom(DrawShape{circle}, Vec2{20.0, 0.0}), 0.0, 1e-9);
    EXPECT_NEAR(DistanceFrom(DrawShape{circle}, Vec2{0.0, 0.0}), 20.0, 1e-9)
        << "the middle of a circle was treated as being on it";
}

TEST(DrawingEntityTest, M33_GEO_003_AnArcOffItsSweepIsMeasuredToItsENDS) {
    // Measuring to the circle instead would let a click on the far side of a
    // quarter arc select it -- the arc equivalent of picking a line by its
    // infinite extension.
    DrawArc quarter;
    quarter.centre = Vec2{0.0, 0.0};
    quarter.radius = 10.0;
    quarter.startAngle = 0.0;
    quarter.endAngle = kTwoPi / 4.0;
    // Dead opposite the sweep. On the circle, but not on the arc.
    const double distance = DistanceFrom(DrawShape{quarter}, Vec2{-10.0, 0.0});
    EXPECT_GT(distance, 10.0) << "a point on the far side of the circle picked the arc";
    // ...and it reports the distance to the NEAREST end, which is the one at
    // 90 degrees: (0, 10) is 10*sqrt(2) from (-10, 0), not the 20 to the other
    // end. This test asserted 20 at first, which was the test being wrong
    // about which end is nearer -- worth keeping the corrected number rather
    // than the round one.
    EXPECT_NEAR(distance, 10.0 * std::sqrt(2.0), 1e-6);
}

TEST(DrawingEntityTest, M33_GEO_004_ACircleUnderANonUniformScaleBecomesAnELLIPSE) {
    // Scaling the radius by an average would draw a circle where the model has
    // an oval, and every measurement taken off it afterwards would be wrong by
    // a different amount in each direction.
    DrawCircle circle;
    circle.centre = Vec2{0.0, 0.0};
    circle.radius = 10.0;
    Matrix2D squash;
    squash.m00 = 2.0;
    squash.m11 = 1.0;

    const DrawShape out = TransformShape(DrawShape{circle}, squash);
    const auto* ellipse = std::get_if<DrawEllipse>(&out);
    ASSERT_NE(ellipse, nullptr) << "a squashed circle came back a circle";
    EXPECT_NEAR(ellipse->majorRadius, 20.0, 1e-9);
    EXPECT_NEAR(ellipse->minorRadius, 10.0, 1e-9);

    // ...and a UNIFORM scale leaves it a circle, which is what makes the
    // distinction worth making rather than always producing an ellipse.
    const DrawShape same = TransformShape(DrawShape{circle}, Matrix2D::scaleAbout(Vec2{}, 3.0));
    const auto* stillCircle = std::get_if<DrawCircle>(&same);
    ASSERT_NE(stillCircle, nullptr);
    EXPECT_NEAR(stillCircle->radius, 30.0, 1e-9);
}

TEST(DrawingEntityTest, M33_GEO_005_AMirroredArcStillCoversTheSamePoints) {
    // THE ONE THAT IS EASY TO GET WRONG. Reflection reverses the sweep, so an
    // implementation that rotated both angles by the transform's own rotation
    // would mirror the ends and leave the arc bulging the other way -- and it
    // would look almost right.
    //
    // Checked by MEASURING: a point on the original, mirrored by hand, has to
    // be on the result.
    DrawArc arc;
    arc.centre = Vec2{0.0, 0.0};
    arc.radius = 10.0;
    arc.startAngle = 0.0;
    arc.endAngle = kTwoPi / 4.0; // the first quadrant

    // Mirror about the Y axis. The first quadrant becomes the second.
    const Matrix2D mirror = Matrix2D::mirror(Vec2{0.0, 0.0}, Vec2{0.0, 1.0});
    const DrawShape out = TransformShape(DrawShape{arc}, mirror);
    const auto* mirrored = std::get_if<DrawArc>(&out);
    ASSERT_NE(mirrored, nullptr);

    // The middle of the original is at 45 degrees; mirrored, it is at 135.
    const Vec2 shouldBeOn{10.0 * std::cos(3.0 * kPi / 4.0), 10.0 * std::sin(3.0 * kPi / 4.0)};
    EXPECT_NEAR(DistanceFrom(out, shouldBeOn), 0.0, 1e-6)
        << "the mirrored arc does not cover the mirror of its own middle";
    // ...and it is NOT still covering the first quadrant.
    const Vec2 shouldNotBeOn{10.0 * std::cos(kPi / 4.0), 10.0 * std::sin(kPi / 4.0)};
    EXPECT_GT(DistanceFrom(out, shouldNotBeOn), 1.0)
        << "the mirrored arc still covers where it used to be";
}

TEST(DrawingEntityTest, M33_GEO_006_ABulgedPolylineIsAnArcAndFlattensLikeOne) {
    // A bulge is AutoCAD's arc-in-a-polyline: the tangent of a quarter of the
    // included angle. Kept because it is what DXF stores.
    //
    // A bulge of 1 is a HALF CIRCLE, which is the value with an answer anybody
    // can check: from (0,0) to (10,0) it bulges 5 above the chord.
    DrawPolyline polyline;
    polyline.vertices.push_back(DrawVertex{Vec2{0.0, 0.0}, 1.0});
    polyline.vertices.push_back(DrawVertex{Vec2{10.0, 0.0}, 0.0});

    const std::vector<Vec2> points = FlattenShape(DrawShape{polyline}, 0.01);
    ASSERT_GT(points.size(), 4u) << "a half-circle bulge flattened to a straight line";
    double highest = 0.0;
    for (const Vec2 point : points) highest = std::max(highest, point.y);
    EXPECT_NEAR(highest, 5.0, 0.02) << "a bulge of 1 is a half circle, 5 high over a 10 chord";

    // ...and the ends are still the ends.
    EXPECT_NEAR(Length(points.front(), Vec2{0.0, 0.0}), 0.0, 1e-9);
    EXPECT_NEAR(Length(points.back(), Vec2{10.0, 0.0}), 0.0, 1e-9);
}

TEST(DrawingEntityTest, M33_GEO_007_AMirroredBulgeChangesSIGN) {
    // The bulge encodes which side of the chord the arc lies on, and a
    // reflection puts it on the other -- so moving the vertices alone would
    // leave every arc in a mirrored polyline bulging back the way it came.
    DrawPolyline polyline;
    polyline.vertices.push_back(DrawVertex{Vec2{0.0, 0.0}, 1.0});
    polyline.vertices.push_back(DrawVertex{Vec2{10.0, 0.0}, 0.0});

    // Mirror about the X axis: the bulge that went up must now go down.
    const DrawShape out =
        TransformShape(DrawShape{polyline}, Matrix2D::mirror(Vec2{0.0, 0.0}, Vec2{1.0, 0.0}));
    const std::vector<Vec2> points = FlattenShape(out, 0.01);
    double lowest = 0.0;
    for (const Vec2 point : points) lowest = std::min(lowest, point.y);
    EXPECT_NEAR(lowest, -5.0, 0.02) << "a mirrored polyline still bulges the old way";
}

TEST(DrawingEntityTest, M33_GEO_008_ThreeCollinearPointsHaveNoCircle) {
    // Returning a huge one would let a 3-point arc through three points in a
    // row draw something the user can neither see nor select.
    EXPECT_FALSE(CircleFrom3Points(Vec2{0, 0}, Vec2{1, 0}, Vec2{2, 0}).has_value());
    const auto circle = CircleFrom3Points(Vec2{-10, 0}, Vec2{0, 10}, Vec2{10, 0});
    ASSERT_TRUE(circle.has_value());
    EXPECT_NEAR(circle->radius, 10.0, 1e-9);
    EXPECT_NEAR(circle->centre.y, 0.0, 1e-9);
}

// =============================================================================
// Object snap
// =============================================================================

TEST(DrawingEntityTest, M33_SNAP_001_EndpointBeatsMidpointBeatsNearest) {
    // THE PRIORITY IS THE POINT. Several modes match near a corner, and taking
    // the closest would give a different answer every time the cursor moved a
    // pixel. Predictable beats near, and that is what a drafter trusts.
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}});
    const std::vector<const DrawingEntity*> all = document.entities();

    SnapSettings settings = SnapSettings::all();
    settings.apertureMm = 5.0;

    // Just off the endpoint: END wins, even though NEAREST is closer to the
    // cursor.
    const SnapHit atEnd = SnapTo(all, Vec2{1.0, 1.0}, settings);
    ASSERT_TRUE(atEnd);
    EXPECT_EQ(atEnd.mode, SnapMode::Endpoint);
    EXPECT_NEAR(atEnd.at.x, 0.0, 1e-9);

    // Near the middle, away from either end: MID wins over NEAREST.
    const SnapHit atMid = SnapTo(all, Vec2{51.0, 1.0}, settings);
    ASSERT_TRUE(atMid);
    EXPECT_EQ(atMid.mode, SnapMode::Midpoint);
    EXPECT_NEAR(atMid.at.x, 50.0, 1e-9);

    // Away from both: NEAREST is the fallback that makes the aperture feel
    // continuous.
    const SnapHit atNear = SnapTo(all, Vec2{20.0, 1.0}, settings);
    ASSERT_TRUE(atNear);
    EXPECT_EQ(atNear.mode, SnapMode::Nearest);
    EXPECT_NEAR(atNear.at.y, 0.0, 1e-9);
}

TEST(DrawingEntityTest, M33_SNAP_002_AModeThatIsOffDoesNotWin) {
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}});
    SnapSettings settings = SnapSettings::all();
    settings.apertureMm = 5.0;
    settings.endpoint = false;

    const SnapHit hit = SnapTo(document.entities(), Vec2{1.0, 1.0}, settings);
    ASSERT_TRUE(hit);
    EXPECT_NE(hit.mode, SnapMode::Endpoint) << "a mode that was switched off still won";
}

TEST(DrawingEntityTest, M33_SNAP_003_QuadrantsAreTheCardinalPointsNotTheNearestRim) {
    // A quadrant that moved with the cursor would be useless for the thing
    // quadrants are for, which is measuring a diameter.
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawCircle{Vec2{0.0, 0.0}, 10.0});
    SnapSettings settings = SnapSettings::none();
    settings.quadrant = true;
    settings.apertureMm = 3.0;

    // Near 45 degrees on the rim -- between two quadrants and on neither.
    const double at45 = 10.0 / std::sqrt(2.0);
    EXPECT_FALSE(SnapTo(document.entities(), Vec2{at45, at45}, settings))
        << "a quadrant snapped to a point that is not a quadrant";
    // ...and right of centre it finds the one at 0 degrees.
    const SnapHit east = SnapTo(document.entities(), Vec2{11.0, 0.5}, settings);
    ASSERT_TRUE(east);
    EXPECT_NEAR(east.at.x, 10.0, 1e-9);
    EXPECT_NEAR(east.at.y, 0.0, 1e-9);
}

TEST(DrawingEntityTest, M33_SNAP_004_IntersectionFindsWhereTwoLinesActuallyCross) {
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 100.0}});
    document.addEntity(DrawLine{Vec2{0.0, 100.0}, Vec2{100.0, 0.0}});
    SnapSettings settings = SnapSettings::none();
    settings.intersection = true;
    settings.apertureMm = 5.0;

    const SnapHit hit = SnapTo(document.entities(), Vec2{51.0, 49.0}, settings);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit.mode, SnapMode::Intersection);
    EXPECT_NEAR(hit.at.x, 50.0, 1e-9);
    EXPECT_NEAR(hit.at.y, 50.0, 1e-9);
}

TEST(DrawingEntityTest, M33_SNAP_005_PerpendicularIsFromTheANCHORNotTheCursor) {
    // "Perpendicular to that line" means the line being DRAWN meets it at a
    // right angle, and the line being drawn starts at the anchor. Taking the
    // foot from the cursor instead would give a point that moves as the mouse
    // moves and is perpendicular to nothing.
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}});
    SnapSettings settings = SnapSettings::none();
    settings.perpendicular = true;
    settings.apertureMm = 50.0;

    // Anchor directly above x = 30. The foot is (30, 0) whatever the cursor
    // is doing, so a cursor near (70, 5) must still snap to 30.
    const SnapHit hit =
        SnapTo(document.entities(), Vec2{70.0, 5.0}, settings, Vec2{30.0, 40.0});
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit.mode, SnapMode::Perpendicular);
    EXPECT_NEAR(hit.at.x, 30.0, 1e-9) << "the perpendicular foot followed the cursor";
}

TEST(DrawingEntityTest, M33_SNAP_006_TangentFromInsideACircleHasNoAnswer) {
    // Offering the nearest rim point instead would draw a line that is tangent
    // to nothing, and it would look plausible.
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawCircle{Vec2{0.0, 0.0}, 10.0});
    SnapSettings settings = SnapSettings::none();
    settings.tangent = true;
    settings.apertureMm = 50.0;

    EXPECT_FALSE(SnapTo(document.entities(), Vec2{9.0, 0.0}, settings, Vec2{2.0, 0.0}))
        << "a tangent was offered from inside the circle";
    // ...and from outside there are two, so one of them is found.
    EXPECT_TRUE(SnapTo(document.entities(), Vec2{6.0, 8.0}, settings, Vec2{40.0, 0.0}));
}

// =============================================================================
// The document
// =============================================================================

TEST(DrawingEntityTest, M33_DOC_001_NewGeometryLandsOnTheCurrentLayer) {
    DrawingDocument document{"Sheet"};
    Layer& notes = document.addLayer("Notes", 3);
    ASSERT_TRUE(document.setCurrentLayer(notes.id()));
    const DrawingEntity& line = document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}});
    EXPECT_EQ(line.layerId(), notes.id());
    // ...and its colour resolves THROUGH the layer, which is the whole reason
    // layers exist.
    EXPECT_EQ(line.color(), kColorByLayer);
    EXPECT_EQ(document.resolvedColorOf(line), 3);
    ASSERT_TRUE(document.setLayerColor(notes.id(), 5));
    EXPECT_EQ(document.resolvedColorOf(line), 5)
        << "changing a layer's colour did not change what is drawn on it";
}

TEST(DrawingEntityTest, M33_DOC_002_ALockedLayerIsNeitherMovedNorPicked) {
    // A lock that still let things be picked or moved would be a lock in name
    // only.
    DrawingDocument document{"Sheet"};
    Layer& frame = document.addLayer("Frame", 7);
    ASSERT_TRUE(document.setCurrentLayer(frame.id()));
    const ObjectId line = document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}}).id();
    ASSERT_TRUE(document.setCurrentLayer(document.findLayerNamed(kDefaultLayerName)->id()));
    ASSERT_TRUE(document.setLayerLocked(frame.id(), true));

    EXPECT_TRUE(document.entitiesNear(Vec2{5.0, 0.0}, 1.0).empty())
        << "something on a locked layer was picked";
    EXPECT_FALSE(document.transformEntities({line}, Matrix2D::translation(Vec2{100.0, 0.0})))
        << "something on a locked layer was moved";
    const auto* still = std::get_if<DrawLine>(&document.findEntity(line)->shape());
    ASSERT_NE(still, nullptr);
    EXPECT_NEAR(still->a.x, 0.0, 1e-9);
}

TEST(DrawingEntityTest, M33_DOC_003_MovingASelectionIsONEUndoStep) {
    // Moving forty lines is one thing the user did. Undoing it a line at a
    // time would stop somewhere no drawing was ever in.
    DrawingDocument document{"Sheet"};
    std::vector<ObjectId> ids;
    for (int i = 0; i < 4; ++i)
        ids.push_back(document.addEntity(DrawLine{Vec2{0.0, i * 10.0},
                                                  Vec2{10.0, i * 10.0}}).id());
    const std::size_t before = document.undoDepth();
    ASSERT_TRUE(document.transformEntities(ids, Matrix2D::translation(Vec2{50.0, 0.0})));
    EXPECT_EQ(document.undoDepth(), before + 1);

    ASSERT_TRUE(document.undo());
    for (const ObjectId id : ids) {
        const auto* line = std::get_if<DrawLine>(&document.findEntity(id)->shape());
        ASSERT_NE(line, nullptr);
        EXPECT_NEAR(line->a.x, 0.0, 1e-9) << "only some of the selection came back";
    }
}

TEST(DrawingEntityTest, M33_DOC_004_ACopyKeepsTheORIGINALSLayerNotTheCurrentOne) {
    // A copy that landed on the current layer would be a copy that looks
    // different from what was copied.
    DrawingDocument document{"Sheet"};
    Layer& frame = document.addLayer("Frame", 2);
    ASSERT_TRUE(document.setCurrentLayer(frame.id()));
    const ObjectId source = document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}}).id();
    ASSERT_TRUE(document.setCurrentLayer(document.findLayerNamed(kDefaultLayerName)->id()));

    const std::vector<ObjectId> made =
        document.copyEntities({source}, Matrix2D::translation(Vec2{0.0, 20.0}));
    ASSERT_EQ(made.size(), 1u);
    EXPECT_EQ(document.findEntity(made.front())->layerId(), frame.id())
        << "the copy landed on the current layer instead of the original's";
    const auto* line = std::get_if<DrawLine>(&document.findEntity(made.front())->shape());
    ASSERT_NE(line, nullptr);
    EXPECT_NEAR(line->a.y, 20.0, 1e-9);
}

TEST(DrawingEntityTest, M33_DOC_005_WindowAndCrossingAreDifferentRules) {
    // A package with only one of them surprises everybody who has met the
    // other.
    DrawingDocument document{"Sheet"};
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 0.0}});   // pokes out
    document.addEntity(DrawLine{Vec2{10.0, 10.0}, Vec2{20.0, 20.0}}); // fully inside

    Box2D window;
    window.grow(Vec2{-5.0, -5.0});
    window.grow(Vec2{50.0, 50.0});

    EXPECT_EQ(document.entitiesInWindow(window, /*crossing=*/false).size(), 1u)
        << "a window selection caught something that pokes out of it";
    EXPECT_EQ(document.entitiesInWindow(window, /*crossing=*/true).size(), 2u)
        << "a crossing selection missed something it touches";
}

TEST(DrawingEntityTest, M33_DOC_006_ALayerWithGeometryOnItCannotBeDeleted) {
    // AutoCAD refuses this too. The alternative -- deleting the geometry with
    // it -- would throw away work in answer to a command about a table entry.
    DrawingDocument document{"Sheet"};
    Layer& notes = document.addLayer("Notes");
    ASSERT_TRUE(document.setCurrentLayer(notes.id()));
    const ObjectId line = document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}}).id();
    ASSERT_TRUE(document.setCurrentLayer(document.findLayerNamed(kDefaultLayerName)->id()));

    EXPECT_FALSE(document.removeObject(notes.id())) << "a layer with geometry on it was deleted";
    ASSERT_TRUE(document.removeObject(line));
    EXPECT_TRUE(document.removeObject(notes.id())) << "an empty layer could not be deleted";
}

// =============================================================================
// The file
// =============================================================================

TEST(DrawingEntityTest, M33_SER_001_EveryShapeSurvivesASaveAndAReopen) {
    DrawingDocument document{"Sheet"};
    document.addLinetype("HIDDEN", "Hidden line", std::vector<double>{5.0, -2.5});
    Layer& notes = document.addLayer("Notes", 3, "HIDDEN");
    ASSERT_TRUE(document.setCurrentLayer(notes.id()));

    document.addEntity(DrawPoint{Vec2{1.0, 2.0}});
    document.addEntity(DrawLine{Vec2{0.0, 0.0}, Vec2{100.0, 50.0}});
    document.addEntity(DrawCircle{Vec2{20.0, 20.0}, 8.0});
    DrawArc arc;
    arc.centre = Vec2{40.0, 40.0};
    arc.radius = 12.0;
    arc.startAngle = 0.25;
    arc.endAngle = 2.0;
    document.addEntity(arc);
    DrawEllipse ellipse;
    ellipse.centre = Vec2{60.0, 60.0};
    ellipse.majorRadius = 20.0;
    ellipse.minorRadius = 8.0;
    ellipse.rotation = 0.4;
    document.addEntity(ellipse);
    DrawPolyline polyline;
    polyline.vertices.push_back(DrawVertex{Vec2{0.0, 0.0}, 0.5});
    polyline.vertices.push_back(DrawVertex{Vec2{10.0, 0.0}, 0.0});
    polyline.vertices.push_back(DrawVertex{Vec2{10.0, 10.0}, 0.0});
    polyline.closed = true;
    document.addEntity(polyline);
    DrawText text;
    text.at = Vec2{5.0, 90.0};
    text.text = "SECTION A-A";
    text.heightMm = 5.0;
    text.rotation = 0.0;
    document.addEntity(text);

    const std::string saved = SaveToString(document);
    const DrawingLoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    const DrawingDocument& back = *loaded.document;

    ASSERT_EQ(back.entities().size(), 7u) << "not every shape came back";
    // BYTE FOR BYTE on a second save, which is the property that catches a
    // field written and not read.
    EXPECT_EQ(SaveToString(back), saved);
    EXPECT_EQ(back.undoDepth(), 0u);

    // ...and the BULGE survived, which is the field a lossy round trip loses
    // first because it looks like a rounding artefact.
    for (const DrawingEntity* entity : back.entities()) {
        const auto* line = std::get_if<DrawPolyline>(&entity->shape());
        if (line == nullptr) continue;
        ASSERT_EQ(line->vertices.size(), 3u);
        EXPECT_NEAR(line->vertices.front().bulge, 0.5, 1e-12);
        EXPECT_TRUE(line->closed);
    }
}

TEST(DrawingEntityTest, M33_SER_002_AnEntityOnALayerThatIsGoneIsREFUSED) {
    DrawingDocument document{"Sheet"};
    Layer& notes = document.addLayer("Notes");
    ASSERT_TRUE(document.setCurrentLayer(notes.id()));
    document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}});

    std::string text = SaveToString(document);
    const std::string real = "\"layerId\": \"" + std::to_string(notes.id()) + "\"";
    const std::size_t at = text.find(real);
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, real.size(), "\"layerId\": \"777333\"");

    const DrawingLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
    EXPECT_NE(loaded.message.find("777333"), std::string::npos) << loaded.message;
}

TEST(DrawingEntityTest, M33_SER_003_AnEllipseWhoseMajorAxisIsShorterIsREFUSED) {
    // A file that says otherwise would draw an ellipse turned ninety degrees
    // from the one it describes -- and it would draw it without complaint.
    DrawingDocument document{"Sheet"};
    DrawEllipse ellipse;
    ellipse.centre = Vec2{0.0, 0.0};
    ellipse.majorRadius = 20.0;
    ellipse.minorRadius = 8.0;
    document.addEntity(ellipse);

    std::string text = SaveToString(document);
    const std::size_t at = text.find("\"majorRadius\": 20");
    ASSERT_NE(at, std::string::npos) << text;
    text.replace(at, std::string("\"majorRadius\": 20").size(), "\"majorRadius\": 2");

    const DrawingLoadResult loaded = LoadFromString(text);
    EXPECT_FALSE(loaded) << "an ellipse with a short major axis was accepted";
}

TEST(DrawingEntityTest, M33_UNDO_001_DrawingErasingAndRecolouringAllComeBack) {
    DrawingDocument document{"Sheet"};
    const ObjectId line = document.addEntity(DrawLine{Vec2{0, 0}, Vec2{10, 0}}).id();
    ASSERT_TRUE(document.setEntityColor(line, 1));
    ASSERT_TRUE(document.removeObject(line));
    EXPECT_EQ(document.findEntity(line), nullptr);

    ASSERT_TRUE(document.undo()); // the erase
    ASSERT_NE(document.findEntity(line), nullptr);
    EXPECT_EQ(document.findEntity(line)->color(), 1);
    ASSERT_TRUE(document.undo()); // the colour
    EXPECT_EQ(document.findEntity(line)->color(), kColorByLayer);
    ASSERT_TRUE(document.undo()); // the draw
    EXPECT_EQ(document.findEntity(line), nullptr);

    while (document.canRedo()) ASSERT_TRUE(document.redo());
    EXPECT_EQ(document.findEntity(line), nullptr) << "redoing an erase brought it back";
}
