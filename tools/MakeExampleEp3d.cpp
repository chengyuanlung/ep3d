// Writes examples/spline-and-ellipse.ep3d.
//
// Built through the ORDINARY document API rather than by writing JSON, so the
// file is one EP3D can certainly load and one whose constraints are the same
// ones the commands create. A hand-written example is a second definition of
// the format that starts out agreeing and stops.
//
// What it demonstrates is in the ADRs it prints: every constraint an ellipse
// and a spline can currently carry, and -- just as important -- the degrees of
// freedom each one has left, with the reason.

#include "Core/Body/Body.h"
#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Recompute/RecomputeTypes.h"
#include "Core/Sketch/Sketch.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace paramcad;

namespace {

constexpr double kPi = 3.14159265358979323846;

void Report(const Sketch& sketch, const char* what) {
    std::printf("  %-22s DOF %2d   %s\n", what, sketch.degreesOfFreedom(),
                sketch.solveMessage().empty() ? SolveStatusName(sketch.solveStatus())
                                              : sketch.solveMessage().c_str());
}

} // namespace

int main() {
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    PartDocument document{"SplineAndEllipse"};
    document.setGeometryKernel(&kernel);
    document.setSketchSolver(&solver);

    // =====================================================================
    // Sketch 1 -- AN ELLIPSE, FULLY CONSTRAINED
    //
    // An ellipse is five numbers: centre (2), major, minor, rotation. So it
    // takes five equations, and EP3D has exactly one command for each.
    // =====================================================================
    Sketch& ring = document.addSketch("EllipseRing");

    Parameter& a = document.addParameter("MajorA", 40.0, UnitType::Millimeter);
    Parameter& b = document.addParameter("MinorB", 15.0, UnitType::Millimeter);
    Parameter& turn = document.addParameter("EllipseAngle", 30.0 * kPi / 180.0,
                                            UnitType::Radian);
    Parameter& hole = document.addParameter("HoleR", 8.0, UnitType::Millimeter);

    const SketchEntityId ellipse = ring.addEllipse(Vec2{0, 0}, 40.0, 15.0, 30.0 * kPi / 180.0);

    // 1-2. Its CENTRE. Fix pins both coordinates at once.
    document.addSketchConstraint(
        ring.id(), FixConstraint{SketchElementRef{ellipse, SketchSubElement::CenterPoint}});
    // 3. The LONG semi-axis. Radius and Diameter are REFUSED on an ellipse --
    //    it has two radii, and a command that picked one would drive it without
    //    saying which.
    document.addSketchConstraint(ring.id(), EllipseAxisConstraint{ellipse, a.id(), false});
    // 4. The SHORT one.
    document.addSketchConstraint(ring.id(), EllipseAxisConstraint{ellipse, b.id(), true});
    // 5. WHERE THE LONG AXIS POINTS. Without this an ellipse can always spin,
    //    and the sketch can never read 0.
    document.addSketchConstraint(ring.id(), EllipseRotationConstraint{ellipse, turn.id()});

    // A circle CONCENTRIC with it, which an ellipse can be: sharing a centre
    // says nothing about how many radii either side has.
    const SketchEntityId bore = ring.addCircle(Vec2{0, 0}, 8.0);
    document.addSketchConstraint(ring.id(), ConcentricConstraint{ellipse, bore});
    document.addSketchConstraint(ring.id(), RadiusConstraint{bore, hole.id()});

    // =====================================================================
    // Sketch 2 -- A FIVE-POINT SPLINE
    //
    // Ten numbers, and only FOUR of them can be constrained: a
    // SketchElementRef names a sub-element, there are four sub-elements, and
    // an interior point has no name. The three points in the middle stay free,
    // and the DOF readout says 6 rather than pretending otherwise.
    // =====================================================================
    Sketch& blade = document.addSketch("SplineBlade");

    Parameter& span = document.addParameter("BladeSpan", 120.0, UnitType::Millimeter);

    const SketchEntityId spline = blade.addSpline(
        {Vec2{0, 0}, Vec2{30, 25}, Vec2{60, -10}, Vec2{90, 20}, Vec2{120, 0}}, false);
    // The straight edge that closes the profile, so the sketch can be padded.
    const SketchEntityId chord = blade.addLine(Vec2{120, 0}, Vec2{0, 0});

    // The chord, pinned outright: start fixed, horizontal, and a length.
    document.addSketchConstraint(
        blade.id(), FixConstraint{SketchElementRef{chord, SketchSubElement::EndPoint}});
    document.addSketchConstraint(blade.id(), HorizontalConstraint{chord});
    document.addSketchConstraint(blade.id(), LengthConstraint{chord, span.id()});

    // ...and the spline's TWO ENDS joined to it. These are ordinary Coincidents
    // on ordinary variables -- the ends are what a profile chains through, and
    // they are exactly the part of a spline a constraint can reach.
    document.addSketchConstraint(
        blade.id(),
        CoincidentConstraint{SketchElementRef{spline, SketchSubElement::StartPoint},
                             SketchElementRef{chord, SketchSubElement::EndPoint}});
    document.addSketchConstraint(
        blade.id(),
        CoincidentConstraint{SketchElementRef{spline, SketchSubElement::EndPoint},
                             SketchElementRef{chord, SketchSubElement::StartPoint}});

    // =====================================================================
    // Two solids, so the file opens as something to look at.
    // =====================================================================
    Parameter& thick = document.addParameter("Thickness", 10.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    document.addPadFeature(body, "RingPad", ring.id(), thick.id());
    Body& second = document.addBody("Body002");
    document.addPadFeature(second, "BladePad", blade.id(), thick.id());

    const DocumentRecomputeReport report = document.recompute();
    std::printf("recompute: %s\n", report.success ? "OK" : "FAILED");
    Report(ring, "EllipseRing");
    Report(blade, "SplineBlade");

    const SaveResult saved =
        savePartDocumentToFile(document, "examples/spline-and-ellipse.ep3d");
    std::printf("save: %s %s\n", saved ? "OK" : "FAILED", saved.message.c_str());

    // Read it straight back, because a file that cannot be reopened is not an
    // example of anything.
    const LoadResult loaded = loadPartDocumentFromFile("examples/spline-and-ellipse.ep3d");
    std::printf("reload: %s %s\n", loaded ? "OK" : "FAILED", loaded.message.c_str());
    if (!loaded) return 1;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);
    const DocumentRecomputeReport again = loaded.document->recompute();
    std::printf("reloaded recompute: %s\n", again.success ? "OK" : "FAILED");
    for (const Sketch* s : loaded.document->sketches()) Report(*s, s->name().c_str());
    return (report.success && saved && again.success) ? 0 : 1;
}
