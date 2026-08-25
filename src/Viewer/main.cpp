// Minimal M4 viewer application (spec 17): displays the solid produced by a
// Sketch + Pad, with rotate, pan, zoom, fit-all, whole-object selection and
// refresh-after-recompute.
//
// The document and kernel are owned HERE, not by any Qt object (ADR-M4-006):
// Qt presentation objects never own the semantic CAD model.

#include "Core/Document/PartDocument.h"
#include "Core/Physics/MassProperties.h"
#include "Core/Reconstruction/SketchReconstructor.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Reference/ReferenceFrame.h"
#include "Core/Connector/Connector.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/RevolveFeature.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include "Viewer/DocumentOutline.h"
#include "Viewer/DrawingPlot.h"
#include "Viewer/DocumentPresenter.h"
#include "Viewer/MainWindow.h"
#include "Viewer/ScriptServer.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QMessageBox>
#include <QTcpSocket>
#include "Viewer/SketchCanvasWidget.h"
#include "Viewer/OcctViewWidget.h"

#include <QApplication>
#include <QFile>
#include <QDir>
#include <QColor>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QPalette>
#include <QString>
#include <QTimer>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <string>

using namespace paramcad;

namespace {

// Deterministic validation samples (M5 UI guide 2: a generator is acceptable in
// place of committed sample files, provided anyone can reproduce them without
// relying on the developer modelling by hand).
//
// These are M4-shaped. The guide's samples B and C assume under-constrained and
// conflicting states, which are M5 concepts and do not exist yet; its Test A
// edits sketch Width, which M4's UI does not expose (only the Pad Length
// Parameter is editable). Each substitute below exercises the same property the
// original was checking, using something M4 can actually do.
enum class Sample {
    Rectangle,      // A: 100 x 50 padded 20, aluminium
    FailedProfile,  // B: the same rectangle with one side missing
    CircleR10,      // D1: r = 10 padded 30
    CircleR20,      // D2: r = 20 padded 30 -- volume must be 4x D1
    // M5 samples. The M4 ones above stay exactly as they were: they are the
    // evidence that a constraint-free document still behaves identically, and
    // rewriting them to use constraints would destroy that evidence.
    M5Rectangle,      // Gate A: fully constrained rectangle, DOF = 0
    M5UnderConstrained, // Gate C: the same rectangle missing its dimensions
    M5Conflict,       // Gate E: two disagreeing Lengths on one line
    M5Circle,         // Gate F: fixed-centre circle driven by a Radius Parameter
    // M8 (spec 27-equivalent for the chain): the feature history visible and
    // editable in the running shell. Creation DIALOGS are deferred to M9 with
    // the edit-transaction work (ADR-M8-007); what M8 ships is the chain
    // reachable, displayed as its tail, and driven by panel edits.
    M8Chain,          // Pad 100x50x20 minus Pocket 20x30x10 = 94000 mm^3
    // M8.2 and M8.3 shipped Revolve, Fillet and Chamfer -- three of the four
    // features M8 spec 4 REQUIRES -- with no way to reach any of them in the
    // running application. Owner UI validation could therefore cover a quarter
    // of the milestone. These two samples close that: the same chain machinery,
    // the same panel, three more feature kinds.
    M8Revolve,        // Annulus: profile x in [10,30], v in [0,50] about x=0
    M8Dress,          // Pad 100x50x20 with every edge filleted r=2
    // M10: the same 100x50x20 pad, but SUPPORTED BY A FRAME that is offset and
    // rotated. Volume is identical to m8-chain's pad -- the point is WHERE the
    // solid is, which is why the selftest checks the centre of mass and the
    // owner checklist asks what is on screen.
    M10Frame,
    // M11.3: a parameter DRIVEN BY AN EXPRESSION, reachable in the running
    // shell. The pad length is `#PadBase / 2`, so the solid on screen is
    // evidence the expression was evaluated -- and the panel has an Expression
    // row a user can actually type into. Without this sample every part of
    // M11 was unreachable from the application, which is the exact position
    // M8.2/M8.3 were in before m8-revolve and m8-dress existed.
    M11Expression,
    // M12: the same M4 rectangle and pad, present ONLY so the selftest has a
    // document to open a NEW sketch inside. The sample's own geometry is not
    // what it proves -- the drawing, dimensioning and constraining the
    // selftest then performs through the shell is.
    //
    // It exists for the reason m8-revolve and m11-expression exist: a
    // milestone whose UI is unreachable from the running application can only
    // ever be owner-validated in the quarter of it that has a sample.
    M12Sketch
};

bool IsM5(Sample sample) noexcept {
    return sample == Sample::M5Rectangle || sample == Sample::M5UnderConstrained ||
           sample == Sample::M5Conflict || sample == Sample::M5Circle;
}

struct DemoModel {
    PartDocument document{"ViewerDemo"};
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    Parameter* padLength = nullptr;
    Parameter* width = nullptr;   // M5 samples only
    Parameter* height = nullptr;  // M5 samples only
    PadFeature* pad = nullptr;
    // The sample's base-capable solid -- the pad for every sample that has one,
    // the revolve for the one that does not. Selection, the panel-fit sweep and
    // the `empty`/`pad-selected` scenarios all need SOME feature to name, and
    // naming `pad` unconditionally would dereference null the moment a sample
    // ships without one.
    Feature* baseSolid = nullptr;
    ObjectId sketchId = kInvalidObjectId;

    explicit DemoModel(Sample sample) {
        document.setGeometryKernel(&kernel);
        // The solver is constructed HERE and injected, exactly like the kernel
        // (ADR-M3-003 / ADR-M5-003): the viewer is an application, so it is
        // allowed to name a concrete backend. Core never does.
        document.setSketchSolver(&solver);
        document.addMaterial("Aluminium", 2700.0);

        const bool circle = sample == Sample::CircleR10 || sample == Sample::CircleR20;
        padLength = &document.addParameter("PadLength", circle ? 30.0 : 20.0,
                                           UnitType::Millimeter);
        Sketch& sketch = document.addSketch("Sketch001");
        sketchId = sketch.id();

        // M8.2 in the shell. The profile and axis are the release gate's own
        // fixture (GATE_RB): a 20 x 50 rectangle at x in [10,30] revolved a
        // full turn about the sketch line at x = 0, giving the annulus
        // pi*(30^2 - 10^2)*50 = 40000*pi. The v extent is 50 and not 40 for the
        // reason GATE_RB records -- at 40 the correct annulus and the volume an
        // axis-resolved-by-position bug produces are equal by coincidence.
        if (sample == Sample::M8Revolve) {
            // The axis is added FIRST here and the gate proves order does not
            // matter; what the sample must show is that the axis line is
            // construction geometry, not a profile edge.
            const SketchEntityId axis = sketch.addLine(Vec2{0, -5}, Vec2{0, 55});
            sketch.addLine(Vec2{10, 0}, Vec2{30, 0});
            sketch.addLine(Vec2{30, 0}, Vec2{30, 50});
            sketch.addLine(Vec2{30, 50}, Vec2{10, 50});
            sketch.addLine(Vec2{10, 50}, Vec2{10, 0});
            Parameter& angle =
                document.addParameter("RevolveAngle", 2.0 * 3.14159265358979323846,
                                      UnitType::Radian);
            Body& body = document.addBody("Body001");
            baseSolid = &document.addRevolveFeature(body, "Revolve001", sketch.id(), axis,
                                                   angle.id());
            return;
        }

        // M8.3 in the shell: the dress chain Sketch -> Pad -> Fillet. Every
        // edge of the 100 x 50 x 20 pad rounded at r = 2, which is the
        // Minkowski oracle GATE_FB checks:
        //   96*46*16 + 2*2*(96*46 + 46*16 + 96*16) + pi*4*(96+46+16) + (4/3)*pi*8
        if (sample == Sample::M8Dress) {
            buildConstrainedRectangle(sketch, Sample::M5Rectangle);
            Parameter& radius = document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
            Body& body = document.addBody("Body001");
            pad = &document.addPadFeature(body, "Pad001", sketch.id(), padLength->id());
            baseSolid = pad;
            document.addFilletFeature(body, "Fillet001", pad->id(), radius.id());
            return;
        }

        // M10 in the shell: a frame hierarchy carrying a sketch. The root is
        // lifted +30 in Z and the child is rotated 90 degrees about X, so the
        // pad lands somewhere only the frame chain can put it -- the volume
        // stays 100000 and the centre of mass is the discriminator.
        if (sample == Sample::M10Frame) {
            ReferenceFrame& root = document.addFrame("Root");
            ReferenceFrame& plate = document.addFrame("PlateFrame", root.id());
            Transform3D lifted;
            lifted.translation = Vec3{0.0, 0.0, 30.0};
            document.setFrameTransform(root.id(), lifted);
            Transform3D turned;
            turned.rotation = Quaternion{std::cos(0.25 * 3.14159265358979323846),
                                         std::sin(0.25 * 3.14159265358979323846), 0.0, 0.0};
            document.setFrameTransform(plate.id(), turned);
            document.addConnector("MountPoint", ConnectorRole::Mount, plate.id());

            sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
            sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
            sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
            sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
            document.setSketchSupportFrame(sketch.id(), plate.id());

            Body& body = document.addBody("Body001");
            pad = &document.addPadFeature(body, "Pad001", sketch.id(), padLength->id());
            baseSolid = pad;
            return;
        }

        if (sample == Sample::M11Expression) {
            // PadBase = 40, PadLength = #PadBase / 2 = 20, on the 100 x 50
            // rectangle -> 100000 mm^3, the same oracle every other sample
            // uses. The number is reached by EVALUATION, not by assignment:
            // padLength is created at 999 so that seeing 100000 proves the
            // expression ran, exactly as the M5 samples draw off-size geometry
            // so that seeing 100 x 50 proves the solver ran.
            document.setParameterValue(padLength->id(), 999.0);
            document.addParameter("PadBase", 40.0, UnitType::Millimeter);
            document.setParameterExpression(padLength->id(), "#PadBase / 2");
            buildConstrainedRectangle(sketch, Sample::M5Rectangle);
            Body& body = document.addBody("Body001");
            pad = &document.addPadFeature(body, "Pad001", sketch.id(), padLength->id());
            baseSolid = pad;
            return;
        }
        if (sample == Sample::M8Chain) {
            buildConstrainedRectangle(sketch, Sample::M5Rectangle);
            Sketch& pocketSketch = document.addSketch("PocketSketch");
            pocketSketch.addLine(Vec2{10, 10}, Vec2{30, 10});
            pocketSketch.addLine(Vec2{30, 10}, Vec2{30, 40});
            pocketSketch.addLine(Vec2{30, 40}, Vec2{10, 40});
            pocketSketch.addLine(Vec2{10, 40}, Vec2{10, 10});
            Parameter& depth = document.addParameter("PocketDepth", 10.0, UnitType::Millimeter);
            Body& body = document.addBody("Body001");
            pad = &document.addPadFeature(body, "Pad001", sketch.id(), padLength->id());
            baseSolid = pad;
            document.addPocketFeature(body, "Pocket001", pad->id(), pocketSketch.id(),
                                      depth.id());
            return;
        }
        if (sample == Sample::M5Circle) {
            buildConstrainedCircle(sketch);
        } else if (IsM5(sample)) {
            buildConstrainedRectangle(sketch, sample);
        } else if (circle) {
            sketch.addCircle(Vec2{0, 0}, sample == Sample::CircleR10 ? 10.0 : 20.0);
        } else {
            sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
            sketch.addLine(Vec2{100, 0}, Vec2{100, 50});
            if (sample != Sample::FailedProfile)
                sketch.addLine(Vec2{100, 50}, Vec2{0, 50});
            sketch.addLine(Vec2{0, 50}, Vec2{0, 0});
        }

        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", sketch.id(), padLength->id());
        baseSolid = pad;
    }

private:
    // Spec 15's reference circle: fixed centre, radius driven by a Parameter.
    // Drawn at 7 mm so that seeing 20 mm on screen is evidence the solver ran.
    void buildConstrainedCircle(Sketch& sketch) {
        width = &document.addParameter("Radius", 20.0, UnitType::Millimeter);
        const SketchEntityId circle = sketch.addCircle(Vec2{0, 0}, 7.0);
        document.addSketchConstraint(
            sketch.id(),
            FixConstraint{SketchElementRef{circle, SketchSubElement::CenterPoint}});
        document.addSketchConstraint(sketch.id(), RadiusConstraint{circle, width->id()});
    }

    // Spec 14's reference rectangle. Drawn deliberately OFF-SIZE and skewed so
    // that seeing 100 x 50 on screen is evidence the solver ran, not evidence
    // that the geometry was typed in already correct.
    void buildConstrainedRectangle(Sketch& sketch, Sample sample) {
        width = &document.addParameter("Width", 100.0, UnitType::Millimeter);
        height = &document.addParameter("Height", 50.0, UnitType::Millimeter);

        const SketchEntityId bottom = sketch.addLine(Vec2{0, 0}, Vec2{112, 3});
        const SketchEntityId right = sketch.addLine(Vec2{112, 3}, Vec2{115, 58});
        const SketchEntityId top = sketch.addLine(Vec2{115, 58}, Vec2{2, 61});
        const SketchEntityId left = sketch.addLine(Vec2{2, 61}, Vec2{0, 0});

        const auto sp = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::StartPoint};
        };
        const auto ep = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::EndPoint};
        };
        const auto add = [&](SketchConstraintData data) {
            document.addSketchConstraint(sketch.id(), std::move(data));
        };

        add(CoincidentConstraint{ep(bottom), sp(right)});
        add(CoincidentConstraint{ep(right), sp(top)});
        add(CoincidentConstraint{ep(top), sp(left)});
        add(CoincidentConstraint{ep(left), sp(bottom)});
        add(HorizontalConstraint{bottom});
        add(HorizontalConstraint{top});
        add(VerticalConstraint{right});
        add(VerticalConstraint{left});
        add(FixConstraint{sp(bottom)});

        // Under-constrained stops here: the shape is a rectangle but no
        // dimension pins its size, so DOF > 0 and the UI must SAY so rather
        // than presenting it as finished work.
        if (sample == Sample::M5UnderConstrained) return;

        add(LengthConstraint{bottom, width->id()});
        add(LengthConstraint{right, height->id()});

        if (sample != Sample::M5Conflict) return;
        // A second, disagreeing length on the same line: the textbook conflict.
        Parameter& other = document.addParameter("WidthAlt", 70.0, UnitType::Millimeter);
        add(LengthConstraint{bottom, other.id()});
    }
};

bool gUnknownSample = false;

// What the imported file SHOULD produce, when the caller says. -1 means "no
// expectation", so the existing dimensionless fixture keeps working unchanged.
int gExpectFromSource = -1;
int gExpectSkipped = -1;

Sample SampleFromName(const char* name) {
    if (name == nullptr) return Sample::Rectangle;
    if (std::strcmp(name, "m4-failed-profile") == 0) return Sample::FailedProfile;
    if (std::strcmp(name, "m4-circle-r10") == 0) return Sample::CircleR10;
    if (std::strcmp(name, "m4-circle-r20") == 0) return Sample::CircleR20;
    if (std::strcmp(name, "m5-rectangle") == 0) return Sample::M5Rectangle;
    if (std::strcmp(name, "m5-underconstrained") == 0) return Sample::M5UnderConstrained;
    if (std::strcmp(name, "m5-conflict") == 0) return Sample::M5Conflict;
    if (std::strcmp(name, "m5-circle") == 0) return Sample::M5Circle;
    if (std::strcmp(name, "m8-chain") == 0) return Sample::M8Chain;
    if (std::strcmp(name, "m8-revolve") == 0) return Sample::M8Revolve;
    if (std::strcmp(name, "m8-dress") == 0) return Sample::M8Dress;
    if (std::strcmp(name, "m10-frame") == 0) return Sample::M10Frame;
    if (std::strcmp(name, "m11-expression") == 0) return Sample::M11Expression;
    if (std::strcmp(name, "m12-sketch") == 0) return Sample::M12Sketch;
    // An unknown name is an ERROR, not a fallback. Falling back to the M4
    // rectangle and still printing SELFTEST OK meant a typo in CI silently
    // downgraded an M5 gate to an M4 smoke test that passes.
    gUnknownSample = true;
    return Sample::Rectangle;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);

    // --selftest: build the real window, let it lay out and paint, assert the
    // things a user would notice immediately, then exit.
    //
    // This exists because four user-facing defects -- a missing Qt platform
    // plugin that stopped the binary launching at all, OCCT painting over every
    // Qt control, wireframe instead of shaded, and a resize that left stale
    // pixels -- survived two full review rounds while 302 tests passed, purely
    // because nothing ever STARTED the executable. Unit tests cannot see any of
    // them; a build cannot either. Starting the program is its own check, and
    // it now runs in CTest.
    const bool selfTest = argc > 1 && std::strcmp(argv[1], "--selftest") == 0;

    // A SCREENSHOT HAS TO SHOW WHAT IS ON THE SCREEN.
    //
    // The self test never enters the event loop, so a label whose text changed
    // has been marked dirty and not repainted -- and grab() then composites the
    // stale paint UNDER the new one. Three superimposed status lines is what
    // that looks like, and it made every status bar in the golden set
    // unreadable while every assertion passed.
    //
    // Draining the posted events and forcing a repaint before the grab is the
    // fix, in ONE place: five call sites each remembering to do it is the
    // shape of defect this project keeps finding.
    const auto shoot = [](QWidget& window, const QString& path) {
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        window.repaint();
        return window.grab().save(path);
    };

    // --scenario <name>: drive the shell into a named state so the golden
    // screenshot set (UI spec 16) captures real application states rather than
    // mock-ups. Each name matches a UI-0xx entry in the self-validation report.
    const char* scenario = nullptr;
    const char* sampleName = nullptr;
    // Where to write a PNG of the sketch canvas, for looking at what was drawn.
    // Dimension rendering is the one part of this UI whose correctness is a
    // JUDGEMENT -- geometry tests can say an arrowhead exists and points the
    // right way, and still not tell anyone whether the result reads as a
    // drawing.
    const char* screenshotPath = nullptr;
    // Whether the picture was actually TAKEN. --screenshot was honoured inside
    // exactly one sample's branch (`--sample=m12-sketch`) and silently ignored
    // in every other, so asking for a picture of the model toolbar printed
    // SELFTEST OK and wrote no file. Silently discarding a flag the program
    // understands is the defect --sample was fixed for three separate times
    // (ADR-M5-025, ADR-M6-025); this is the same defect, one flag along.
    bool screenshotWritten = false;
    const char* importPath = nullptr;
    // THE SCRIPT SOCKET IS ON BY DEFAULT (M17.28).
    //
    // It was a flag first, and that was wrong twice over: a GUI application has
    // no console, so a bind failure closed the window with the reason written
    // to a stream nobody could see -- and even when it worked, the only way to
    // know was to have typed the flag. A feature you must know about in advance
    // and cannot tell is running is a feature nobody uses.
    //
    // What makes on-by-default defensible is that it is LOOPBACK ONLY and never
    // invisible: the title bar carries the port for as long as it is open, and
    // --no-listen turns it off. It is still a local process that can drive this
    // one into saving files, which is why the title says so.
    //
    // `--listen PORT` asks for a particular number; 0 means "any free port".
    bool listenDisabled = false;
    int listenPort = 5310;
    // Accepts BOTH `--flag value` and `--flag=value`.
    //
    // Matching only the separate-token form meant `--sample=m5-circle` failed
    // strcmp, the flag was dropped entirely, and the M4 rectangle was built and
    // passed with SELFTEST OK -- the third appearance of the same CI downgrade,
    // after an unknown name and a missing value, and the most commonly typed of
    // the three. A flag the program does not understand must never be silently
    // discarded.
    const auto valueFor = [&](int i, const char* flag, bool& present) -> const char* {
        present = false;
        const std::size_t length = std::strlen(flag);
        if (std::strncmp(argv[i], flag, length) != 0) return nullptr;
        present = true;
        if (argv[i][length] == '=') return argv[i] + length + 1; // --flag=value
        if (argv[i][length] != '\0') { present = false; return nullptr; } // --flagXYZ
        return (i + 1 < argc) ? argv[i + 1] : nullptr;                     // --flag value
    };
    for (int i = 1; i < argc; ++i) {
        bool present = false;
        if (const char* value = valueFor(i, "--scenario", present); present && value != nullptr)
            scenario = value;
        if (const char* value = valueFor(i, "--import", present); present) {
            // An --import with no path is an error, not a silent no-op: the
            // same rule --sample learned three times over (ADR-M6-025's
            // sibling, ADR-M5-025).
            if (value == nullptr || value[0] == '\0') gUnknownSample = true;
            else importPath = value;
        }
        if (const char* value = valueFor(i, "--screenshot", present); present) {
            if (value == nullptr || *value == '\0') {
                std::fprintf(stderr, "--screenshot needs a file path\n");
                return 2;
            }
            screenshotPath = value;
        }
        if (std::strcmp(argv[i], "--no-listen") == 0) listenDisabled = true;
        if (const char* value = valueFor(i, "--listen", present); present) {
            // The PORT is optional: `--listen` alone takes the default. A
            // following token that is not a number belongs to the next flag.
            if (value != nullptr && value[0] != '\0') {
                char* end = nullptr;
                const long parsed = std::strtol(value, &end, 10);
                if (end != nullptr && *end == '\0' && parsed >= 0 && parsed <= 65535)
                    listenPort = static_cast<int>(parsed);
            }
        }
        if (const char* value = valueFor(i, "--sample", present); present) {
            // An EMPTY or MISSING value is an error, not a silent default.
            if (value == nullptr || value[0] == '\0') gUnknownSample = true;
            else sampleName = value;
        }
        // The two reconstruction-count expectations. These were DECLARED and
        // never parsed: `--expect-from-source 999 --expect-skipped 777`
        // returned SELFTEST OK, and both ctest registrations handed their
        // numbers to a loop that discarded them -- so the "counts must be
        // RIGHT, not merely self-consistent" half of the evidence never ran.
        // Found by independent review round 2; this file already warns about
        // exactly this class three times in capitals.
        if (const char* value = valueFor(i, "--expect-from-source", present); present) {
            if (value == nullptr || value[0] == '\0') gUnknownSample = true;
            else gExpectFromSource = std::atoi(value);
        }
        if (const char* value = valueFor(i, "--expect-skipped", present); present) {
            if (value == nullptr || value[0] == '\0') gUnknownSample = true;
            else gExpectSkipped = std::atoi(value);
        }
    }

    // A `--flag` this build does not understand is an ERROR, not something to
    // step over. The reason two dead flags went unnoticed for a milestone is
    // that from the outside an unknown flag looked exactly like a known one.
    // Closing the CLASS, not the instance.
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (arg[0] != '-' || arg[1] != '-') continue;
        static const char* const kKnownFlags[] = {
            "--scenario", "--import", "--sample", "--expect-from-source",
            "--expect-skipped", "--dark", "--selftest", "--screenshot", "--listen",
            "--no-listen"};
        bool known = false;
        for (const char* flag : kKnownFlags) {
            const std::size_t length = std::strlen(flag);
            if (std::strncmp(arg, flag, length) == 0 &&
                (arg[length] == '\0' || arg[length] == '='))
                known = true;
        }
        if (!known) gUnknownSample = true;
    }

    // --dark: apply a dark palette so the alternate-theme smoke test
    // (UI spec 14/21) exercises the real widgets rather than asserting that
    // theme independence works. The state colours are derived from the active
    // palette (ADR-M4-013), and this is what actually checks that.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dark") != 0) continue;
        QPalette dark;
        dark.setColor(QPalette::Window, QColor(0x2B, 0x2B, 0x2B));
        dark.setColor(QPalette::WindowText, QColor(0xE0, 0xE0, 0xE0));
        dark.setColor(QPalette::Base, QColor(0x1E, 0x1E, 0x1E));
        dark.setColor(QPalette::AlternateBase, QColor(0x2B, 0x2B, 0x2B));
        dark.setColor(QPalette::Text, QColor(0xE0, 0xE0, 0xE0));
        dark.setColor(QPalette::Button, QColor(0x35, 0x35, 0x35));
        dark.setColor(QPalette::ButtonText, QColor(0xE0, 0xE0, 0xE0));
        dark.setColor(QPalette::Highlight, QColor(0x2D, 0x5A, 0x88));
        dark.setColor(QPalette::HighlightedText, QColor(0xFF, 0xFF, 0xFF));
        dark.setColor(QPalette::Disabled, QPalette::Text, QColor(0x80, 0x80, 0x80));
        dark.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x80, 0x80, 0x80));
        app.setStyle(QStringLiteral("Fusion"));
        app.setPalette(dark);
        break;
    }

    // The document and kernel are owned HERE, not by any Qt object
    // (ADR-M4-006, UI spec 20): Qt presentation objects never own the semantic
    // CAD model, and both outlive the window they are displayed in.
    auto model = std::make_unique<DemoModel>(SampleFromName(sampleName));
    DocumentPresenter presenter(model->document);
    presenter.recomputeForDisplay();

    // Scenarios mutate the document BEFORE the window exists, so what is shown
    // is genuinely the state the name claims -- not a screenshot taken during a
    // transition.
    if (scenario != nullptr) {
        if (std::strcmp(scenario, "failed-profile") == 0 && sampleName == nullptr) {
            // Break the rectangle: remove one side. Pad fails, and the tree and
            // status bar must say so.
            const ObjectId sketchId = model->document.sketches().front()->id();
            model->document.editSketch(sketchId, [](Sketch& sketch) {
                const SketchEntityId last = sketch.entities().back().id;
                sketch.removeEntity(last);
            });
        } else if (std::strcmp(scenario, "empty") == 0) {
            model->document.removeObject(model->baseSolid->id());
        } else if (std::strcmp(scenario, "long-length") == 0) {
            model->document.setParameterValue(model->padLength->id(), 137.5);
        }
        presenter.recomputeForDisplay();
    }

    MainWindow window(model->document, presenter);
    window.resize(1440, 900);
    window.show();

    // DXF import, driven through MainWindow exactly as the menu action does.
    // This is the half of the workflow no unit test reaches: the panel commit,
    // the recompute, the redisplay and the status message.
    QString importReport;
    bool importedSketchIsClosed = false;
    if (importPath != nullptr) {
        importReport = window.importDxfFile(QString::fromUtf8(importPath));
        // Whether the import yielded an extrudable profile, which decides how
        // many solids the viewer should be showing.
        if (!model->document.sketches().empty())
            importedSketchIsClosed =
                static_cast<bool>(BuildProfile(*model->document.sketches().back()));
    }

    // Selection scenarios need the window to exist first.
    if (scenario != nullptr && std::strcmp(scenario, "pad-selected") == 0)
        window.selectObject(model->baseSolid->id());
    if (scenario != nullptr && std::strcmp(scenario, "sketch-selected") == 0)
        window.selectObject(model->document.sketches().front()->id());

    // --- The script socket (M17.28) -----------------------------------------
    //
    // LOOPBACK ONLY. This executes commands that create, modify and save files;
    // bound anywhere reachable it would be an unauthenticated command service.
    // ScriptServer has no flag to widen it, and this is the only call site.
    std::unique_ptr<ScriptServer> scriptServer;
    if (!listenDisabled) {
        scriptServer = std::make_unique<ScriptServer>(model->document, [&window]() {
            // The WINDOW, after every exchange. A socket that edited the
            // document while the view showed the old one would be worse than no
            // socket: the user would be looking at a lie.
            window.refreshAll();
        });
        QString listenError;
        // A BUSY PORT MUST NEVER STOP EP3D STARTING.
        //
        // This began as fprintf-then-exit(2), which was survivable while the
        // socket was opt-in and is not now: a second copy of EP3D would refuse
        // to open at all because the first one holds 5310, with the reason
        // written to a stream a GUI application does not have. So a taken port
        // falls back to any free one -- the FIRST instance keeps the
        // well-known number, which is also what makes `ep3d --connect` with no
        // argument predictable.
        if (!scriptServer->listen(static_cast<quint16>(listenPort), &listenError) &&
            !scriptServer->listen(0, &listenError)) {
            // Both refused: something is wrong with sockets on this machine,
            // not with the port. Say so and carry on WITHOUT the socket rather
            // than not opening -- a CAD program whose windows will not appear
            // because a network feature failed is the wrong trade.
            std::fprintf(stderr, "could not open a script socket: %s\n",
                         listenError.toStdString().c_str());
            scriptServer.reset();
        }
        if (scriptServer) {
            // BOTH: the TITLE BAR for whoever is looking at the window -- which
            // is the only place a GUI application can say anything -- and
            // stdout for whoever launched from a terminal, because with a
            // fallback port the caller cannot know the number otherwise.
            window.showScriptPort(scriptServer->port());
            std::printf("listening on 127.0.0.1:%u\n", scriptServer->port());
            std::fflush(stdout);
        }
    }

    if (!selfTest) return app.exec();

    int status = 0;
    QTimer::singleShot(1500, &app, [&] {
        const auto fail = [&status](const char* what) {
            std::fprintf(stderr, "SELFTEST FAIL: %s\n", what);
            status = 1;
        };
        // The window really came up (this alone catches the platform-plugin
        // defect, which aborted before any of the below could run).
        if (gUnknownSample) fail("--sample named a sample that does not exist");
        if (!window.isVisible()) fail("main window is not visible");
        if (window.width() < 800 || window.height() < 500) fail("main window is undersized");

        // The document produced a solid and the viewer is willing to show it.
        //
        // TWO when a file was imported: the demo model's own Pad, plus the one
        // the import now builds on the imported sketch. That second solid is
        // the point -- until M7's review, importing produced no solid at all,
        // so editing a reconstructed Width changed nothing the user could see
        // (spec 27 steps 9-10). A closed profile is required for it, so a
        // drawing that does not close still yields just the one.
        const Sample sampleBuilt = SampleFromName(sampleName);
        const std::size_t expectedSolids =
            (importPath != nullptr && importedSketchIsClosed) ? 2u : 1u;
        if (sampleBuilt != Sample::FailedProfile && sampleBuilt != Sample::M5Conflict &&
            presenter.displayableSolids().size() != expectedSolids)
            fail("the viewer is not showing the expected number of solids");

        // Mass properties are current and match the analytical oracle, so the
        // status bar cannot be showing stale or wrong numbers. The expected
        // volume depends on which sample was built, so this is a real oracle
        // for each rather than one number that only holds for the default.
        const Sample built = sampleBuilt;
        const double kPi = 3.14159265358979323846;
        double expectedVolume = 100000.0;                 // 100 x 50 x 20
        if (built == Sample::CircleR10) expectedVolume = kPi * 100.0 * 30.0;
        if (built == Sample::CircleR20) expectedVolume = kPi * 400.0 * 30.0;
        // pi * 20^2 * 20: the M5 circle's PadLength is the rectangle's 20 mm.
        if (built == Sample::M5Circle) expectedVolume = kPi * 400.0 * 20.0;
        // Pad minus pocket: 100*50*20 - 20*30*10 (M8 chain).
        if (built == Sample::M8Chain) expectedVolume = 94000.0;
        // The M10 frame sample is the same 100 x 50 x 20 pad, moved -- so the
        // volume oracle is the default 100000 and the CENTRE OF MASS is what
        // this sample actually proves. Asserted below.
        // pi*(30^2 - 10^2)*50, the annulus of a full revolution (M8.2).
        if (built == Sample::M8Revolve) expectedVolume = kPi * 40000.0;
        // The Minkowski rounded box, r = 2 on a 100 x 50 x 20 pad (M8.3).
        if (built == Sample::M8Dress)
            expectedVolume = 70656.0 + 26752.0 + 632.0 * kPi + (4.0 / 3.0) * kPi * 8.0;
        // The M5 rectangle solves to the SAME 100 x 50 the M4 one is drawn at,
        // padded 20 -- which is the point: the solved result must match the
        // analytical oracle, not merely be self-consistent.

        const MassProperties& mp = model->document.massProperties();
        const bool expectFailure =
            built == Sample::FailedProfile || built == Sample::M5Conflict;
        if (expectFailure) {
            // These samples are SUPPOSED to fail: the document must report no
            // current mass and offer nothing to draw.
            if (mp.valid) fail("a failing sample still reports current mass properties");
            if (!presenter.displayableSolids().empty())
                fail("a failing sample still offers a solid to draw");
        } else if (gExpectSkipped > 0) {
            // An import that SKIPPED a dimension has no analytical volume, and
            // that is not a gap in the oracle -- the skipped dimension is
            // precisely what would have pinned the size, so the geometry keeps
            // whatever the drawing happened to carry. Asserting a number here
            // would be asserting the fixture's draughting error to six
            // figures. What must hold is that a partial reconstruction still
            // produces a real solid; the skip-row checks below carry the rest.
            if (!mp.valid) fail("a partly reconstructed import produced no mass properties");
            if (mp.volumeMm3 <= 0.0) fail("a partly reconstructed import produced no volume");
        } else if (built == Sample::M5UnderConstrained) {
            // Deliberately NO volume oracle. Nothing pins this sketch's size,
            // so its solved dimensions are whatever least-change answer the
            // solver lands on -- asserting a number would be asserting solver
            // internals. What IS required is that an under-constrained sketch
            // still produces a real solid and reports DOF > 0.
            if (!mp.valid) fail("an under-constrained sketch produced no mass properties");
            if (mp.volumeMm3 <= 0.0) fail("an under-constrained sketch produced no volume");
        } else {
            if (!mp.valid) fail("mass properties are not current");
            if (std::fabs(mp.volumeMm3 - expectedVolume) >
                1e-6 * std::max(1.0, expectedVolume))
                fail("volume does not match the sample's analytical value");
        }

        // M11.3: THE EXPRESSION UI, CHECKED AT THE WIDGET.
        //
        // Every assertion below reads the property TABLE, not the model. That
        // is the whole of M6.14's lesson: `propertiesOf()` returned ten correct
        // rows while the user saw ten labels and no values, and every
        // data-level test agreed with the model. ApplyPropertyEdit is unit
        // tested; what only a running window can answer is whether any of it
        // reaches the screen.
        if (built == Sample::M11Expression) {
            window.selectObject(model->padLength->id());

            if (!window.hasPropertyRow("Expression"))
                fail("the property panel has no Expression row for a parameter");
            if (window.displayedPropertyValue("Expression") != "#PadBase / 2")
                fail("the panel is not showing the expression that drives the value");
            // 20, not 999: the panel shows the EVALUATED value.
            if (window.displayedPropertyValue("Value").rfind("20", 0) != 0)
                fail("the panel is not showing the value the expression produced");
            // A driven value must not be typeable -- editing it would silently
            // delete the formula (ADR-M11-006).
            const QString refusedValue = window.editPropertyByLabel("Value", QStringLiteral("5"));
            if (!refusedValue.contains(QStringLiteral("not editable")))
                fail("the value row is editable while an expression drives it");
            // And it must SAY why, naming the expression.
            if (window.displayedPropertyTooltip("Value").find("#PadBase / 2") == std::string::npos)
                fail("the value row does not say what drives it");

            // A REFUSED expression: the message carries a column, the typed
            // text survives in the cell, and the caret rendering is delivered.
            const QString refused =
                window.editPropertyByLabel("Expression", QStringLiteral("#PadBase / #Nope"));
            if (!refused.contains(QStringLiteral("col ")))
                fail("a refused expression did not report a column");
            if (window.displayedPropertyValue("Expression") != "#PadBase / #Nope")
                fail("a refused expression lost the text the user typed");
            const std::string tip = window.displayedPropertyTooltip("Expression");
            if (tip.find('^') == std::string::npos)
                fail("the refused expression has no caret rendering in its tooltip");
            if (tip.find("Nope") == std::string::npos)
                fail("the refused expression's tooltip does not name the problem");
            // The document is untouched by a refusal.
            if (model->padLength->expression() != "#PadBase / 2")
                fail("a refused expression changed the document");

            // A GOOD edit: the value follows, and the panel says so.
            const QString accepted =
                window.editPropertyByLabel("Expression", QStringLiteral("#PadBase / 4"));
            if (accepted.isEmpty()) fail("an accepted expression produced no status line");
            if (window.displayedPropertyValue("Expression") != "#PadBase / 4")
                fail("the panel did not adopt the accepted expression");
            if (window.displayedPropertyValue("Value").rfind("10", 0) != 0)
                fail("the value did not follow the new expression");
            if (std::fabs(model->document.massProperties().volumeMm3 - 50000.0) > 1e-6)
                fail("the solid did not rebuild from the edited expression");

            // CLEARING gives the value row back.
            window.editPropertyByLabel("Expression", QString());
            if (!window.displayedPropertyValue("Expression").empty())
                fail("clearing the expression left text behind");
            const QString typed = window.editPropertyByLabel("Value", QStringLiteral("30"));
            if (typed.contains(QStringLiteral("not editable")))
                fail("the value row is still locked after the expression was cleared");
            if (window.displayedPropertyValue("Value").rfind("30", 0) != 0)
                fail("a plain value typed after clearing did not take");
        }

        // M5: the solver actually ran and the UI can say what happened.
        // Checked HERE, in the running program, because everything below is
        // invisible to a unit test -- and the M4 round taught that four
        // user-facing defects survived two review rounds precisely because
        // nothing ever started the executable (ADR-M4-012).
        if (IsM5(built)) {
            const Sketch* sketch = model->document.findSketch(model->sketchId);
            if (sketch == nullptr) {
                fail("the M5 sample has no sketch");
            } else {
                const DocumentOutline outline(model->document);
                const std::vector<PropertyRow> rows = outline.propertiesOf(sketch->id());
                const auto rowValue = [&rows](const char* label) -> std::string {
                    for (const PropertyRow& row : rows)
                        if (row.label == label) return row.value;
                    return {};
                };
                // Status and DOF are readable as TEXT, not as a colour
                // (spec 18): a status the user cannot read is not reported.
                if (rowValue("Solve status").empty()) fail("the sketch shows no solve status");
                if (rowValue("Degrees of freedom").empty())
                    fail("the sketch shows no degrees of freedom");
                // THE CONSTRAINT LIST IS THE PANEL'S, not the tree's (M26.10).
                // Every constraint still has a row, and the row still says
                // whether the solver blamed it -- which is spec 18's actual
                // requirement, and it is now asked of the surface that carries
                // it.
                if (ConstraintRowsFor(model->document, *sketch).size() !=
                    sketch->constraints().size())
                    fail("the constraint panel does not show every constraint");
                {
                    const OutlineNode root = outline.build();
                    std::size_t constraintRows = 0;
                    const std::function<void(const OutlineNode&)> count =
                        [&](const OutlineNode& node) {
                            if (node.kind == OutlineKind::Constraint) ++constraintRows;
                            for (const OutlineNode& child : node.children) count(child);
                        };
                    count(root);
                    if (constraintRows != 0)
                        fail("the model tree is listing constraints again");
                }

                if (built == Sample::M5Circle) {
                    if (sketch->solveStatus() != SketchSolveStatus::Solved)
                        fail("the constrained circle did not solve");
                    if (sketch->degreesOfFreedom() != 0)
                        fail("the constrained circle does not report DOF 0");
                } else if (built == Sample::M5Rectangle) {
                    if (sketch->solveStatus() != SketchSolveStatus::Solved)
                        fail("the fully constrained rectangle did not solve");
                    if (sketch->degreesOfFreedom() != 0)
                        fail("the fully constrained rectangle does not report DOF 0");
                    if (rowValue("Degrees of freedom") != "0")
                        fail("the panel does not show DOF 0 for a solved rectangle");
                } else if (built == Sample::M5Conflict) {
                    if (sketch->solveStatus() == SketchSolveStatus::Solved)
                        fail("a conflicting sketch reported success");
                    // Naming the offending constraint is the whole point: a
                    // status with no id leaves the user nothing to act on.
                    if (rowValue("Offending constraint IDs").empty())
                        fail("a conflicting sketch names no offending constraint");
                } else if (built == Sample::M5UnderConstrained) {
                    if (sketch->degreesOfFreedom() <= 0)
                        fail("an under-constrained sketch reports no free degrees");
                }
            }
        }

        // The DXF import workflow really ran, in the running application.
        if (importPath != nullptr) {
            if (importReport.isEmpty()) fail("the import produced no status message");
            if (importReport.contains(QStringLiteral("failed")))
                fail("the DXF import failed");
            if (model->document.sketches().empty())
                fail("the import produced no sketch");
            else {
                const Sketch* imported = model->document.sketches().back();
                if (imported->entities().empty())
                    fail("the imported sketch has no entities");

                // Select it exactly as a user clicking the tree row does, then
                // check the panel is READABLE -- not merely correct.
                //
                // This is the half no data-level test reaches. propertiesOf()
                // returned ten fully populated rows while the owner, looking at
                // the running application, saw ten labels and no values: the
                // sketch's ninety-character profile diagnostic had sized the
                // value column to itself and pushed the whole column out of the
                // panel. Correct data, invisible to the person it was for.
                window.selectObject(imported->id());
                if (!window.propertyPanelFitsItsPanel())
                    fail("the property panel is wider than its dock, so values are "
                         "pushed out of sight");

                // M7 (spec 27): reconstruction ran as part of the import, and
                // the panel SHOWS what it did -- separating what the drawing
                // stated from what EP3D inferred, which spec 3 says must never
                // be silently mixed.
                const ReconstructionReport* report =
                    window.reconstructionReportFor(imported->id());
                // Everything below reads THROUGH `report`, so it is scoped
                // rather than guarded statement by statement. Two mistakes in a
                // row here are worth the comment: reporting the failure and
                // then dereferencing anyway segfaulted (`fail()` records, it
                // does not return), and an early `return` skipped the
                // `app.quit()` at the end of this timer callback and hung the
                // test until ctest's timeout.
                if (report == nullptr) fail("the import produced no reconstruction report");
                if (report != nullptr) {
                if (report->entries.empty())
                    fail("reconstruction produced no constraints for an imported rectangle");

                // Read from the TABLE, not from propertiesOf: these rows are
                // added by the shell, and M6.14's lesson is that a UI claim has
                // to ask the widget.
                // EXACT VALUES, not merely non-empty.
                //
                // Asserting `!empty()` was worthless: independent review swapped
                // the "From source" and "Inferred" counts, deleted every skip
                // diagnostic row, and hard-coded the panel-fit guard to true --
                // and all 13 viewer smoke tests stayed green through each
                // (the count at the time of that review; 21 are registered
                // now -- historical, and dated for that reason).
                //
                // The expected numbers depend on the file, so they come from
                // the report and are cross-checked against the panel: the panel
                // must agree with the model AND the model must be right.
                const std::string fromSource = window.displayedPropertyValue("From source");
                const std::string inferred = window.displayedPropertyValue("Inferred");
                const std::string skippedShown = window.displayedPropertyValue("Skipped");
                if (fromSource != std::to_string(report->explicitCount()))
                    fail("the panel's source-dimension count disagrees with the report");
                if (inferred !=
                    std::to_string(report->entries.size() - report->explicitCount()))
                    fail("the panel's inferred count disagrees with the report");
                if (skippedShown != std::to_string(report->skipped.size()))
                    fail("the panel's skipped count disagrees with the report");

                // And the counts must be RIGHT, not merely consistent. Which
                // file was imported decides them, so each registered fixture
                // states its own.
                if (gExpectFromSource >= 0 &&
                    fromSource != std::to_string(gExpectFromSource))
                    fail("the panel does not show the expected number of source dimensions");
                if (gExpectSkipped >= 0 && skippedShown != std::to_string(gExpectSkipped))
                    fail("the panel does not show the expected number of skipped items");

                // Every skip must be READABLE, not merely present: a row whose
                // value column is nine characters wide delivers three different
                // diagnostics as three identical strings.
                //
                // EXACT, against the report's own composed string -- the same
                // discipline the three counts above already use, and the
                // discipline this row was missing (round 4, R3R4-M3). Asserting
                // only `!empty()` here let a reviewer hard-code every skip
                // detail to the literal "42" with all 19 viewer smokes and all
                // 797 ctest entries green. That is the THIRD appearance of the
                // non-emptiness class -- penalised in M7 round 1, fixed for the
                // m8-chain rows in M8 round 1, and reintroduced by the very fix
                // written to close M7 round 2's R3-M4 -- sitting directly under
                // a comment condemning it.
                if (!report->skipped.empty()) {
                    const ReconstructionSkip& first = report->skipped.front();
                    std::string expected =
                        std::string(ReconstructionSkipReasonName(first.reason)) + ": " +
                        first.detail;
                    if (!first.sourceRef.empty())
                        expected += " (source " + first.sourceRef + ")";
                    const std::string shown = window.displayedPropertyValue("Skipped item");
                    if (shown.empty())
                        fail("items were skipped but no diagnostic row is shown");
                    else if (shown != expected)
                        fail("the skip diagnostic row does not show what the report says was "
                             "skipped");
                }
                }
            }
            // The tree must SHOW it -- an import the model tree does not list
            // is an import the user cannot select or delete.
            const DocumentOutline outline(model->document);
            const OutlineNode root = outline.build();
            std::size_t sketchRows = 0;
            const std::function<void(const OutlineNode&)> count =
                [&](const OutlineNode& node) {
                    if (node.kind == OutlineKind::Sketch) ++sketchRows;
                    for (const OutlineNode& child : node.children) count(child);
                };
            count(root);
            if (sketchRows < 2) fail("the imported sketch is not in the model tree");

            // PROVENANCE DOES NOT OUTLIVE ITS SKETCH (round 4, R3R4-M2).
            //
            // The erase path shipped with no caller a test could reach and no
            // test at all: a reviewer replaced `pruneProvenance`'s whole body
            // with `return;` and nothing failed anywhere. Removing the imported
            // sketch and refreshing must drop its report -- ids come from the
            // FILE on a later load, so a surviving entry would eventually be
            // read as belonging to an unrelated sketch that reused the number.
            if (!model->document.sketches().empty()) {
                const ObjectId importedSketch = model->document.sketches().back()->id();
                if (window.reconstructionReportFor(importedSketch) != nullptr) {
                    if (!model->document.removeObject(importedSketch))
                        fail("the imported sketch could not be removed");
                    window.refreshAll();
                    if (window.reconstructionReportFor(importedSketch) != nullptr)
                        fail("the reconstruction report outlived the sketch it describes");
                }
            }
        }

        // M8 chain: the viewer shows the TAIL only, and the pocket's Depth is
        // an editable panel row -- the two facts that make the chain a usable
        // model rather than internal state (ADR-M8-003, M8.5).
        if (sampleBuilt == Sample::M8Chain) {
            if (presenter.displayableSolids().size() != 1)
                fail("the chain does not display exactly its tail");
            // Select the pocket (the tail) and read its panel THROUGH THE
            // WIDGET (M6.14's lesson).
            if (!presenter.displayableSolids().empty()) {
                window.selectObject(presenter.displayableSolids().front());
                // EXACT values, not merely non-empty (round 1's R3-M2: a
                // displayedPropertyValue hard-coded to "42" sailed through the
                // non-empty version of these checks; the import counts above
                // learned the same lesson in M7). The depth is the sample's
                // known 10 mm in the panel's fixed 3-decimal format; the base
                // must be THE pad, by id.
                if (window.displayedPropertyValue("Depth") != "10.000")
                    fail("the pocket's Depth row does not show the sample's 10.000");
                if (window.displayedPropertyValue("Base feature") !=
                    std::to_string(model->pad->id()))
                    fail("the pocket's Base feature row does not name the pad");
            }
        }

        // M8.2 in the shell: a revolve is base-capable, so it is the tail
        // itself and carries NO "Base feature" row -- the absence is part of
        // what distinguishes it from a consumer and is asserted as such.
        if (sampleBuilt == Sample::M8Revolve) {
            if (presenter.displayableSolids().size() != 1)
                fail("the revolve sample does not display exactly one solid");
            window.selectObject(model->baseSolid->id());
            if (window.displayedPropertyValue("Type") != "Revolve")
                fail("the revolve's Type row does not say Revolve");
            // 2*pi in the panel's fixed 3-decimal format, in the Parameter's
            // own unit. EXACT, for round 1's R3-M2 reason.
            if (window.displayedPropertyValue("Angle") != "6.283")
                fail("the revolve's Angle row does not show the sample's 6.283 rad");
            if (!window.displayedPropertyValue("Base feature").empty())
                fail("a base-capable revolve is claiming to consume something");
        }

        // M8.3 in the shell: the dress chain. The fillet is the tail, it
        // consumes THE PAD by id, and its editable row is a Radius in mm.
        if (sampleBuilt == Sample::M8Dress) {
            if (presenter.displayableSolids().size() != 1)
                fail("the dress chain does not display exactly its tail");
            window.selectObject(presenter.displayableSolids().front());
            if (window.displayedPropertyValue("Type") != "Fillet")
                fail("the dress chain's tail is not the Fillet");
            if (window.displayedPropertyValue("Radius") != "2.000")
                fail("the fillet's Radius row does not show the sample's 2.000");
            if (window.displayedPropertyValue("Base feature") !=
                std::to_string(model->pad->id()))
                fail("the fillet's Base feature row does not name the pad");
        }

        // M9.5: the history commands, driven THROUGH THE SHELL.
        //
        // M9's undo machinery was complete and unreachable from the running
        // application until these existed -- the shape M8 was caught in when
        // three of its four required features had no sample. So this runs the
        // commands the menu items run, and asserts what the USER is told as
        // well as what the model does.
        if (sampleBuilt == Sample::M8Chain) {
            const double before = model->document.massProperties().volumeMm3;

            // An edit, then Undo, then Redo -- through the window.
            if (!model->document.setParameterValue(model->pad->lengthParameterId(), 40.0))
                fail("the pad length could not be edited");
            window.refreshAll();
            const QString undone = window.undoCommand();
            if (undone != QStringLiteral("Undone"))
                fail("Undo did not report that it undid anything");
            if (std::fabs(model->document.massProperties().volumeMm3 - before) > 1e-6)
                fail("Undo did not restore the volume the document started at");
            if (window.redoCommand() != QStringLiteral("Redone"))
                fail("Redo did not report that it redid anything");
            if (window.undoCommand() != QStringLiteral("Undone"))
                fail("the second Undo did not run");

            // Suppress the tail: the pad becomes the displayed solid.
            const ObjectId tail = presenter.displayableSolids().front();
            window.selectObject(tail);
            if (window.toggleSuppressSelected() != QStringLiteral("Feature suppressed"))
                fail("Suppress did not report suppressing anything");
            if (presenter.displayableSolids().size() != 1)
                fail("suppressing the tail left the body with no displayable solid");
            if (presenter.displayableSolids().front() == tail)
                fail("the suppressed feature is still the one being displayed");
            window.selectObject(tail);
            if (window.toggleSuppressSelected() != QStringLiteral("Feature unsuppressed"))
                fail("Unsuppress did not report unsuppressing anything");
            if (presenter.displayableSolids().front() != tail)
                fail("unsuppressing did not restore the tail");

            // Roll back to the first feature, then forward again.
            window.selectObject(model->pad->id());
            if (window.rollbackToSelected() != QStringLiteral("Rolled back to step 1"))
                fail("Roll Back did not report the step it rolled back to");
            if (presenter.displayableSolids().size() != 1 ||
                presenter.displayableSolids().front() != model->pad->id())
                fail("rolling back to the pad did not leave the pad as what is drawn");
            if (window.rollForwardToEnd() != QStringLiteral("Rolled forward to the end"))
                fail("Roll Forward did not report running");
            if (presenter.displayableSolids().front() == model->pad->id())
                fail("rolling forward did not bring the pocket back as the tail");

            // A command with nothing to do SAYS SO rather than doing nothing
            // silently -- the user has to be able to tell the two apart.
            //
            // Checked on the REDO stack, not by draining the undo stack to
            // zero. Draining it would undo the fixture's own feature additions,
            // which DESTROYS the features -- and `model->pad` is a raw pointer
            // into them, so everything after this block would be reading freed
            // memory. A redo that recreates a feature builds a NEW object with
            // the same ObjectId; identity survives, addresses do not. Anyone
            // writing a test around undo needs to know that, so it is written
            // here rather than learned from a crash.
            while (model->document.redoDepth() > 0) window.redoCommand();
            if (window.redoCommand() != QStringLiteral("Nothing to redo"))
                fail("an empty redo stack did not say so");
            if (std::fabs(model->document.massProperties().volumeMm3 - before) > 1e-6)
                fail("the history commands did not leave the part as they found it");

            // M9.5 FEATURE CREATION, closing ADR-M8-007's deferral. A fillet on
            // the current solid, created from the menu command, then undone --
            // and the undo must take the RADIUS PARAMETER with it, which is the
            // whole reason parameter creation became an undo delta.
            const std::size_t parametersBefore = model->document.parameters().items().size();
            const std::size_t featuresBefore =
                model->document.bodies().front()->features().size();
            const QString created = window.insertFilletOnTail();
            if (!created.startsWith(QStringLiteral("Fillet created")))
                fail("Insert Fillet did not report creating anything");
            if (model->document.bodies().front()->features().size() != featuresBefore + 1)
                fail("Insert Fillet did not add a feature");
            if (model->document.parameters().items().size() != parametersBefore + 1)
                fail("Insert Fillet did not add its Radius parameter");
            if (model->document.massProperties().volumeMm3 >= before)
                fail("the fillet did not remove material");
            // ONE undo step for parameter AND feature together.
            if (window.undoCommand() != QStringLiteral("Undone"))
                fail("the created fillet could not be undone");
            if (model->document.bodies().front()->features().size() != featuresBefore)
                fail("undoing the creation left the feature behind");
            if (model->document.parameters().items().size() != parametersBefore)
                fail("undoing the creation left an orphan parameter behind");
            if (std::fabs(model->document.massProperties().volumeMm3 - before) > 1e-6)
                fail("undoing the creation did not restore the part");

            // A FILLET THAT CANNOT BE BUILT SAYS SO (ADR-M17-022, extended to
            // Fillet and Chamfer at M17.11).
            //
            // These two were still answering "Fillet created" whatever the
            // kernel did, months after Pad, Pocket and Revolve stopped. A
            // radius the geometry cannot take is the ordinary way to meet it:
            // the command reported success over a solid that had not changed.
            //
            // The part is squeezed to 2 mm and the default 2 mm radius then has
            // nowhere to go. Restored immediately afterwards, because every
            // check below this one shares the same document.
            if (model->padLength != nullptr) {
                const double keep = model->padLength->value();
                if (model->document.setParameterValue(model->padLength->id(), 2.0)) {
                    model->document.recompute();
                    const QString refused = window.insertFilletOnTail();
                    if (refused.startsWith(QStringLiteral("Fillet created")))
                        fail(("a fillet the kernel refused was reported as created: " +
                              refused.toStdString())
                                 .c_str());
                    if (!refused.contains(QStringLiteral("could not be built")))
                        fail("the refused fillet did not say what went wrong");
                    window.undoCommand();
                }
                model->document.setParameterValue(model->padLength->id(), keep);
                model->document.recompute();
                window.refreshAll();
            }
        }

        // M10: the solid is where the FRAME puts it, and the tree and panel say
        // so. Volume cannot discriminate this sample -- that is deliberate.
        if (sampleBuilt == Sample::M10Frame) {
            const MassProperties& mp = model->document.massProperties();
            if (!mp.valid) fail("the frame-supported pad produced no mass properties");
            // Root lifts +30 Z; the child turns 90 degrees about X, sending the
            // local centroid (50, 25, 10) to (50, -10, 25); the root's lift then
            // adds 30 in Z. Hand-computed, not read back.
            if (std::fabs(mp.centerOfMassMm.x - 50.0) > 1e-6 ||
                std::fabs(mp.centerOfMassMm.y + 10.0) > 1e-6 ||
                std::fabs(mp.centerOfMassMm.z - 55.0) > 1e-6)
                fail("the solid is not where its support frame puts it");

            // The tree lists the frames, nested, with the connector under the
            // frame it is on.
            const DocumentOutline outline(model->document);
            const OutlineNode root = outline.build();
            std::size_t frameRows = 0;
            std::size_t connectorRows = 0;
            const std::function<void(const OutlineNode&)> count =
                [&](const OutlineNode& node) {
                    if (node.kind == OutlineKind::Frame) ++frameRows;
                    if (node.kind == OutlineKind::Connector) ++connectorRows;
                    for (const OutlineNode& child : node.children) count(child);
                };
            count(root);
            if (frameRows != 3) fail("the model tree does not list Origin, Root and PlateFrame");
            if (connectorRows != 1) fail("the connector is not in the model tree");

            // And the panel says where the frame IS, composed -- the world row
            // is the one a user reads to know whether the part moved.
            window.selectObject(model->document.frames().back()->id());
            if (window.displayedPropertyValue("Type") != "Frame")
                fail("the frame's Type row does not say Frame");
        }

        // Selection round-trips through the shell by ObjectId.
        window.selectObject(model->baseSolid->id());
        if (window.selectedObjectId() != model->baseSolid->id())
            fail("selection did not round-trip");

        // The panel must stay readable for EVERY selectable object, not only
        // the imported sketch the defect happened to be found on.
        //
        // The column-width defect belongs to whichever row holds the longest
        // value, and every sample grows a different long one: the failed-profile
        // and imported sketches carry a ninety-character profile diagnostic, a
        // conflicting sketch names its offending constraint. Guarding only the
        // DXF path would have left the same defect reachable from samples that
        // run on every build -- the third-leg-of-the-triple shape this project
        // has now found in three review rounds.
        window.selectObject(model->baseSolid->id());
        if (!window.propertyPanelFitsItsPanel())
            fail("the Pad property panel is wider than its dock, so values are "
                 "pushed out of sight");
        // ...and the guard must be ABLE to say no. Without this, hard-coding it
        // to `return true` passes every smoke test -- which independent review
        // demonstrated.
        if (!window.panelFitGuardCanFail())
            fail("the panel-fit guard cannot detect an unreadable panel");
        for (const Sketch* sketch : model->document.sketches()) {
            window.selectObject(sketch->id());
            if (!window.propertyPanelFitsItsPanel())
                fail("a Sketch property panel is wider than its dock, so values "
                     "are pushed out of sight");
        }
        // ...and every FEATURE, not only the base solid. M8 added four feature
        // kinds with new row groups ("Chain / Base feature", "Geometry /
        // Angle", "Geometry / Radius"); a sweep that stopped at the pad would
        // have left the M6.14 defect class reachable from every one of them.
        for (const auto& body : model->document.bodies()) {
            for (const auto& feature : body->features()) {
                window.selectObject(feature->id());
                if (!window.propertyPanelFitsItsPanel())
                    fail("a Feature property panel is wider than its dock, so "
                         "values are pushed out of sight");
            }
        }

        // --- The MODEL toolbar ----------------------------------------------
        //
        // Pad, Pocket and Revolve lived only under Insert. A command a user has
        // to go hunting for in a menu is one they do not know exists -- which
        // is how Revolve went unnoticed for a whole milestone.
        {
            const int buttons = window.modelToolbarButtonCount();
            if (buttons < 5) fail("the model toolbar is missing commands");
            if (window.modelToolbarButtonsWithIcons() != buttons)
                fail("a model toolbar button has no icon");

            bool sawPad = false;
            bool sawPocket = false;
            bool sawRevolve = false;
            bool sawOnFace = false;
            std::vector<unsigned long long> prints;
            for (int i = 0; i < buttons; ++i) {
                const std::string label = window.modelToolbarLabel(i);
                if (label.find("Pad") != std::string::npos) sawPad = true;
                if (label.find("Pocket") != std::string::npos) sawPocket = true;
                if (label.find("Revolve") != std::string::npos) sawRevolve = true;
                if (label.find("On Face") != std::string::npos) sawOnFace = true;
                const unsigned long long print = window.modelToolbarIconFingerprint(i);
                if (print == 0) fail("a model toolbar icon rendered as nothing");
                for (std::size_t j = 0; j < prints.size(); ++j)
                    if (prints[j] == print)
                        fail("two model toolbar buttons carry the SAME icon");
                prints.push_back(print);
            }
            if (!sawPad) fail("the model toolbar has no Pad button");
            if (!sawPocket) fail("the model toolbar has no Pocket button");
            if (!sawRevolve) fail("the model toolbar has no Revolve button");
            if (!sawOnFace) fail("the model toolbar has no Sketch-on-Face button");

            // M26.1: the M19-M22 features, which had no button at all until
            // now. Checked BY LABEL rather than by counting, so a button that
            // was renamed or dropped says which one.
            //
            // The icons are already covered: the fingerprint sweep above
            // refuses two buttons that carry the same picture, which is the
            // check that catches "I drew the sweep icon like the pad icon".
            for (const char* wanted : {"Sweep", "Loft", "Shell", "Hole", "Union", "Subtract",
                                       "Intersect", "Ring", "Along", "Export", "Import"}) {
                bool found = false;
                for (int i = 0; i < buttons; ++i)
                    if (window.modelToolbarLabel(i).find(wanted) != std::string::npos)
                        found = true;
                if (!found) {
                    std::string message = "the model toolbar has no ";
                    message += wanted;
                    message += " button";
                    fail(message.c_str());
                }
            }

            // THE SAME ACTIONS the Insert menu holds, so availability cannot
            // differ between the two surfaces. With nothing selected, the
            // sketch-driven commands are off.
            window.selectObject(kInvalidObjectId);
            for (int i = 0; i < buttons; ++i) {
                if (window.modelToolbarLabel(i).find("Pad") == std::string::npos) continue;
                if (window.modelToolbarButtonEnabled(i))
                    fail("Pad is offered on the toolbar with no sketch selected");
            }

            // M26.1: the same rule for every new command whose inputs are not
            // there yet. A button that is offered and then refuses is worse
            // than one that is greyed out, because the refusal arrives after
            // the click.
            //
            // Import is the one exception and is checked as such: it needs
            // nothing but a file, so it is always available.
            //
            // Revolve is in this list for a reason it earned: it was never in
            // refreshCommandStates at all, so it sat enabled with nothing
            // selected while Pad next to it was correctly greyed. Nothing
            // caught it, because every check asked whether the button EXISTED.
            for (const char* off : {"Sweep", "Loft", "Hole", "Union", "Subtract", "Intersect",
                                    "Along", "Revolve"}) {
                for (int i = 0; i < buttons; ++i) {
                    if (window.modelToolbarLabel(i).find(off) == std::string::npos) continue;
                    if (window.modelToolbarButtonEnabled(i)) {
                        std::string message(off);
                        message += " is offered on the toolbar with nothing selected to use it "
                                   "on";
                        fail(message.c_str());
                    }
                }
            }
            for (int i = 0; i < buttons; ++i) {
                if (window.modelToolbarLabel(i).find("Import") == std::string::npos) continue;
                if (!window.modelToolbarButtonEnabled(i))
                    fail("Import is greyed out, but it needs nothing but a file");
            }

            // ...and each one SAYS WHAT IS MISSING rather than failing
            // silently. The message is the whole value of a refusal, and a
            // command that returned an empty string would pass every check
            // above.
            struct Refusal {
                const char* what;
                std::string message;
            };
            const Refusal refusals[] = {
                {"Sweep", window.insertSweepFromSelection().toStdString()},
                {"Loft", window.insertLoftFromSelection().toStdString()},
                {"Hole", window.insertHoleFromSelection().toStdString()},
                {"Union", window.insertBooleanOnTail(BooleanOperation::Union, "Union")
                              .toStdString()},
                {"Along", window.insertCurvePatternFromSelection().toStdString()},
            };
            for (const Refusal& refusal : refusals) {
                if (refusal.message.empty()) {
                    std::string message(refusal.what);
                    message += " refused without saying why";
                    fail(message.c_str());
                }
                if (refusal.message.find("reated") != std::string::npos) {
                    std::string message(refusal.what);
                    message += " reported success with nothing to work on";
                    fail(message.c_str());
                }
            }

            // --- Sketch on Face, with nothing picked -------------------------
            //
            // Nothing in this run has clicked a face -- there is no 3D pick in
            // a self test -- so the button must be OFF and the command must
            // still explain itself if the menu is used anyway. Those are two
            // different promises and both have been broken before: a button
            // enabled on a state it cannot act on, and a command that refuses
            // in silence.
            for (int i = 0; i < buttons; ++i) {
                if (window.modelToolbarLabel(i).find("On Face") == std::string::npos) continue;
                if (window.modelToolbarButtonEnabled(i))
                    fail("Sketch on Face is offered on the toolbar with no face picked");
            }
            const std::string refusal = window.sketchOnFaceCommand().toStdString();
            if (refusal.empty()) fail("Sketch on Face refused in silence");
            if (refusal.find("face") == std::string::npos)
                fail(("Sketch on Face's refusal does not mention a face: " + refusal).c_str());
            if (window.inSketchMode())
                fail("a refused Sketch on Face opened a sketch anyway");

            // --- M26.6: File > Run Script, driven end to end -----------------
            //
            // The dialog is the only part that cannot be driven, so everything
            // below it is. What this proves that no unit test can: the SHELL
            // reaches the same interpreter the CLI and the socket use, the
            // window is refreshed afterwards, and a script that fails says
            // WHICH LINE.
            {
                const QString directory = QDir::tempPath() + QStringLiteral("/ep3d-selftest");
                QDir().mkpath(directory);
                const QString good = directory + QStringLiteral("/run-ok.ep3ds");
                const QString bad = directory + QStringLiteral("/run-bad.ep3ds");
                const auto put = [](const QString& where, const char* text) {
                    QFile out(where);
                    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
                    out.write(text);
                    return true;
                };
                if (!put(good, "sketch RunScriptCheck\ntool rect\nclick 0 0\nclick 30 20\n"))
                    fail("could not write the script the self test runs");
                if (!put(bad, "sketch RunScriptBad\ntool rect\nwobble 1 2\n"))
                    fail("could not write the failing script the self test runs");

                const std::size_t before = window.openedSketches().size();
                const std::string ran = window.runScriptFile(good).toStdString();
                if (ran.empty()) fail("Run Script said nothing at all");
                if (window.openedSketches().size() != before + 1)
                    fail(("Run Script did not add the sketch its script draws: " + ran).c_str());
                // It says how many undo steps it left, because it is not one.
                if (ran.find("undo") == std::string::npos)
                    fail(("Run Script did not say what undo will do: " + ran).c_str());

                // A FAILING SCRIPT NAMES ITS LINE. `wobble` is line 3.
                const std::string stopped = window.runScriptFile(bad).toStdString();
                if (stopped.find("line 3") == std::string::npos)
                    fail(("a failing script did not name the line it stopped on: " + stopped)
                             .c_str());
                if (stopped.find("wobble") == std::string::npos)
                    fail(("a failing script did not say what it did not understand: " + stopped)
                             .c_str());
                // ...and the lines BEFORE the failure stayed. A run that rolled
                // them back would throw away the only clue to what went wrong.
                if (window.openedSketches().size() != before + 2)
                    fail("a failing script discarded the lines that had already worked");

                // A PATH THAT IS NOT THERE is a named refusal, not a crash and
                // not silence.
                const std::string missing =
                    window.runScriptFile(directory + QStringLiteral("/not-here.ep3ds"))
                        .toStdString();
                if (missing.find("not-here") == std::string::npos)
                    fail(("a missing script did not name the file: " + missing).c_str());
            }

            // --- AND THE MIRROR OF IT ---------------------------------------
            //
            // A button that is never enabled is a command that does not exist.
            // Every check above asks what is REFUSED, so greying Revolve out
            // could have switched it off for good and nothing here would have
            // said a word -- the same one-sided test that let it sit enabled
            // in the first place. Select a sketch and it has to come back.
            if (!model->document.sketches().empty()) {
                window.selectObject(model->document.sketches().front()->id());
                for (const char* on : {"Pad", "Revolve"}) {
                    for (int i = 0; i < buttons; ++i) {
                        if (window.modelToolbarLabel(i).find(on) == std::string::npos) continue;
                        if (!window.modelToolbarButtonEnabled(i)) {
                            std::string message(on);
                            message += " stays greyed out with a sketch selected";
                            fail(message.c_str());
                        }
                    }
                }
                window.selectObject(kInvalidObjectId);
            }
        }

        // --- The main toolbar: Undo and Redo are REACHABLE ------------------
        //
        // They were menu-and-shortcut only, which answers "can I undo?" only by
        // opening a menu. The toolbar answers it at a glance -- but only if the
        // buttons are there, carry icons, and carry DIFFERENT ones: Undo and
        // Redo are exact mirrors by design, and a mirroring bug that produced
        // two identical arrows would look deliberate.
        {
            const int buttons = window.mainToolbarButtonCount();
            if (buttons < 5) fail("the main toolbar is missing commands");
            if (window.mainToolbarButtonsWithIcons() != buttons)
                fail("a main toolbar button has no icon");

            bool sawUndo = false;
            bool sawRedo = false;
            std::vector<unsigned long long> prints;
            for (int i = 0; i < buttons; ++i) {
                const std::string label = window.mainToolbarLabel(i);
                if (label.find("Undo") != std::string::npos) sawUndo = true;
                if (label.find("Redo") != std::string::npos) sawRedo = true;
                const unsigned long long print = window.mainToolbarIconFingerprint(i);
                if (print == 0) fail("a main toolbar icon rendered as nothing");
                for (std::size_t j = 0; j < prints.size(); ++j) {
                    if (prints[j] == print) {
                        std::fprintf(stderr, "  main buttons %zu and %d share an icon\n", j, i);
                        fail("two main toolbar buttons carry the SAME icon");
                    }
                }
                prints.push_back(print);
            }
            if (!sawUndo) fail("the main toolbar has no Undo button");
            if (!sawRedo) fail("the main toolbar has no Redo button");
        }

        // --- M12: the sketch UI, driven through the shell -------------------
        //
        // Everything below is reachable ONLY here. tests/SketchCanvasTests.cpp
        // proves what a click MEANS and tests/Solver/SketchCanvasSolveTests.cpp
        // proves what the solver then says -- neither can see whether any of it
        // was PAINTED. That is the M6.14 defect exactly: the model was right and
        // the screen was empty, and every test asked the model.
        if (built == Sample::M12Sketch) {
            window.newSketchCommand();
            if (!window.inSketchMode()) fail("New Sketch did not open the sketch canvas");

            // --- M26.2: the shell around a sketch, not just the canvas -----
            //
            // The model toolbar and the tree act on FEATURES -- neither means
            // anything while a sketch is open -- and the constraint panel
            // moves into the column the tree just vacated rather than
            // crowding the properties dock. None of this is visible to a unit
            // test of the canvas; it is a property of the SHELL, so it is
            // checked here the same way M6.14's lesson is checked here.
            if (window.modelToolBarVisible())
                fail("the model toolbar is still on screen with a sketch open");
            if (window.modelTreeVisible())
                fail("the model tree is still on screen with a sketch open");
            if (!window.constraintPanelOnLeft())
                fail("the constraint panel did not move into the tree's column");

            // --- M15: the icon toolbar ----------------------------------
            //
            // The bar is icon-only, so an action without an icon is a blank
            // button and an action whose tooltip is empty is a command with no
            // name anywhere on screen.
            const int buttons = window.sketchToolbarButtonCount();
            if (buttons < 18) fail("the sketch toolbar is missing commands");
            if (window.sketchToolbarButtonsWithIcons() != buttons)
                fail("a sketch toolbar button has no icon");

            // DISTINCT icons. "Every button has an icon" is satisfied by giving
            // them all the same one -- which is exactly what a copy-paste slip
            // in the command table produces, and it looks fine until a user
            // tries to find one.
            std::vector<unsigned long long> fingerprints;
            for (int i = 0; i < buttons; ++i) {
                const unsigned long long print = window.sketchToolbarIconFingerprint(i);
                if (print == 0) fail("a sketch toolbar icon rendered as nothing");
                for (std::size_t j = 0; j < fingerprints.size(); ++j) {
                    if (fingerprints[j] == print) {
                        std::fprintf(stderr, "  buttons %zu and %d share an icon\n", j, i);
                        fail("two sketch toolbar buttons carry the SAME icon");
                    }
                }
                fingerprints.push_back(print);
                if (window.sketchToolbarTooltip(i).empty())
                    fail("an icon-only toolbar button has no tooltip to name it");
            }

            // NAMED buttons, not just a count. "The bar has 19 buttons" stays
            // true when the one a user is looking for is the one that is
            // missing -- which is how the owner ended up hunting for a tool
            // that had tests, a decision layer, and no way in.
            {
                bool sawUse = false;
                for (int i = 0; i < buttons; ++i)
                    if (window.sketchToolbarTooltip(i).find("Use projected geometry") !=
                        std::string::npos)
                        sawUse = true;
                if (!sawUse) fail("the sketch toolbar has no Use button");
                std::fprintf(stderr, "  sketch toolbar: %d buttons\n", buttons);
            }
            SketchCanvasWidget* canvas = window.sketchCanvas();
            if (canvas == nullptr) {
                fail("the shell has no sketch canvas");
            } else {
                // Draw a rectangle with two clicks, exactly as the user does.
                canvas->setTool(SketchTool::Rectangle);
                canvas->clickAt(Vec2{0.0, 0.0});
                canvas->clickAt(Vec2{80.0, 40.0});
                canvas->repaint();

                // FIVE: the origin point every new sketch starts with, plus the
                // rectangle's four sides.
                if (canvas->paintedEntities() != 5)
                    fail("the sketch canvas is not drawing the origin and the rectangle's 4 "
                         "lines");

                // --- M26.7: clicking sketch geometry fills the PROPERTY PANEL ----
                //
                // In sketch mode the model tree is hidden, so the panel's only
                // other source of rows is not even on screen. Before this, every
                // click inside a sketch left it blank -- the geometry was
                // selectable, highlighted and constrainable, and the one place
                // that says WHAT IT IS said nothing.
                //
                // Driven through selectAt() at real coordinates, because that is
                // the path a mouse takes. A check that called the describer
                // directly would pass with the panel never wired to the canvas
                // at all, which is the M6.14 defect exactly. What each KIND of
                // geometry reports is unit-tested in DocumentOutlineTests; what
                // is proved here is the wiring.
                //
                // IT ADDS NO GEOMETRY. Everything below reads the rectangle that
                // is already there and puts the selection back as it found it --
                // an extra entity here would change the counts and the degrees of
                // freedom that the rest of this block goes on to assert.
                {
                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();

                    // A LINE, picked mid-edge where no endpoint can win.
                    const Vec2 middle{40.0, 0.0};
                    if (!canvas->selectAt(middle))
                        fail("clicking the rectangle's bottom edge selected nothing");
                    canvas->repaint();
                    if (window.propertyRowValue("Type") != "Line")
                        fail(("clicking a line did not describe a Line: got '" +
                              window.propertyRowValue("Type") + "'")
                                 .c_str());
                    if (window.propertyRowValue("Length").empty())
                        fail("a picked line has no Length in the property panel");
                    if (!window.hasPropertyRow("Angle"))
                        fail("a picked line has no Angle in the property panel");
                    // How many constraints hold it -- the row a user wants when
                    // geometry will not drag.
                    if (!window.hasPropertyRow("On this"))
                        fail("the panel does not say how many constraints hold the picked line");

                    // AN ENDPOINT of a line is a DIFFERENT answer from the line,
                    // and the panel has to say which one the click meant.
                    canvas->clearSelection();
                    if (!canvas->selectAt(Vec2{80.0, 0.0}))
                        fail("clicking the rectangle's corner selected nothing");
                    canvas->repaint();
                    if (!window.hasPropertyRow("Picked"))
                        fail("picking an endpoint did not say WHICH part was picked");
                    if (window.propertyRowValue("Picked").find("point") == std::string::npos)
                        fail(("picking an endpoint described it as '" +
                              window.propertyRowValue("Picked") + "'")
                                 .c_str());

                    // TWO SELECTED is a selection for a CONSTRAINT, not one thing
                    // to describe. Showing the first of them would be quietly
                    // describing half of what is highlighted.
                    canvas->clearSelection();
                    canvas->selectAt(middle);
                    canvas->selectAt(Vec2{0.0, 20.0});
                    canvas->repaint();
                    if (canvas->selectionCount() == 2 && window.hasPropertyRow("Length"))
                        fail("with two things picked the panel described just one of them");

                    canvas->clearSelection();
                    canvas->repaint();
                }

                // The colour the geometry was STROKED with while the sketch is
                // still under-constrained. Kept for the comparison at the end of
                // this block: roadmap 8.1 wants the geometry itself to change
                // colour once the solver reaches DOF 0, and nothing but a
                // readback from the painter can say whether it did.
                const QColor underConstrainedColour = canvas->paintedGeometryColour();
                if (!underConstrainedColour.isValid())
                    fail("the canvas drew geometry but reported no colour for it");
                if (canvas->paintedConstraintGlyphs() < 4)
                    fail("the rectangle's constraints are not visible on the canvas");

                // The constraint manager (roadmap 6.3) has to SHOW them.
                //
                // TEN: the origin point's own Fix, then the rectangle's 2
                // horizontal + 2 vertical + 4 coincident corners, plus the
                // coincidence its first corner earned by landing ON the origin
                // point. Roadmap 4.2 will not let that snap be mere magnetism --
                // if it moves the cursor it has to produce a constraint, and the
                // constraint has to be visible and deletable like any other.
                if (window.displayedConstraintRowCount() != 10)
                    fail("the constraint panel is not listing the sketch's 10 constraints");
                if (window.displayedConstraintText(0).empty())
                    fail("the constraint panel has rows but no visible text");
                {
                    bool sawFix = false;
                    for (int i = 0; i < window.displayedConstraintRowCount(); ++i)
                        if (window.displayedConstraintText(i).find("Fix") != std::string::npos)
                            sawFix = true;
                    if (!sawFix)
                        fail("the corner dropped on the origin did not become a visible Fix");
                }

                // Dimension the bottom edge and check the LABEL reaches the
                // screen, not merely the document.
                canvas->clearSelection();
                if (!canvas->selectAt(Vec2{40.0, 0.0}))
                    fail("clicking the bottom edge selected nothing");
                const QString dimensionStatus = canvas->applyDimension(SketchEditKind::None);
                canvas->repaint();
                if (canvas->paintedDimensions() != 1)
                    fail("the dimension was created but is not drawn on the canvas");
                if (dimensionStatus.isEmpty())
                    fail("dimensioning reported nothing to the user");

                // --- Roadmap 8.1: fully constrained LOOKS different ---------
                //
                // Here and not later: the M13 block below adds geometry of its
                // own, and a sketch with a loose circle in it can never reach
                // DOF 0. The rectangle already carries the Fix its origin corner
                // earned, so ONE more dimension -- the height -- finishes it.
                {
                    canvas->clearSelection();
                    if (!canvas->selectAt(Vec2{80.0, 20.0}))
                        fail("clicking the right-hand edge selected nothing");
                    if (canvas->applyDimension(SketchEditKind::None).isEmpty())
                        fail("dimensioning the right-hand edge reported nothing");
                    canvas->clearSelection();
                    canvas->repaint();

                    if (window.displayedSketchStatus().find("Fully constrained") ==
                        std::string::npos)
                        fail("width, height and the origin anchor did not reach DOF 0");

                    const QColor solvedColour = canvas->paintedGeometryColour();
                    if (!solvedColour.isValid())
                        fail("a fully constrained sketch reported no geometry colour");
                    // A06 keeps colour a SECOND channel, never the only one --
                    // which is why the status line is checked above and not
                    // instead. But a second channel that never changes is not a
                    // channel, and only the painter can testify that it did.
                    if (solvedColour == underConstrainedColour)
                        fail("the geometry is drawn the same colour fully constrained as "
                             "under-constrained");

                    // Put the sketch back where the rest of this block expects
                    // it: one dimension, under-constrained.
                    window.undoCommand();
                    canvas->repaint();
                    if (canvas->paintedDimensions() != 1)
                        fail("undoing the height dimension did not restore the canvas");
                    if (canvas->paintedGeometryColour() != underConstrainedColour)
                        fail("the geometry did not go back to its under-constrained colour");
                }

                // A refusal must EXPLAIN itself rather than doing nothing
                // silently -- an empty selection cannot be dimensioned.
                canvas->clearSelection();
                if (canvas->applyDimension(SketchEditKind::None).isEmpty())
                    fail("a refused dimension command said nothing at all");

                // The DOF is reported as TEXT, not only as a colour (A06).
                const std::string sketchStatus = window.displayedSketchStatus();
                if (sketchStatus.find("DOF") == std::string::npos &&
                    sketchStatus.find("constrained") == std::string::npos)
                    fail("the status bar does not report the sketch's constraint state as text");

                // UNDO WHILE THE CANVAS IS OPEN. This is a widget-only defect:
                // Undo routes through onRecomputeRequested -> refreshAll, and a
                // refreshAll that did not touch the canvas left it drawing
                // geometry the document no longer had. Nothing that asks the
                // model can see that.
                window.undoCommand();
                canvas->repaint();
                if (canvas->paintedDimensions() != 0)
                    fail("Ctrl+Z removed the dimension but the canvas is still drawing it");
                if (window.displayedConstraintRowCount() != 10)
                    fail("Ctrl+Z did not refresh the constraint panel");
                window.redoCommand();
                canvas->repaint();
                if (canvas->paintedDimensions() != 1)
                    fail("Redo restored the dimension but the canvas is not drawing it");

                // --- Construction geometry, through the shell ---------------
                //
                // The flag is Core and unit-tested; what only a running window
                // can answer is whether the user can reach it and SEE the
                // result. A dashed stroke is the sole cue that a line will not
                // become an edge, so a flag with no visible consequence is a
                // flag nobody can use.
                {
                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();
                    if (!canvas->selectAt(Vec2{40.0, 0.0}))
                        fail("clicking the bottom edge selected nothing");
                    canvas->repaint();
                    if (canvas->paintedConstructionEntities() != 0)
                        fail("something is drawn as construction before anything was switched");

                    const QString status = canvas->toggleConstruction();
                    if (status.isEmpty()) fail("the construction command reported nothing");
                    canvas->repaint();
                    if (canvas->paintedConstructionEntities() != 1)
                        fail("the switched line is not drawn as construction geometry");
                    // Still DRAWN, just differently: construction geometry that
                    // vanished would be a delete with a friendly name.
                    if (canvas->paintedEntities() != 5)
                        fail("switching to construction removed the line from the canvas");

                    // A refusal explains itself.
                    canvas->clearSelection();
                    if (canvas->toggleConstruction().isEmpty())
                        fail("a refused construction command said nothing at all");

                    // And back, in one undo.
                    window.undoCommand();
                    canvas->repaint();
                    if (canvas->paintedConstructionEntities() != 0)
                        fail("Ctrl+Z did not put the construction line back to normal");
                }

                // --- Offset and Distance-to-Line, THROUGH THE SHELL ---------
                //
                // Both shipped once with tests, a decision layer and NO BUTTON.
                // Every unit test passed and neither command existed as far as
                // a user was concerned. That is the M6.14 defect in its purest
                // form, and this block is the only thing that can see it.
                {
                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();
                    const int entitiesBefore = canvas->paintedEntities();

                    // Offset needs a selection, and says so when it has none.
                    if (!canvas->applyOffset(10.0).contains(QStringLiteral("Select")))
                        fail("Offset with nothing selected did not say what to select");

                    if (!canvas->selectAt(Vec2{40.0, 0.0}))
                        fail("clicking the bottom edge selected nothing");
                    const QString status = canvas->applyOffset(12.0);
                    if (status.isEmpty()) fail("Offset reported nothing to the user");
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 1)
                        fail("Offset did not draw the copy it created");
                    // The COPY is selected, so the next command acts on it.
                    if (canvas->selectionCount() != 1)
                        fail("Offset did not leave its copy selected");

                    // ...and it came with the constraints that make it an
                    // offset rather than a second parallel line.
                    bool sawParallel = false;
                    bool sawDistance = false;
                    for (int i = 0; i < window.displayedConstraintRowCount(); ++i) {
                        const std::string row = window.displayedConstraintText(i);
                        if (row.find("Parallel") != std::string::npos) sawParallel = true;
                        if (row.find("PointLineDistance") != std::string::npos)
                            sawDistance = true;
                    }
                    if (!sawParallel) fail("the offset copy is not listed as Parallel");
                    if (!sawDistance)
                        fail("the offset copy has no distance constraint holding it there");

                    // Undo takes the copy AND its constraints in one step.
                    window.undoCommand();
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore)
                        fail("Ctrl+Z did not remove the offset copy");
                }

                {
                    // Distance to Line: one point, one line, one dimension.
                    const int entitiesBefore = canvas->paintedEntities();
                    canvas->setTool(SketchTool::Point);
                    canvas->clickAt(Vec2{40.0, 30.0});
                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();
                    if (!canvas->selectAt(Vec2{40.0, 30.0}))
                        fail("clicking the new point selected nothing");
                    if (!canvas->selectAt(Vec2{40.0, 0.0}))
                        fail("clicking the bottom edge selected nothing");
                    if (canvas->selectionCount() != 2)
                        fail("the point and the line are not both selected");

                    const int dimensionsBefore = canvas->paintedDimensions();
                    if (canvas->applyDimension(SketchEditKind::AddPointLineDistance).isEmpty())
                        fail("the distance-to-line command reported nothing");
                    canvas->repaint();
                    if (canvas->paintedDimensions() != dimensionsBefore + 1)
                        fail("the distance-to-line dimension is not drawn on the canvas");

                    // TWICE: the dimension, then the point that was drawn for
                    // it. The blocks below count entities, and a stray point
                    // left here would fail them somewhere far from its cause.
                    window.undoCommand();
                    window.undoCommand();
                    canvas->repaint();
                    canvas->clearSelection();
                    if (canvas->paintedEntities() != entitiesBefore)
                        fail("the distance-to-line check left geometry behind");
                }

                // --- Trim, THROUGH THE SHELL --------------------------------
                //
                // Written because Offset shipped with tests, a decision layer
                // and no button. A command is not reachable until something
                // drives it the way a user does.
                {
                    const int entitiesBefore = canvas->paintedEntities();
                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();

                    // A line that overhangs the rectangle's right-hand edge.
                    canvas->setTool(SketchTool::Line);
                    canvas->clickAt(Vec2{40.0, 25.0});
                    const QString drawn = canvas->clickAt(Vec2{120.0, 25.0});
                    if (drawn.isEmpty()) fail("drawing the line to trim reported nothing");
                    canvas->pressEscape();
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 1)
                        fail("the line to trim was not drawn");

                    // Trim is a MODE, and the button says so.
                    canvas->setTrimming(true);
                    if (!canvas->trimming()) fail("the canvas did not enter trim mode");
                    canvas->repaint();
                    if (!window.trimButtonChecked())
                        fail("the canvas is trimming but the Trim button is not pressed");

                    // Click the overhang, past the rectangle's right edge at x=80.
                    const QString status = canvas->clickAt(Vec2{100.0, 25.0});
                    if (status.isEmpty()) fail("Trim reported nothing to the user");
                    canvas->repaint();
                    // The line is still THERE -- trimmed, not deleted.
                    if (canvas->paintedEntities() != entitiesBefore + 1)
                        fail("Trim removed the whole line instead of the overhang");

                    // Esc leaves the mode, and the button follows.
                    canvas->pressEscape();
                    canvas->repaint();
                    if (canvas->trimming()) fail("Esc did not leave trim mode");
                    if (window.trimButtonChecked())
                        fail("Esc left trim mode but the button is still pressed");

                    // AN ARC trims too, since ADR-M17-018 gave its tips
                    // variables. Only the shell can say whether the hit-test
                    // offers an arc at all -- it used to look at lines only.
                    canvas->setTool(SketchTool::Arc);
                    canvas->clickAt(Vec2{150.0, 50.0});   // centre
                    canvas->clickAt(Vec2{180.0, 50.0});   // radius 30, due east
                    canvas->clickAt(Vec2{150.0, 80.0});   // sweeping to due north
                    canvas->pressEscape();
                    canvas->setTool(SketchTool::Line);
                    canvas->clickAt(Vec2{150.0, 50.0});
                    canvas->clickAt(Vec2{200.0, 100.0});  // a 45-degree cutter
                    canvas->pressEscape();
                    canvas->repaint();
                    const int withArc = canvas->paintedEntities();

                    canvas->setTrimming(true);
                    // Click the arc near due north, past the 45-degree crossing.
                    // Asserts it TRIMMED, not merely that it said something:
                    // a refusal ("click the part of a line or arc") is also a
                    // non-empty string, and a mutation that dropped arcs from
                    // the hit-test slipped past a check that only asked for
                    // text -- failing several blocks later, far from its cause.
                    if (!canvas->clickAt(Vec2{154.0, 79.7}).contains(QStringLiteral("Trimmed")))
                        fail("clicking an arc in trim mode did not trim it");
                    canvas->pressEscape();
                    canvas->repaint();
                    if (canvas->paintedEntities() != withArc)
                        fail("trimming an arc removed it instead of shortening it");
                    window.undoCommand();  // the trim
                    window.undoCommand();  // the cutter line
                    window.undoCommand();  // the arc
                    canvas->repaint();

                    // A refusal explains itself: nothing crosses out here.
                    canvas->setTrimming(true);
                    if (canvas->clickAt(Vec2{300.0, 300.0}).isEmpty())
                        fail("a refused trim said nothing at all");
                    canvas->pressEscape();

                    // PICKING A TOOL LEAVES TRIM MODE, and drawing works again.
                    //
                    // Trim swallows the whole click, so a mode left running
                    // makes every drawing tool look broken -- the button
                    // lights, the cursor changes, and nothing is ever drawn.
                    canvas->setTrimming(true);
                    canvas->setTool(SketchTool::Line);
                    if (canvas->trimming())
                        fail("choosing a drawing tool did not leave trim mode");
                    if (window.trimButtonChecked())
                        fail("the Trim button is still pressed after choosing a tool");
                    const int beforeDraw = canvas->paintedEntities();
                    canvas->clickAt(Vec2{200.0, 200.0});
                    canvas->clickAt(Vec2{260.0, 200.0});
                    canvas->repaint();
                    if (canvas->paintedEntities() != beforeDraw + 1)
                        fail("a drawing tool cannot draw after trim mode was used");
                    window.undoCommand();
                    canvas->pressEscape();

                    // Undo puts the whole line back, then remove it entirely so
                    // the blocks below see the sketch they expect.
                    window.undoCommand();
                    window.undoCommand();
                    canvas->repaint();
                    canvas->clearSelection();
                    if (canvas->paintedEntities() != entitiesBefore)
                        fail("the trim check left geometry behind");
                }

                // --- Extend and Chamfer, THROUGH THE SHELL ------------------
                //
                // Same reason as the Trim block: a command with a decision
                // layer and no reachable button is a command that does not
                // exist, and only this can tell the difference.
                {
                    const int entitiesBefore = canvas->paintedEntities();

                    // A short line well inside the rectangle, pointing at its
                    // right-hand edge at x = 80.
                    canvas->setTool(SketchTool::Line);
                    canvas->clickAt(Vec2{20.0, 15.0});
                    canvas->clickAt(Vec2{50.0, 15.0});
                    canvas->pressEscape();
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 1)
                        fail("the line to extend was not drawn");

                    canvas->setExtending(true);
                    if (!canvas->extending()) fail("the canvas did not enter extend mode");
                    canvas->repaint();
                    if (!window.extendButtonChecked())
                        fail("the canvas is extending but the Extend button is not pressed");
                    // The two picking modes are exclusive.
                    canvas->setTrimming(true);
                    if (canvas->extending())
                        fail("turning on Trim left Extend running as well");
                    canvas->setExtending(true);
                    if (canvas->trimming()) fail("turning on Extend left Trim running as well");

                    // Click near the far end: it stretches to the rectangle's edge.
                    if (canvas->clickAt(Vec2{48.0, 15.0}).isEmpty())
                        fail("Extend reported nothing to the user");
                    canvas->pressEscape();
                    if (canvas->extending()) fail("Esc did not leave extend mode");
                    if (window.extendButtonChecked())
                        fail("Esc left extend mode but the button is still pressed");

                    // A drawing tool works afterwards -- the lesson Trim taught.
                    canvas->setExtending(true);
                    canvas->setTool(SketchTool::Line);
                    if (canvas->extending())
                        fail("choosing a drawing tool did not leave extend mode");

                    window.undoCommand(); // the extend
                    window.undoCommand(); // the line
                    canvas->repaint();
                    canvas->clearSelection();
                    if (canvas->paintedEntities() != entitiesBefore)
                        fail("the extend check left geometry behind");
                }

                {
                    // Chamfer the rectangle's bottom-right corner. The two
                    // sides are already joined by a Coincident, which is the
                    // interesting part: it has to GO.
                    const int entitiesBefore = canvas->paintedEntities();
                    const int rowsBefore = window.displayedConstraintRowCount();
                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();
                    if (!canvas->selectAt(Vec2{40.0, 0.0}))
                        fail("clicking the bottom edge selected nothing");
                    if (!canvas->selectAt(Vec2{80.0, 20.0}))
                        fail("clicking the right edge selected nothing");
                    if (canvas->selectionCount() != 2)
                        fail("the two sides of the corner are not both selected");

                    const QString status = canvas->applyChamfer(10.0, 10.0);
                    if (status.isEmpty()) fail("Chamfer reported nothing to the user");
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 1)
                        fail("the chamfer line was not drawn");
                    // One coincidence released, two created: net +1 row.
                    if (window.displayedConstraintRowCount() != rowsBefore + 1)
                        fail("the chamfer did not replace the corner's coincidence with two");

                    // ONE undo for the whole thing.
                    window.undoCommand();
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore)
                        fail("Ctrl+Z did not undo the whole chamfer");
                    if (window.displayedConstraintRowCount() != rowsBefore)
                        fail("Ctrl+Z did not restore the corner's coincidence");
                    canvas->clearSelection();
                }

                // --- Mirror, THROUGH THE SHELL ------------------------------
                {
                    const int entitiesBefore = canvas->paintedEntities();
                    const int rowsBefore = window.displayedConstraintRowCount();

                    // A short line to mirror, and a vertical axis well clear of
                    // the rectangle so nothing snaps to anything unintended.
                    canvas->setTool(SketchTool::Line);
                    canvas->clickAt(Vec2{150.0, 10.0});
                    canvas->clickAt(Vec2{170.0, 30.0});
                    canvas->pressEscape();
                    canvas->setTool(SketchTool::Line);
                    canvas->clickAt(Vec2{200.0, -20.0});
                    canvas->clickAt(Vec2{200.0, 60.0});
                    canvas->pressEscape();
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 2)
                        fail("the mirror sample geometry was not drawn");

                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();
                    // Refused until there is something AND an axis.
                    if (canvas->applyMirror().isEmpty())
                        fail("Mirror with nothing selected said nothing at all");

                    if (!canvas->selectAt(Vec2{160.0, 20.0}))
                        fail("clicking the line to mirror selected nothing");
                    // The AXIS LAST -- that is the rule the tooltip states.
                    if (!canvas->selectAt(Vec2{200.0, 20.0}))
                        fail("clicking the mirror axis selected nothing");
                    if (canvas->selectionCount() != 2)
                        fail("the source and the axis are not both selected");

                    const QString status = canvas->applyMirror();
                    if (status.isEmpty()) fail("Mirror reported nothing to the user");
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 3)
                        fail("Mirror did not draw the reflected copy");
                    // TIED, not stamped: two symmetries, one per end.
                    int symmetries = 0;
                    for (int i = 0; i < window.displayedConstraintRowCount(); ++i)
                        if (window.displayedConstraintText(i).find("Symmetric") !=
                            std::string::npos)
                            ++symmetries;
                    if (symmetries != 2)
                        fail("the mirrored line is not tied to its original at both ends");

                    // One command, one Ctrl+Z.
                    window.undoCommand();
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 2)
                        fail("Ctrl+Z did not undo the whole mirror");
                    // ...and clean up the two sample lines.
                    window.undoCommand();
                    window.undoCommand();
                    canvas->repaint();
                    canvas->clearSelection();
                    if (canvas->paintedEntities() != entitiesBefore)
                        fail("the mirror check left geometry behind");
                    if (window.displayedConstraintRowCount() != rowsBefore)
                        fail("the mirror check left constraints behind");
                }

                // --- Dragging geometry, THROUGH THE SHELL -------------------
                //
                // The Core tests prove the CONSTRAINTS decide. What only a
                // running window can say is whether a press picks anything up,
                // whether a release commits it, and whether a plain click still
                // just selects.
                {
                    const int entitiesBefore = canvas->paintedEntities();
                    canvas->setTool(SketchTool::Line);
                    canvas->clickAt(Vec2{150.0, 100.0});
                    canvas->clickAt(Vec2{190.0, 100.0});
                    canvas->pressEscape();
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 1)
                        fail("the line to drag was not drawn");
                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();

                    // A PLAIN CLICK still only selects, and costs no undo step.
                    const std::size_t depthAfterDraw = model->document.undoDepth();
                    canvas->clickAt(Vec2{190.0, 100.0});
                    if (canvas->isDraggingGeometry()) {
                        // Picked up, as a press does -- releasing without
                        // moving must record nothing.
                        canvas->finishGeometryDrag();
                    }
                    if (model->document.undoDepth() != depthAfterDraw)
                        fail("clicking a point without moving it left an undo step behind");

                    // A REAL DRAG: grab the free end and move it.
                    if (!canvas->beginGeometryDrag(Vec2{190.0, 100.0}))
                        fail("pressing on a line's endpoint did not pick it up");
                    if (!canvas->isDraggingGeometry())
                        fail("the canvas does not think a drag is running");
                    canvas->updateGeometryDrag(Vec2{200.0, 130.0});
                    canvas->updateGeometryDrag(Vec2{210.0, 150.0});
                    const QString status = canvas->finishGeometryDrag();
                    if (status.isEmpty()) fail("finishing a drag reported nothing to the user");
                    canvas->repaint();
                    if (canvas->isDraggingGeometry()) fail("the drag did not end on release");

                    // ONE undo step for the whole drag, however many moves.
                    if (model->document.undoDepth() != depthAfterDraw + 1)
                        fail("a drag did not collapse into ONE undo step");
                    window.undoCommand();
                    canvas->repaint();

                    // ESC MID-DRAG puts it back and records nothing.
                    if (!canvas->beginGeometryDrag(Vec2{190.0, 100.0}))
                        fail("could not pick the endpoint up a second time");
                    canvas->updateGeometryDrag(Vec2{220.0, 160.0});
                    const std::size_t depthMidDrag = model->document.undoDepth();
                    if (!canvas->pressEscape()) fail("Esc did nothing during a drag");
                    if (canvas->isDraggingGeometry()) fail("Esc did not abandon the drag");
                    if (model->document.undoDepth() != depthMidDrag)
                        fail("an abandoned drag left an undo step behind");

                    // ...and the geometry really is back where it started.
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 1)
                        fail("Esc during a drag lost geometry");

                    window.undoCommand(); // remove the sample line
                    canvas->repaint();
                    canvas->clearSelection();
                    if (canvas->paintedEntities() != entitiesBefore)
                        fail("the drag check left geometry behind");
                }

                // --- A REFUSAL SURVIVES THE MOUSE ---------------------------
                //
                // Reported by the owner: select two line endpoints, press
                // Concentric, "nothing happens". The refusal WAS produced --
                // and then erased by the next repaint, which a canvas with
                // mouse tracking gets on the first pixel of movement. A message
                // nobody can read is silence, which is the failure roadmap 8 is
                // written against.
                {
                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();
                    // Two ENDPOINTS of two different lines -- the owner's exact
                    // selection.
                    if (!canvas->selectAt(Vec2{0.0, 0.0}))
                        fail("clicking the first endpoint selected nothing");
                    if (!canvas->selectAt(Vec2{80.0, 40.0}))
                        fail("clicking the second endpoint selected nothing");

                    const QString refusal =
                        window.applySketchCommand(SketchEditKind::AddConcentric, false);
                    if (refusal.isEmpty()) fail("Concentric on two points said nothing at all");
                    // It NAMES the command they wanted. "Not a circle or an arc"
                    // is true and useless.
                    if (!refusal.contains(QStringLiteral("Coincident")))
                        fail("the Concentric refusal does not say to use Coincident");
                    if (window.displayedSketchMessage().empty())
                        fail("the refusal never reached the status line");

                    // NOW MOVE THE MOUSE. This is the whole test.
                    canvas->hoverAt(Vec2{40.0, 25.0});
                    canvas->hoverAt(Vec2{41.0, 26.0});
                    canvas->repaint();
                    if (window.displayedSketchMessage().empty())
                        fail("moving the mouse erased the reason the command refused");

                    // Doing something REAL clears it again -- a refusal that
                    // outlived its own answer would be its own kind of lie.
                    if (window.applySketchCommand(SketchEditKind::AddCoincident, false).isEmpty())
                        fail("Coincident on the two endpoints reported nothing");
                    canvas->hoverAt(Vec2{45.0, 25.0});
                    canvas->repaint();
                    if (!window.displayedSketchMessage().empty())
                        fail("the status line is stuck on a message the user has answered");
                    window.undoCommand();
                    canvas->clearSelection();
                }

                // --- Fillet, THROUGH THE SHELL ------------------------------
                {
                    const int entitiesBefore = canvas->paintedEntities();
                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();
                    // The rectangle's bottom and right sides meet at a corner.
                    if (!canvas->selectAt(Vec2{40.0, 0.0}))
                        fail("clicking the bottom edge selected nothing");
                    if (!canvas->selectAt(Vec2{80.0, 20.0}))
                        fail("clicking the right edge selected nothing");
                    const QString status = canvas->applyFillet(10.0);
                    if (status.isEmpty()) fail("Fillet reported nothing to the user");
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 1)
                        fail("the fillet arc was not drawn");

                    // It SURVIVES a recompute still joined -- the thing an arc
                    // with a fixed sweep could never do.
                    (void)model->document.recompute();
                    canvas->repaint();
                    const std::string sketchStatus = window.displayedSketchStatus();
                    if (sketchStatus.find("Conflicting") != std::string::npos)
                        fail("the fillet made the sketch conflicting");
                    if (canvas->paintedEntities() != entitiesBefore + 1)
                        fail("the fillet arc vanished on recompute");

                    window.undoCommand();
                    canvas->repaint();
                    canvas->clearSelection();
                    if (canvas->paintedEntities() != entitiesBefore)
                        fail("Ctrl+Z did not undo the whole fillet");
                }

                // --- Esc leaves the tool, and the TOOLBAR says so -----------
                //
                // Every drawing tool, because the bug this guards against is
                // per-button: the checked state used to be set only inside a
                // button's own handler, so Esc returned the canvas to the arrow
                // while the toolbar went on showing Rectangle pressed. The
                // model was right and the screen was wrong -- and no test that
                // asks the model can see it.
                {
                    struct Case {
                        SketchTool tool;
                        const char* label;
                        Vec2 first;
                    };
                    const Case kCases[] = {
                        {SketchTool::Line, "Line", Vec2{200.0, 200.0}},
                        {SketchTool::Rectangle, "Rectangle", Vec2{200.0, 220.0}},
                        {SketchTool::Circle, "Circle", Vec2{220.0, 200.0}},
                        {SketchTool::Arc, "Arc", Vec2{240.0, 200.0}},
                        {SketchTool::Point, "Point", Vec2{260.0, 200.0}},
                    };
                    const int entitiesBefore = canvas->paintedEntities();
                    for (const Case& one : kCases) {
                        canvas->setTool(one.tool);
                        if (canvas->tool() != one.tool)
                            fail("the canvas did not take the tool it was given");
                        if (window.checkedSketchToolLabel() != one.label)
                            fail("the toolbar is not showing the tool that was selected");

                        // Half-draw with it. Point completes on one click, so it
                        // is left alone -- there is nothing half-drawn to abandon.
                        if (one.tool != SketchTool::Point) canvas->clickAt(one.first);

                        canvas->pressEscape();
                        if (canvas->tool() != SketchTool::Select)
                            fail("Esc did not return the canvas to Select");
                        if (window.checkedSketchToolLabel() != std::string("Select"))
                            fail("Esc returned the canvas to Select but the toolbar still "
                                 "shows the drawing tool pressed");
                    }
                    canvas->repaint();
                    // ONE press, so nothing was committed on the way out.
                    if (canvas->paintedEntities() != entitiesBefore)
                        fail("abandoning a shape with Esc left geometry behind");
                }

                // --- Dimension FROM the origin -----------------------------
                //
                // The measurement almost every mechanical sketch starts with,
                // and it was impossible: the canvas drew a marker at (0,0) and
                // snapped to it, but nothing was there to select, so nothing
                // could be measured from it. A real fixed Point is what makes
                // this reachable, and only the running shell can say whether a
                // user can actually pick it.
                {
                    const SketchEntityId origin = canvas->originPoint();
                    if (origin == kInvalidSketchEntityId)
                        fail("a new sketch has no origin point to dimension from");

                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();
                    if (!canvas->selectAt(Vec2{0.0, 0.0}))
                        fail("clicking the origin selected nothing");
                    // The far corner of the rectangle.
                    if (!canvas->selectAt(Vec2{80.0, 40.0}))
                        fail("clicking the rectangle's far corner selected nothing");
                    if (canvas->selectionCount() != 2)
                        fail("the origin and the corner are not both selected");

                    const int dimensionsBefore = canvas->paintedDimensions();
                    const QString status = canvas->applyDimension(SketchEditKind::None);
                    if (status.isEmpty())
                        fail("dimensioning from the origin reported nothing");
                    canvas->repaint();
                    if (canvas->paintedDimensions() != dimensionsBefore + 1)
                        fail("the origin-to-corner dimension is not drawn on the canvas");

                    // It measures the DIAGONAL of an 80x40 rectangle, and it is
                    // a real driving dimension rather than a label: a value that
                    // did not come from the geometry would still look right.
                    const double expected = std::sqrt(80.0 * 80.0 + 40.0 * 40.0);
                    bool found = false;
                    for (int i = 0; i < window.displayedConstraintRowCount(); ++i) {
                        const std::string row = window.displayedConstraintText(i);
                        if (row.find("Distance") == std::string::npos) continue;
                        found = true;
                    }
                    if (!found)
                        fail("the origin-to-corner dimension is not listed as a Distance");
                    bool matched = false;
                    for (const auto& parameter : model->document.parameters().items())
                        if (parameter != nullptr &&
                            std::abs(parameter->value() - expected) < 1e-6)
                            matched = true;
                    if (!matched)
                        fail("the origin-to-corner dimension was not seeded at what it measures");

                    // Undo it: the block below counts on the sketch it was left.
                    window.undoCommand();
                    canvas->repaint();
                    if (canvas->paintedDimensions() != dimensionsBefore)
                        fail("Ctrl+Z did not remove the origin-to-corner dimension");
                    canvas->clearSelection();
                }

                // --- The DIMENSION TOOL, through the shell (M17.18) ---------
                //
                // Onshape's shape: pick the geometry, then click where the
                // dimension line goes. What only a running window can answer
                // is whether the placement click lands the dimension WHERE IT
                // WAS CLICKED -- "the tool created a dimension" and "it is
                // where the user put it" are two claims, and the second is the
                // whole reason the tool exists.
                {
                    const int constraintsBefore = window.displayedConstraintRowCount();
                    canvas->setDimensioning(true);
                    if (!canvas->dimensioning()) fail("Dimension mode would not switch on");

                    // Pick the bottom edge. One line is a LENGTH -- inferred,
                    // not chosen from eight buttons.
                    canvas->clickAt(Vec2{40.0, 0.0});
                    if (canvas->selectionCount() != 1)
                        fail("the first dimension click did not pick the edge");

                    // With something dimensionable picked, the pending
                    // dimension is DRAWN at the cursor: a user asked to click a
                    // position for something invisible is guessing.
                    canvas->hoverAt(Vec2{40.0, -18.0});
                    canvas->repaint();
                    if (canvas->paintedDimensionGhosts() < 1)
                        fail("the pending dimension is not shown before it is placed");

                    // The placement click.
                    const QString placed = canvas->dimensionClickAt(Vec2{40.0, -18.0});
                    if (!canvas->lastCommandApplied())
                        fail(("the placement click created no dimension: " +
                              placed.toStdString())
                                 .c_str());
                    if (window.displayedConstraintRowCount() != constraintsBefore + 1)
                        fail("the dimension tool did not add exactly one constraint");

                    // WHERE IT WAS CLICKED. Read back from the document, not
                    // from what the tool intended.
                    const Sketch* dimensioned = window.openedSketches().empty()
                                                    ? nullptr
                                                    : window.openedSketches().back();
                    if (dimensioned == nullptr) fail("the dimensioned sketch went missing");
                    bool placedWhereClicked = false;
                    if (dimensioned != nullptr)
                        for (const SketchConstraint& constraint : dimensioned->constraints()) {
                            const Vec2* at = dimensioned->dimensionPlacement(constraint.id);
                            if (at == nullptr) continue;
                            if (std::fabs(at->x - 40.0) < 1e-6 && std::fabs(at->y + 18.0) < 1e-6)
                                placedWhereClicked = true;
                        }
                    if (!placedWhereClicked)
                        fail("the dimension was not placed where the second click was");

                    // The selection is cleared, so the next click starts a NEW
                    // dimension rather than adding to the one just finished.
                    if (canvas->selectionCount() != 0)
                        fail("the finished dimension left its picks selected");

                    // Esc backs out of the mode; a drawing tool leaves it too.
                    canvas->pressEscape();
                    if (canvas->dimensioning()) fail("Esc did not leave dimension mode");
                    canvas->setTool(SketchTool::Select);

                    // UNDONE, so the checks below meet the sketch they expect.
                    // Every block here shares one document, and a dimension
                    // left behind is a constraint the next block counts.
                    window.undoCommand();
                    if (window.displayedConstraintRowCount() != constraintsBefore)
                        fail("undoing the placed dimension did not restore the sketch");
                    canvas->clearSelection();
                }

                // --- Roadmap 6.3: find a constraint, see it, throw it away --
                //
                // The panel already LISTED constraints; listing is not managing.
                // Section 6.3 asks for two more things, and section 8.2 point 2
                // for the reason: a user diagnosing a sketch has to be able to
                // pick a row and see WHICH geometry it is about, then delete it.
                // Neither is reachable from a unit test -- one is a ring drawn
                // by the painter, the other a button on a dock.
                {
                    canvas->clearSelection();
                    if (!window.selectConstraintRow(0))
                        fail("could not select the first constraint row");
                    canvas->repaint();
                    if (canvas->paintedHighlightedGlyphs() != 1)
                        fail("selecting a constraint row did not ring its glyph on the canvas");
                    if (canvas->paintedHighlightedEntities() < 1)
                        fail("selecting a constraint row did not emphasise the geometry it names");
                    // The canvas selection is UNTOUCHED: picking a row is a
                    // diagnosis, not a change of what the user has picked.
                    if (canvas->selectionCount() != 0)
                        fail("selecting a constraint row clobbered the canvas selection");
                    if (!window.constraintDeleteButtonEnabled())
                        fail("the delete button is disabled while a constraint row is selected");
                    if (window.constraintDeleteButtonText().empty())
                        fail("the delete button has no label");
                    const SketchConstraintId pinned = canvas->highlightedConstraint();
                    if (pinned == kInvalidSketchConstraintId)
                        fail("selecting a row did not record which constraint is highlighted");

                    // Now over-constrain it ON PURPOSE and recover. A second
                    // length on the bottom edge, disagreeing with the first, is
                    // the smallest honest conflict -- and getting out of it by
                    // deleting the offender is the entire point of the panel.
                    const int rowsBefore = window.displayedConstraintRowCount();
                    canvas->clearSelection();
                    if (!canvas->selectAt(Vec2{40.0, 0.0}))
                        fail("clicking the bottom edge selected nothing");
                    if (canvas->applyDimension(SketchEditKind::None).isEmpty())
                        fail("adding a second length reported nothing");
                    canvas->clearSelection();
                    const int addedRow = window.displayedConstraintRowCount() - 1;
                    if (addedRow != rowsBefore)
                        fail("the second length did not appear in the constraint panel");

                    // The panel was just REBUILT, and the highlight survived it.
                    // Every recompute rebuilds these rows, so a highlight that
                    // did not survive one would blink out at exactly the moment
                    // the user is watching the sketch resolve.
                    if (canvas->highlightedConstraint() != pinned)
                        fail("rebuilding the constraint panel dropped the highlight");
                    canvas->repaint();
                    if (canvas->paintedHighlightedGlyphs() != 1)
                        fail("the ring is gone after the panel was rebuilt");
                    const auto addedId = static_cast<SketchConstraintId>(static_cast<ObjectId>(
                        window.displayedConstraintId(addedRow)));
                    if (canvas->commitDimensionText(addedId, QStringLiteral("55")).isEmpty())
                        fail("committing the contradicting value reported nothing");
                    canvas->repaint();

                    const SketchStatusLine conflicted = canvas->statusLine();
                    if (conflicted.badge == "OK")
                        fail("two disagreeing lengths on one edge are reported as OK");

                    // Prefer whatever the solver BLAMED -- that is the row a
                    // user would reach for. Fall back to the one just added when
                    // it named nobody, so the recovery is still exercised.
                    int target = -1;
                    for (int i = 0; i < window.displayedConstraintRowCount(); ++i)
                        if (window.displayedConstraintText(i).find("AT FAULT") !=
                            std::string::npos)
                            target = i;
                    if (target < 0) target = addedRow;
                    if (!window.selectConstraintRow(target))
                        fail("could not select the offending constraint row");
                    if (!window.constraintDeleteButtonEnabled())
                        fail("the delete button is disabled on the offending row");
                    if (window.clickConstraintDeleteButton().isEmpty())
                        fail("deleting a constraint reported nothing to the user");
                    canvas->repaint();

                    if (window.displayedConstraintRowCount() != rowsBefore)
                        fail("deleting the constraint did not remove its row");
                    if (canvas->statusLine().badge == conflicted.badge &&
                        conflicted.badge != "OK")
                        fail("the sketch is still reporting trouble after the offender was "
                             "deleted");
                    // The highlight went WITH it rather than sliding onto
                    // whatever took its row.
                    if (canvas->highlightedConstraint() != kInvalidSketchConstraintId)
                        fail("the deleted constraint is still highlighted");
                    if (canvas->paintedHighlightedGlyphs() != 0)
                        fail("a ring is still drawn for a constraint that no longer exists");
                }

                // --- The other direction: click the BADGE, press Delete -----
                //
                // Roadmap 6.3's own words: "click the constraint icon on the
                // canvas, then press Delete". Both halves are reachable only
                // here -- one is a hit-test against a box the painter placed,
                // the other a key event -- and the round trip is what proves
                // the panel and the canvas agree about what is selected.
                {
                    canvas->setTool(SketchTool::Select);
                    canvas->clearSelection();

                    // Pick a badge that still exists, by asking the canvas where
                    // it drew one rather than guessing at coordinates.
                    const int rowsBefore = window.displayedConstraintRowCount();
                    if (rowsBefore < 1) fail("no constraints left to pick on the canvas");
                    const auto wantedId = static_cast<SketchConstraintId>(
                        static_cast<ObjectId>(window.displayedConstraintId(0)));
                    Vec2 badgeCentre{};
                    if (!canvas->constraintBadgeCentre(wantedId, &badgeCentre))
                        fail("the first listed constraint has no badge on the canvas");
                    if (canvas->constraintBadgeAt(badgeCentre) != wantedId)
                        fail("the badge cannot be hit where it is drawn");

                    canvas->clickAt(badgeCentre);
                    canvas->repaint();
                    if (canvas->highlightedConstraint() != wantedId)
                        fail("clicking a constraint badge did not highlight it");
                    if (canvas->paintedHighlightedGlyphs() != 1)
                        fail("clicking a badge did not ring it");
                    // The PANEL followed the canvas: the same constraint, and
                    // its delete button live.
                    const int followed = window.selectedConstraintRow();
                    if (followed < 0)
                        fail("clicking a badge left the constraint panel with no selection");
                    if (static_cast<SketchConstraintId>(static_cast<ObjectId>(
                            window.displayedConstraintId(followed))) != wantedId)
                        fail("the panel selected a different constraint from the one clicked");
                    if (!window.constraintDeleteButtonEnabled())
                        fail("the delete button is dead after picking a badge on the canvas");
                    // ...and the click did not also select geometry, or Delete
                    // would be ambiguous.
                    if (canvas->selectionCount() != 0)
                        fail("clicking a badge also selected geometry underneath it");

                    // Delete, from the keyboard, on the canvas.
                    //
                    // Delivered to the widget directly rather than through
                    // QApplication::sendEvent: the application redirects key
                    // events to the focus widget, and this window is never
                    // shown, so there is no focus to redirect to. What is under
                    // test is the canvas's key handling, which is what
                    // QWidget::event dispatches to.
                    QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
                    QCoreApplication::sendEvent(canvas, &del);
                    canvas->repaint();
                    if (window.displayedConstraintRowCount() != rowsBefore - 1)
                        fail("Delete did not remove the constraint whose badge was clicked");
                    if (canvas->highlightedConstraint() != kInvalidSketchConstraintId)
                        fail("the deleted constraint is still highlighted");
                    if (canvas->paintedEntities() != 5)
                        fail("Delete removed geometry as well as the constraint");

                    // Undo brings it back: a deleted constraint is an ordinary
                    // document change, not a view action (roadmap 15).
                    window.undoCommand();
                    canvas->repaint();
                    if (window.displayedConstraintRowCount() != rowsBefore)
                        fail("Ctrl+Z did not bring the deleted constraint back");
                }

                // --- M13: a geometric constraint, in the running shell ------
                //
                // Same reason the M12 block exists: SketchGeometricConstraintTests
                // proves the residual solves and SketchCanvasTests proves the
                // selection rules hold, and NEITHER can say whether a user can
                // reach any of it. Without this the seven new constraints would
                // be owner-validatable only on paper.
                const int rowsBeforeTangent = window.displayedConstraintRowCount();
                const int glyphsBeforeTangent = canvas->paintedConstraintGlyphs();

                canvas->setTool(SketchTool::Circle);
                canvas->clickAt(Vec2{40.0, 20.0});   // centre
                canvas->clickAt(Vec2{50.0, 20.0});   // rim: r = 10
                canvas->setTool(SketchTool::Select);
                canvas->clearSelection();
                if (!canvas->selectAt(Vec2{40.0, 30.0}))
                    fail("clicking the circle's rim selected nothing");
                // ...and the tangency is CHECKED, not just listed. A constraint
                // that appears in the panel and does not hold is the silent
                // failure this whole file exists to catch -- and the circle's
                // position after it is what the pick below depends on.
                if (!canvas->selectAt(Vec2{40.0, 0.0}))
                    fail("clicking the rectangle's bottom edge selected nothing");
                if (canvas->selectionCount() != 2)
                    fail("the circle and the line are not both selected");

                const QString tangentStatus =
                    canvas->applyConstraint(SketchEditKind::AddTangent);
                canvas->repaint();
                if (tangentStatus.isEmpty())
                    fail("the tangent command reported nothing to the user");
                if (window.displayedConstraintRowCount() != rowsBeforeTangent + 1)
                    fail("the tangent constraint is not listed in the constraint panel");
                if (canvas->paintedConstraintGlyphs() <= glyphsBeforeTangent)
                    fail("the tangent constraint has no glyph on the canvas");
                if (canvas->paintedEntities() != 6)
                    fail("the canvas is not drawing the circle it was asked to draw");
                {
                    const Sketch* solved = model->document.findSketch(canvas->sketchId());
                    if (solved == nullptr) fail("the sketch went away mid-test");
                    const SketchCircle* touching = nullptr;
                    for (const SketchEntity& entity : solved->entities())
                        if (const auto* circle = std::get_if<SketchCircle>(&entity.geometry))
                            touching = circle;
                    if (touching == nullptr) fail("the circle this test drew is gone");
                    // The rectangle's bottom edge is y = 0, so a tangent circle
                    // has its centre exactly one radius from it.
                    else if (std::abs(std::abs(touching->center.y) - touching->radiusMm) > 1e-6)
                        fail("the tangent constraint was listed but the circle does not touch");
                }

                // A REFUSAL has to reach the user too. Two lines cannot be
                // tangent, and a command that silently does nothing is the
                // failure roadmap section 8 is written against.
                canvas->clearSelection();
                canvas->selectAt(Vec2{40.0, 0.0});
                canvas->selectAt(Vec2{0.0, 20.0});
                if (canvas->applyConstraint(SketchEditKind::AddTangent).isEmpty())
                    fail("a refused tangent command said nothing at all");

                // --- Every dimension TYPE, on screen ------------------------
                //
                // The linear case above exercises extension lines, arrowheads
                // and rotated knocked-out text. Radial, diametral and angular
                // reuse that machinery but the ANGULAR one also strokes an arc,
                // and an arc drawn with the wrong sweep or the wrong centre is
                // a defect only a painted pixel can show.
                // WHERE THE CIRCLE IS NOW, asked rather than assumed.
                //
                // The tangency above MOVES it: a circle told to touch the
                // rectangle's bottom edge ends up with its centre one radius
                // above that edge, not where it was drawn. This used to be a
                // hard-coded (40, 30) that happened to still be on the rim, and
                // it stopped being so the day the solver got better at
                // satisfying the constraint -- a test failing because the code
                // started working.
                const auto rimOfTheCircle = [&]() {
                    const Sketch* current = model->document.findSketch(canvas->sketchId());
                    if (current == nullptr) fail("the sketch went away mid-test");
                    for (const SketchEntity& entity : current->entities())
                        if (const auto* circle = std::get_if<SketchCircle>(&entity.geometry))
                            return Vec2{circle->center.x, circle->center.y + circle->radiusMm};
                    fail("the circle this test drew is gone");
                    return Vec2{0.0, 0.0};
                };
                canvas->clearSelection();
                if (!canvas->selectAt(rimOfTheCircle()))
                    fail("could not select the circle to dimension it");
                if (canvas->applyDimension(SketchEditKind::None).isEmpty())
                    fail("dimensioning the circle reported nothing");

                // Two fresh lines making a corner. The rectangle's own sides
                // are already Horizontal and Vertical, so an angle between them
                // would be REDUNDANT -- a true diagnosis, and the wrong thing
                // to put in a screenshot of what a good dimension looks like.
                canvas->setTool(SketchTool::Line);
                canvas->clickAt(Vec2{10.0, -40.0});
                canvas->clickAt(Vec2{70.0, -40.0});
                canvas->clickAt(Vec2{95.0, -12.0});
                canvas->setTool(SketchTool::Select);
                canvas->clearSelection();
                if (!canvas->selectAt(Vec2{40.0, -40.0}))
                    fail("could not select the first leg of the corner");
                if (!canvas->selectAt(Vec2{82.0, -26.0}))
                    fail("could not select the second leg of the corner");
                if (canvas->applyDimension(SketchEditKind::None).isEmpty())
                    fail("dimensioning the corner angle reported nothing");

                canvas->repaint();
                // Length, diameter and angle -- all three drawn.
                if (canvas->paintedDimensions() != 3)
                    fail("the canvas is not drawing all three dimension types");
                // DRAWN AS DIMENSIONS, not merely as numbers. Two heads for the
                // length, two for the diameter, two on the angular arc.
                if (canvas->paintedDimensionArrows() != 6)
                    fail("the dimensions are not being drawn with arrowheads");
                if (canvas->paintedDimensionArcs() != 1)
                    fail("the angular dimension is not drawing its arc");

                // --- M16: dragging a dimension, in the running shell --------
                //
                // The layer below proves a placement REPLACES the computed
                // layout; only this can say the canvas actually hands the drag
                // through, and that one drag is one undo step rather than one
                // per mouse move.
                const SketchConstraintId dragged = canvas->dimensionAt(Vec2{40.0, -14.4});
                if (dragged == kInvalidSketchConstraintId) {
                    fail("the length dimension cannot be found where it was drawn");
                } else {
                    const std::size_t undoBefore = model->document.undoDepth();
                    if (!canvas->beginDimensionDrag(Vec2{40.0, -14.4}))
                        fail("grabbing the dimension did not start a drag");
                    // Several moves, as a real drag produces.
                    canvas->updateDimensionDrag(Vec2{40.0, -20.0});
                    canvas->updateDimensionDrag(Vec2{40.0, -26.0});
                    canvas->updateDimensionDrag(Vec2{40.0, -32.0});
                    if (!canvas->isDraggingDimension())
                        fail("the drag ended before the mouse was released");
                    if (canvas->finishDimensionDrag().isEmpty())
                        fail("finishing the drag reported nothing to the user");

                    // ONE undo step for the whole drag. Recording per move
                    // would make a drag across the canvas a thousand-step
                    // history nobody can get back through.
                    if (model->document.undoDepth() != undoBefore + 1)
                        fail("a dimension drag did not collapse into ONE undo step");

                    canvas->repaint();
                    if (canvas->paintedDimensions() != 3)
                        fail("a dragged dimension stopped being drawn");

                    // ...and it is REALLY where it was dropped.
                    const Sketch* dragTarget = model->document.findSketch(canvas->sketchId());
                    if (dragTarget == nullptr ||
                        dragTarget->dimensionPlacement(dragged) == nullptr)
                        fail("the drag did not record a placement");

                    // Auto-place puts it back, also in one step.
                    if (window.sketchCanvas()->autoPlaceAllDimensions().isEmpty())
                        fail("auto-place reported nothing");
                    if (dragTarget->dimensionPlacement(dragged) != nullptr)
                        fail("auto-place left the dimension pinned");
                }

                // --- M16: a prefix and a tolerance, through the shell -------
                if (dragged != kInvalidSketchConstraintId) {
                    const QString formatted = window.applyDimensionFormat(
                        dragged, QStringLiteral("2x "), QStringLiteral(""), 0.1, 0.1);
                    if (formatted.isEmpty()) fail("formatting a dimension reported nothing");
                    canvas->repaint();
                    const QString shown = canvas->dimensionDisplayText(dragged);
                    if (!shown.startsWith(QStringLiteral("2x ")))
                        fail("the dimension is not showing its prefix");
                    if (!shown.contains(QStringLiteral("+/-0.1")))
                        fail("the dimension is not showing its tolerance");
                    if (canvas->paintedDimensions() != 3)
                        fail("a formatted dimension stopped being drawn");
                }

                if (screenshotPath != nullptr) {
                    canvas->repaint();
                    // The WHOLE window, not just the canvas: the toolbar is
                    // now icon-only, so whether those icons read is part of
                    // what a screenshot has to answer.
                    if (!shoot(window, QString::fromUtf8(screenshotPath)))
                        fail("could not write the requested screenshot");
                    else
                        screenshotWritten = true;
                }

                // --- Projected reference geometry, through the shell --------
                //
                // The projection is unit-tested and the Use tool is unit-tested,
                // and NEITHER can answer the only question that matters here:
                // does the underlay reach the screen, and can a click convert
                // it? An underlay that is stored but never painted is a feature
                // a user cannot see; a Use mode with no button is a command
                // that does not exist. Both have shipped in this project
                // before, with every test green.
                {
                    const int entitiesBefore = canvas->paintedEntities();

                    // Refuses BEFORE there is anything to use, and says why.
                    // "Nothing happened" is the failure this block exists for.
                    canvas->setUseReference(true);
                    if (!canvas->useReference()) fail("Use mode would not switch on");
                    const QString empty = canvas->useReferenceAt(Vec2{10.0, 10.0});
                    if (!empty.contains(QStringLiteral("reference")))
                        fail("Use with no projected geometry did not say what is missing");
                    canvas->setUseReference(false);

                    // The underlay a sketch on a face would have: the far edge
                    // of the face, a hole in it, and the two corners. Added
                    // through the SAME facade call the sketch-on-face command
                    // uses, so this drives the real path.
                    const std::vector<SketchGeometry> projected = {
                        SketchLine{Vec2{0.0, 60.0}, Vec2{80.0, 60.0}},
                        SketchCircle{Vec2{40.0, 30.0}, 8.0},
                        SketchPoint{Vec2{0.0, 60.0}},
                        SketchPoint{Vec2{80.0, 60.0}}};
                    if (model->document.addSketchReferences(window.editingSketch(), projected) !=
                        projected.size())
                        fail("the document refused the projected reference geometry");

                    canvas->repaint();
                    if (canvas->paintedReferences() != 4)
                        fail("the projected reference geometry is not drawn on the canvas");
                    // And it did NOT become sketch geometry by being drawn: the
                    // whole separation would be pointless if painting merged
                    // the two.
                    if (canvas->paintedEntities() != entitiesBefore)
                        fail("projected geometry was counted as sketch geometry");

                    // Convert the reference line, exactly as a user does: turn
                    // the mode on, click the edge.
                    canvas->setUseReference(true);
                    const QString used = canvas->useReferenceAt(Vec2{40.0, 60.0});
                    if (used.isEmpty()) fail("Use reported nothing to the user");
                    if (!canvas->lastCommandApplied())
                        fail(("clicking a projected edge in Use mode converted nothing: " +
                              used.toStdString())
                                 .c_str());
                    canvas->repaint();
                    if (canvas->paintedEntities() != entitiesBefore + 1)
                        fail("the converted edge was not drawn as sketch geometry");
                    // The reference SURVIVES being used -- the underlay is what
                    // the face looked like, and using an edge did not change
                    // that.
                    if (canvas->paintedReferences() != 4)
                        fail("using a projected edge consumed it");

                    // The same edge again is REFUSED. A duplicate curve lying
                    // exactly on top of another is invisible, and it makes the
                    // profile ambiguous.
                    const QString again = canvas->useReferenceAt(Vec2{40.0, 60.0});
                    if (!again.contains(QStringLiteral("already")))
                        fail("Use converted the same edge twice");

                    // Use is a MODE, and choosing a drawing tool leaves it --
                    // the defect Trim shipped with, where the button stayed lit
                    // and nothing was ever drawn.
                    canvas->setTool(SketchTool::Line);
                    if (canvas->useReference())
                        fail("choosing a drawing tool left Use mode running");
                    canvas->setTool(SketchTool::Select);
                }

                // --- A PAD WITH A HOLE IN IT --------------------------------
                //
                // Reported by the owner: a rectangle with a circle inside it
                // padded to a solid rectangle with no hole. TWO defects behind
                // one symptom -- the profile validator refused a second loop,
                // and the command printed "Pad created" regardless, so the user
                // was told the opposite of what happened.
                //
                // Both are fixed, so this now checks the FEATURE: the pad is
                // built, and the profile really did carry the hole.
                {
                    window.newSketchCommand();
                    SketchCanvasWidget* holed = window.sketchCanvas();
                    if (holed == nullptr) fail("no canvas for the two-loop sketch");
                    holed->setTool(SketchTool::Rectangle);
                    holed->clickAt(Vec2{0.0, 0.0});
                    holed->clickAt(Vec2{100.0, 50.0});
                    holed->pressEscape();
                    holed->setTool(SketchTool::Circle);
                    holed->clickAt(Vec2{50.0, 25.0});
                    holed->clickAt(Vec2{60.0, 25.0});
                    holed->pressEscape();
                    window.finishSketchCommand();

                    // --- The sketch is IN THE PART VIEW (M17.7) -------------
                    //
                    // Reported by the owner: after Finish Sketch the sketch was
                    // simply not there. It existed only on the 2D canvas, so
                    // nothing in the part view said where it sat relative to
                    // anything else -- which is the whole reason a sketch has a
                    // plane.
                    //
                    // Counted from the SCENE, not from the presenter's list: a
                    // presenter that names a sketch and a viewer that never
                    // draws it is precisely the shape of the bug.
                    if (window.viewer() == nullptr) fail("the shell has no 3D view");
                    if (window.viewer()->displayedSketchCount() < 1)
                        fail("the finished sketch is not drawn in the part view");

                    // --- RENAMING, through the panel (M17.16) ---------------
                    //
                    // "The row is editable" and "typing into it renames the
                    // thing" are two claims, and the gap between them is
                    // exactly how a Length row that accepted typing and changed
                    // nothing survived a milestone (ADR-M17-027). This types
                    // into the cell and reads the TREE back.
                    {
                        // The sketch just finished, selected the way clicking
                        // its row selects it.
                        const ObjectId sketchId = window.openedSketches().empty()
                                                      ? kInvalidObjectId
                                                      : window.openedSketches().back()->id();
                        if (sketchId == kInvalidObjectId) fail("no sketch to rename");
                        window.selectObject(sketchId);
                        if (!window.typeIntoPropertyRow("Name", "Outline"))
                            fail("the Name row cannot be typed into");

                        bool renamed = false;
                        for (const std::string& row : window.treeRows())
                            if (row.find("Outline") != std::string::npos) renamed = true;
                        if (!renamed) fail("renaming did not reach the model tree");

                        // Renaming it to a name ANOTHER row already has is
                        // refused, and the name that was there survives --
                        // names are how a user picks what to delete.
                        window.selectObject(sketchId);
                        // "PadLength" is a PARAMETER the sample already has --
                        // taken at this exact moment, unlike a feature name
                        // that is not created until further down.
                        window.typeIntoPropertyRow("Name", "PadLength");
                        // Checked in the TREE, not in the cell. A refused edit
                        // deliberately keeps the typed text on screen so a
                        // typo does not have to be retyped (M11.3) -- so the
                        // cell reads "PadLength" and the MODEL must not.
                        bool keptItsName = false;
                        for (const std::string& row : window.treeRows())
                            if (row.find("Outline") != std::string::npos) keptItsName = true;
                        if (!keptItsName)
                            fail("a refused rename changed the name anyway");
                    }

                    // --- Every row has its OWN name (M17.15) ----------------
                    //
                    // The owner deleted one of two rows both reading "Pocket"
                    // and lost their undo history: the middle link of a chain
                    // cannot be removed reversibly, the tail can, and nothing
                    // on screen told the two apart. The tree is how a user
                    // picks what to delete.
                    {
                        std::vector<std::string> names;
                        for (std::string row : window.treeRows()) {
                            const std::size_t at = row.find_first_not_of(' ');
                            if (at != std::string::npos) row = row.substr(at);
                            // Constraints repeat by design -- four Coincidents
                            // under one sketch are four different constraints
                            // and the row shows what each one IS. Objects a
                            // user selects and deletes must not repeat.
                            if (row.rfind("[Cst]", 0) == 0) continue;
                            if (row.rfind("[Par]", 0) == 0) continue;
                            names.push_back(row);
                        }
                        for (std::size_t i = 0; i < names.size(); ++i)
                            for (std::size_t j = i + 1; j < names.size(); ++j)
                                if (names[i] == names[j])
                                    fail(("two model tree rows read the same: " + names[i])
                                             .c_str());
                    }

                    // --- The tree is a TIMELINE, and it ABSORBS (M17.10) ----
                    //
                    // The outline's shape is unit-tested; what only a running
                    // window can answer is whether it reached the QTreeWidget.
                    // "The outline nested it" and "the tree shows it nested"
                    // are two claims, and this shell exists because the gap
                    // between two such claims has shipped here before.
                    {
                        const std::vector<std::string> rows = window.treeRows();
                        const auto depthOf = [](const std::string& row) {
                            const std::size_t indent = row.find_first_not_of(' ');
                            return indent == std::string::npos
                                       ? 0
                                       : static_cast<int>(indent) / 2;
                        };
                        bool sawPad = false;
                        bool padHoldsItsSketch = false;
                        for (std::size_t i = 0; i < rows.size(); ++i) {
                            // By KIND TAG, not by name: "PadLength" is a
                            // PARAMETER whose name begins with Pad, and matching
                            // it found a row whose next sibling is another
                            // parameter -- a check that failed on a tree that
                            // was in fact correct.
                            if (rows[i].find("[Sld] Pad") == std::string::npos) continue;
                            sawPad = true;
                            // The absorbed sketch is the pad's only child, so
                            // it is the very next row, one level deeper.
                            if (i + 1 >= rows.size()) break;
                            if (depthOf(rows[i + 1]) == depthOf(rows[i]) + 1 &&
                                rows[i + 1].find("Sketch") != std::string::npos)
                                padHoldsItsSketch = true;
                            break;
                        }
                        if (!sawPad) fail("the model tree has no Pad row");
                        if (!padHoldsItsSketch) {
                            for (const std::string& row : rows)
                                std::fprintf(stderr, "  tree| %s\n", row.c_str());
                            fail("the pad's sketch is not nested inside it in the tree");
                        }
                    }

                    // --- View > Solid / Wireframe (M17.9) -------------------
                    //
                    // Two claims, and the second is the one a menu gets wrong:
                    // the VIEW changed, and the MENU says what the view is
                    // doing. A menu that ticks itself while the view does
                    // something else is the defect the sketch toolbar shipped
                    // once already.
                    {
                        if (window.wireframeMenuChecked())
                            fail("the view starts in wireframe rather than shaded");

                        const QString toWire = window.setSolidDisplayCommand(true);
                        if (!toWire.contains(QStringLiteral("wireframe")))
                            fail("switching to wireframe reported nothing about it");
                        if (window.viewer()->solidDisplay() !=
                            OcctViewWidget::SolidDisplay::Wireframe)
                            fail("the menu said wireframe and the view stayed shaded");
                        if (!window.wireframeMenuChecked())
                            fail("the view is in wireframe and the menu does not say so");
                        // The SKETCH is still drawn -- switching how solids are
                        // shaded must not remove anything from the scene.
                        if (window.viewer()->displayedSketchCount() < 1)
                            fail("wireframe mode dropped the sketch from the scene");

                        const QString toSolid = window.setSolidDisplayCommand(false);
                        if (!toSolid.contains(QStringLiteral("shaded")))
                            fail("switching back to solid reported nothing about it");
                        if (window.viewer()->solidDisplay() !=
                            OcctViewWidget::SolidDisplay::Shaded)
                            fail("the menu said solid and the view stayed in wireframe");
                        if (window.wireframeMenuChecked())
                            fail("the view is shaded and the menu still says wireframe");
                    }

                    const ObjectId holedId = window.openedSketches().empty()
                                               ? kInvalidObjectId
                                               : window.openedSketches().back()->id();
                    if (holedId == kInvalidObjectId) fail("the two-loop sketch went missing");

                    const QString status = window.insertPadFromSelection();
                    if (!status.contains(QStringLiteral("Pad created")))
                        fail("a rectangle with a circle in it still cannot be padded");

                    // THE HOLE REACHED THE PROFILE. A pad that computed is not
                    // the claim -- one that quietly dropped the circle would
                    // compute too, and look exactly like the bug being fixed.
                    const Sketch* profileSketch = window.openedSketchById(holedId);
                    if (profileSketch == nullptr) fail("could not re-read the two-loop sketch");
                    if (profileSketch != nullptr) {
                        const ProfileResult built = BuildProfile(*profileSketch);
                        if (!built) fail("the two-loop profile no longer validates");
                        if (built && built.profile.inners.size() != 1)
                            fail("the pad's profile did not carry the circle as a hole");
                    }
                    // --- TWO POCKETS, AND YOU CAN TELL THEM APART (M17.15) --
                    //
                    // The owner's own file, reproduced: Pad -> Pocket -> Pocket.
                    // Both pockets were called "Pocket", so the tree showed two
                    // identical rows. They are not interchangeable -- the
                    // middle link of a chain cannot be deleted reversibly and
                    // the tail can -- and deleting the wrong one silently
                    // cleared every undo step the owner had.
                    //
                    // The check is on the NAMES, because that is the only thing
                    // a user has to pick by.
                    {
                        window.selectObject(holedId);
                        const QString first = window.insertPocketFromSelection();
                        window.selectObject(holedId);
                        const QString second = window.insertPocketFromSelection();
                        if (!first.startsWith(QStringLiteral("Pocket created")) ||
                            !second.startsWith(QStringLiteral("Pocket created")))
                            fail("a second pocket on the same sketch was refused");

                        std::vector<std::string> pockets;
                        for (std::string row : window.treeRows()) {
                            const std::size_t at = row.find_first_not_of(' ');
                            if (at != std::string::npos) row = row.substr(at);
                            if (row.find("Pocket") != std::string::npos) pockets.push_back(row);
                        }
                        if (pockets.size() < 2) fail("the tree does not show both pockets");
                        for (std::size_t i = 0; i < pockets.size(); ++i)
                            for (std::size_t j = i + 1; j < pockets.size(); ++j)
                                if (pockets[i] == pockets[j])
                                    fail(("two pocket rows read the same: " + pockets[i])
                                             .c_str());

                        // And deleting the CONSUMED one says what it costs --
                        // which is the half the owner was never told.
                        window.selectObject(window.openedSketches().empty()
                                                ? kInvalidObjectId
                                                : kInvalidObjectId);
                        window.undoCommand();
                        window.undoCommand();
                    }

                    window.undoCommand();
                }

                // --- SKETCH TO SOLID: Revolve, through the shell -------------
                //
                // Core could build a RevolveFeature since M8 and the shell had
                // no command for it -- implemented, tested, unreachable. The
                // same gap Offset shipped with, and the reason this block
                // exists at all.
                {
                    const std::size_t bodiesBefore = model->document.bodies().empty()
                                                         ? 0
                                                         : model->document.bodies().front()->features().size();

                    // A profile beside an axis: a rectangle to the right of a
                    // vertical CONSTRUCTION line. Revolving it makes a tube.
                    window.newSketchCommand();
                    SketchCanvasWidget* revolveCanvas = window.sketchCanvas();
                    if (revolveCanvas == nullptr) fail("no canvas for the revolve sketch");
                    revolveCanvas->setTool(SketchTool::Rectangle);
                    revolveCanvas->clickAt(Vec2{20.0, 0.0});
                    revolveCanvas->clickAt(Vec2{40.0, 60.0});
                    revolveCanvas->pressEscape();

                    revolveCanvas->setTool(SketchTool::Line);
                    revolveCanvas->clickAt(Vec2{0.0, -10.0});
                    revolveCanvas->clickAt(Vec2{0.0, 70.0});
                    revolveCanvas->pressEscape();
                    revolveCanvas->setTool(SketchTool::Select);
                    revolveCanvas->clearSelection();
                    if (!revolveCanvas->selectAt(Vec2{0.0, 30.0}))
                        fail("clicking the axis line selected nothing");
                    if (revolveCanvas->toggleConstruction().isEmpty())
                        fail("marking the axis as construction reported nothing");

                    const ObjectId sketchId = revolveCanvas->sketchId();
                    window.finishSketchCommand();

                    // THE AXIS IS OBVIOUS: one construction line, so no question
                    // is asked. That is what the Construction flag buys here.
                    const SketchEntityId axis = window.obviousRevolveAxis(sketchId);
                    if (axis == kInvalidSketchEntityId)
                        fail("a sketch with ONE construction line has no obvious revolve axis");

                    // TWO construction lines is NOT an obvious answer, and
                    // taking the first would be a guess wearing a convention's
                    // clothes. Checked here because a sketch with exactly one
                    // cannot tell "exactly one" from "the first one" -- a
                    // mutation that took the first survived without this.
                    window.editSelectedSketchCommand();
                    SketchCanvasWidget* again = window.sketchCanvas();
                    if (again == nullptr) fail("could not reopen the revolve sketch");
                    again->setTool(SketchTool::Line);
                    again->clickAt(Vec2{60.0, -10.0});
                    again->clickAt(Vec2{60.0, 70.0});
                    again->pressEscape();
                    again->setTool(SketchTool::Select);
                    again->clearSelection();
                    if (!again->selectAt(Vec2{60.0, 30.0}))
                        fail("clicking the second axis candidate selected nothing");
                    again->toggleConstruction();
                    window.finishSketchCommand();
                    if (window.obviousRevolveAxis(sketchId) != kInvalidSketchEntityId)
                        fail("two construction lines still produced an 'obvious' axis");
                    // Put the sketch back to one construction line.
                    window.undoCommand();  // the construction switch
                    window.undoCommand();  // the second line
                    if (window.obviousRevolveAxis(sketchId) == kInvalidSketchEntityId)
                        fail("removing the second candidate did not restore the obvious axis");

                    const QString status = window.insertRevolveFromSelection(axis, 360.0);
                    if (status.isEmpty()) fail("Revolve reported nothing to the user");
                    // It SAYS the panel will show radians, because the user
                    // typed 360 and will read 6.2832.
                    if (!status.contains(QStringLiteral("radians")))
                        fail("Revolve did not warn that its angle reads in radians");

                    // A SOLID came out. The feature existing is not the claim --
                    // a revolve that failed would still be in the tree.
                    if (model->document.bodies().empty()) fail("Revolve created no body");
                    const auto& features = model->document.bodies().front()->features();
                    if (features.size() != bodiesBefore + 1)
                        fail("Revolve did not add exactly one feature");
                    if (features.back()->state() != ComputeState::Valid)
                        fail("the revolve feature did not compute");

                    // ...and it is PARAMETRIC: halving the angle rebuilds it.
                    //
                    // Asked of the FEATURE, not looked up by name. The name was
                    // a literal here until feature and parameter names became
                    // unique (M17.15) -- and looking a parameter up by the name
                    // a command happened to give it was always the fragile
                    // half: it asserts a naming convention while claiming to
                    // assert the revolve's own angle.
                    const auto* revolveFeature =
                        dynamic_cast<const RevolveFeature*>(features.back().get());
                    const Parameter* angle =
                        revolveFeature == nullptr
                            ? nullptr
                            : model->document.parameters().findById(
                                  revolveFeature->angleParameterId());
                    if (angle == nullptr) fail("the revolve has no angle parameter");
                    if (angle != nullptr) {
                        if (!model->document.setParameterValue(angle->id(), 3.14159265358979 / 2.0))
                            fail("the revolve angle refused a new value");
                        (void)model->document.recompute();
                        if (model->document.bodies().front()->features().back()->state() !=
                            ComputeState::Valid)
                            fail("the revolve did not rebuild after its angle changed");
                    }
                }

                // LAST IN THIS BLOCK, deliberately: Open leaves the window
                // looking at a document its owner does not hold, so anything
                // after it that inspected `model->document` would be asking the
                // wrong one. Two checks below did, and failed for that reason
                // rather than for any fault of their own.
                // --- FILE: Save then Open, through the shell ----------------
                //
                // Round-tripped through the RUNNING window, not through the
                // serializer's own tests: what only this can answer is whether
                // Open can re-seat everything that points at a document.
                // PartDocument is deliberately non-copyable AND non-movable, so
                // opening a file cannot swap a document's contents -- it has to
                // change WHICH document the window, the presenter and the
                // canvas look at, and any one of them left behind is a dangling
                // pointer that Debug will not necessarily catch.
                {
                    const std::size_t featuresBefore =
                        model->document.bodies().empty()
                            ? 0
                            : model->document.bodies().front()->features().size();
                    const std::size_t parametersBefore =
                        model->document.parameters().items().size();

                    const QString path =
                        QDir::temp().filePath(QStringLiteral("ep3d_selftest_roundtrip.ep3d"));
                    QFile::remove(path);
                    const QString saved = window.saveDocumentFile(path);
                    if (!saved.startsWith(QStringLiteral("Saved")))
                        fail("saving the document reported a failure");
                    if (!QFile::exists(path)) fail("Save wrote no file");
                    if (window.documentPath() != path)
                        fail("the window did not remember where it saved");

                    // CHANGE THE ORIGINAL AFTER SAVING, so the file and the
                    // in-memory document differ. Without this the round trip
                    // compares a document with itself, and a window that never
                    // re-seated its pointer passes every count -- which is
                    // exactly what a mutation proved.
                    model->document.addParameter("AfterSave", 1.0, UnitType::Millimeter);
                    if (model->document.parameters().items().size() != parametersBefore + 1)
                        fail("the post-save marker parameter was not added");

                    const QString opened = window.openDocumentFile(path);
                    if (!opened.startsWith(QStringLiteral("Opened")))
                        fail("opening the document just saved reported a failure");

                    // THE WINDOW IS LOOKING AT THE LOADED DOCUMENT, and it is
                    // whole. Counting through the WINDOW rather than through
                    // `model` is the point: `model` still owns the original,
                    // and a window that quietly kept pointing at it would pass
                    // any check that asked `model`.
                    if (window.openedDocumentFeatureCount() != featuresBefore)
                        fail("the opened document lost features");
                    // The FILE's parameter count, not the original's -- the
                    // marker added after saving must NOT be there.
                    if (window.openedDocumentParameterCount() != parametersBefore)
                        fail("the window is still looking at the document it had before Open");

                    // AND THE SKETCH IS STILL EDITABLE.
                    //
                    // Reported by the owner: create a sketch, save, reopen, and
                    // it can no longer be edited. A document you cannot carry on
                    // working in is not saved, it is exported.
                    {
                        const std::vector<const Sketch*> sketches = window.openedSketches();
                        if (sketches.empty())
                            fail("the opened document has no sketch to edit");
                        if (!sketches.empty()) {
                            const ObjectId reopened = sketches.front()->id();
                            window.selectObject(reopened);
                            const QString editing = window.editSelectedSketchCommand();
                            if (!window.inSketchMode())
                                fail("a sketch from a reopened document cannot be edited");
                            if (editing.contains(QStringLiteral("Select a sketch")))
                                fail("the reopened sketch could not be selected for editing");
                            // ...and it can be DRAWN in: an editable sketch that
                            // refuses geometry is editable in name only.
                            SketchCanvasWidget* reopenedCanvas = window.sketchCanvas();
                            if (reopenedCanvas == nullptr) fail("no canvas for the reopened sketch");
                            // REPAINTED FIRST. The counter reports the LAST paint,
                            // and that was of a different sketch -- reading it before
                            // repainting compares this sketch against another one.
                            reopenedCanvas->repaint();
                            const int before = reopenedCanvas->paintedEntities();
                            reopenedCanvas->setTool(SketchTool::Line);
                            reopenedCanvas->clickAt(Vec2{200.0, 200.0});
                            reopenedCanvas->clickAt(Vec2{240.0, 200.0});
                            reopenedCanvas->pressEscape();
                            reopenedCanvas->repaint();
                            if (reopenedCanvas->paintedEntities() != before + 1)
                                fail("nothing can be drawn in a sketch from a reopened document");
                            window.finishSketchCommand();
                        }
                    }

                    // --- DELETE, and FILE > NEW ----------------------------
                    //
                    // Reported by the owner: nothing in the shell could delete
                    // an existing object, and there was no way to start a fresh
                    // document. Core could do both; only the commands were
                    // missing -- the same gap Offset and Revolve shipped with.
                    {
                        const std::vector<const Sketch*> before = window.openedSketches();
                        if (before.empty()) fail("no sketch to delete");
                        if (!before.empty()) {
                            const ObjectId victim = before.back()->id();
                            window.selectObject(victim);
                            const QString deleted = window.deleteSelectedObjectCommand();
                            if (!deleted.startsWith(QStringLiteral("Deleted")))
                                fail("deleting a selected sketch reported a failure");
                            if (window.openedSketches().size() != before.size() - 1)
                                fail("the deleted sketch is still in the document");
                            // AND IT WARNS. Deleting a sketch is NOT undoable
                            // (ADR-M12-008: UndoDelta has no sketch-existence
                            // case), so the command says so before the fact
                            // rather than letting the user find out when Ctrl+Z
                            // does nothing.
                            if (!deleted.contains(QStringLiteral("cannot be undone")))
                                fail("deleting a sketch did not warn that it is irreversible");
                        }
                        // Nothing selected: refused with a reason, never silent.
                        window.selectObject(kInvalidObjectId);
                        if (!window.deleteSelectedObjectCommand().contains(
                                QStringLiteral("Select")))
                            fail("deleting with nothing selected said nothing useful");
                    }

                    // A FILE THAT IS NOT ONE changes nothing and says why.
                    const QString bogus =
                        QDir::temp().filePath(QStringLiteral("ep3d_selftest_not_a_doc.ep3d"));
                    QFile junk(bogus);
                    if (junk.open(QIODevice::WriteOnly))
                        junk.write("this is not an EP3D document"), junk.close();
                    const QString refused = window.openDocumentFile(bogus);
                    if (!refused.startsWith(QStringLiteral("Could not open")))
                        fail("opening a corrupt file did not report a failure");
                    if (window.openedDocumentFeatureCount() != featuresBefore)
                        fail("a refused open damaged the document that was already loaded");
                    QFile::remove(bogus);
                    QFile::remove(path);

                    // --- A DOCUMENT WHOSE ONLY CONTENT IS A SKETCH -------
                    //
                    // Reported by the owner against a real file: open it, and
                    // "Edit Selected Sketch" stays GREYED OUT however the
                    // sketch is clicked.
                    //
                    // The cause was not Open at all -- selectObject() updated
                    // the tree, the viewer and the properties panel but never
                    // re-armed the commands, so the enabled state only caught
                    // up when some other command happened to refresh it. Finish
                    // Sketch does, which is why the usual route worked; a
                    // document that is ONLY a sketch has no other route.
                    {
                        const QString onlySketch =
                            QDir::temp().filePath(QStringLiteral("ep3d_selftest_sketch_only.ep3d"));
                        QFile::remove(onlySketch);
                        window.newDocumentCommand();
                        window.newSketchCommand();
                        SketchCanvasWidget* only = window.sketchCanvas();
                        if (only == nullptr) fail("no canvas for the sketch-only document");
                        only->setTool(SketchTool::Rectangle);
                        only->clickAt(Vec2{0.0, 0.0});
                        only->clickAt(Vec2{40.0, 20.0});
                        only->pressEscape();
                        window.finishSketchCommand();
                        if (!window.saveDocumentFile(onlySketch).startsWith(QStringLiteral("Saved")))
                            fail("could not save the sketch-only document");

                        // Read it back, exactly as the owner did.
                        if (!window.openDocumentFile(onlySketch)
                                 .startsWith(QStringLiteral("Opened")))
                            fail("could not reopen the sketch-only document");
                        if (window.openedSketches().size() != 1u)
                            fail("the sketch-only document did not come back with its sketch");

                        // NOTHING is selected after Open, so the command is
                        // correctly unavailable...
                        if (window.editSketchEnabled())
                            fail("Edit Sketch is offered with nothing selected");
                        // ...and selecting the sketch MUST arm it. This is the
                        // whole bug: the selection landed, and the menu item
                        // stayed grey.
                        window.selectObject(window.openedSketches().front()->id());
                        if (!window.editSketchEnabled())
                            fail("selecting a sketch did not enable Edit Selected Sketch");
                        window.editSelectedSketchCommand();
                        if (!window.inSketchMode())
                            fail("the reopened sketch-only document cannot be edited");
                        window.finishSketchCommand();
                        QFile::remove(onlySketch);
                    }

                    // LAST: File > New empties the document, so any check above
                    // that counts what was loaded has to run before it.
                    {
                        // FILE > NEW: an empty document, and no path -- so the
                        // next Save must ask, rather than overwriting the file
                        // that was open a moment ago.
                        const QString made = window.newDocumentCommand();
                        if (!made.contains(QStringLiteral("New document")))
                            fail("File > New reported something unexpected");
                        if (!window.openedSketches().empty())
                            fail("a new document is not empty");
                        if (window.openedDocumentFeatureCount() != 0)
                            fail("a new document already has features");
                        if (!window.documentPath().isEmpty())
                            fail("File > New kept the previous document's path");
                        // ...and it is USABLE: a new document you cannot draw in
                        // is a blank screen with a menu bar.
                        window.newSketchCommand();
                        SketchCanvasWidget* freshCanvas = window.sketchCanvas();
                        if (freshCanvas == nullptr) fail("no canvas in a new document");
                        freshCanvas->repaint();
                        const int start = freshCanvas->paintedEntities();
                        freshCanvas->setTool(SketchTool::Rectangle);
                        freshCanvas->clickAt(Vec2{0.0, 0.0});
                        freshCanvas->clickAt(Vec2{50.0, 30.0});
                        freshCanvas->pressEscape();
                        freshCanvas->repaint();
                        if (freshCanvas->paintedEntities() != start + 4)
                            fail("a new document cannot be drawn in");
                        window.finishSketchCommand();
                    }

                }

                window.finishSketchCommand();
                if (window.inSketchMode()) fail("Finish Sketch did not leave sketch mode");
                // ...and the shell reverts, rather than leaving the tree
                // hidden and the constraint panel parked on the left forever.
                if (!window.modelToolBarVisible())
                    fail("the model toolbar did not come back when the sketch closed");
                if (!window.modelTreeVisible())
                    fail("the model tree did not come back when the sketch closed");
                if (window.constraintPanelOnLeft())
                    fail("the constraint panel stayed in the tree's column after closing");
            }
        }

        // --- The script socket, over a REAL connection (M17.28) --------------
        //
        // ScriptServer's own logic is small; what is not small is TCP. A
        // command can arrive in two pieces and two commands can arrive in one
        // packet, and an interpreter fed whatever happened to be in the buffer
        // would run half a line. Nothing but a real socket can show that.
        {
            paramcad::ScriptServer probe(model->document, [&window]() { window.refreshAll(); });
            QString listenError;
            // PORT 0: the operating system picks a free one. A fixed number
            // would make this test fail on a machine that happens to be using
            // it, which is a flake rather than a finding.
            if (!probe.listen(0, &listenError)) {
                fail("the script server could not listen on a free port");
            } else {
                const std::size_t sketchesBefore = model->document.sketches().size();
                QTcpSocket client;
                client.connectToHost(QHostAddress::LocalHost, probe.port());
                if (!client.waitForConnected(3000)) {
                    fail("could not connect to the script server");
                } else {
                    // ONE BUFFER, and the event loop PUMPED rather than
                    // blocked on.
                    //
                    // The client and the server are in the same thread here, so
                    // client.waitForReadyRead() would wait for a reply the
                    // server has not been given a chance to write -- it services
                    // one socket, and the one that needs servicing is the other
                    // one. The first version of this test did exactly that and
                    // timed out on every exchange.
                    QByteArray inbox;
                    const auto readLine = [&client, &inbox]() -> QString {
                        QElapsedTimer clock;
                        clock.start();
                        for (;;) {
                            const int newline = inbox.indexOf('\n');
                            if (newline >= 0) {
                                const QByteArray line = inbox.left(newline);
                                inbox.remove(0, newline + 1);
                                return QString::fromUtf8(line);
                            }
                            if (clock.elapsed() > 3000) return QString();
                            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                            inbox.append(client.readAll());
                        }
                    };
                    // Reads log lines until the verdict, which the protocol
                    // promises exactly one of per exchange.
                    const auto exchange = [&readLine](QString* log) -> QString {
                        for (;;) {
                            const QString line = readLine();
                            if (line.isEmpty()) return QString();
                            if (line.startsWith(QStringLiteral(". "))) {
                                if (log != nullptr) *log += line.mid(2) + "\n";
                                continue;
                            }
                            return line;
                        }
                    };

                    if (!exchange(nullptr).startsWith(QStringLiteral("OK")))
                        fail("the script server did not greet the client");

                    // TWO COMMANDS IN ONE WRITE, which is what a client that
                    // does not flush per line produces.
                    client.write("sketch Socket\ntool line\n");
                    client.flush();
                    if (!exchange(nullptr).startsWith(QStringLiteral("OK")) ||
                        !exchange(nullptr).startsWith(QStringLiteral("OK")))
                        fail("two commands in one packet were not both run");

                    // ...and ONE COMMAND SPLIT ACROSS TWO WRITES.
                    client.write("click 0 ");
                    client.flush();
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    client.write("0\n");
                    client.flush();
                    if (!exchange(nullptr).startsWith(QStringLiteral("OK")))
                        fail("a command split across two packets was not reassembled");

                    QString madeLog;
                    client.write("click 60 0\n");
                    client.flush();
                    if (!exchange(&madeLog).startsWith(QStringLiteral("OK")) ||
                        !madeLog.contains(QStringLiteral("Line1")))
                        fail("the socket did not report the geometry it made");

                    // THE STATE SURVIVED between messages: `tool line` arrived
                    // in one packet and its two clicks in others, and a line
                    // came out. That is the whole reason a session exists.
                    const paramcad::Sketch* made = nullptr;
                    for (const paramcad::Sketch* one : model->document.sketches())
                        if (one->name() == "Socket") made = one;
                    if (made == nullptr)
                        fail("the socket did not create the sketch it was told to");
                    else if (made->entities().size() != 1)
                        fail("the socket's two clicks did not make one line");
                    if (model->document.sketches().size() != sketchesBefore + 1)
                        fail("the socket created the wrong number of sketches");

                    // A REFUSAL comes back as ERR and names what was wrong.
                    client.write("tool wobble\n");
                    client.flush();
                    const QString refused = exchange(nullptr);
                    if (!refused.startsWith(QStringLiteral("ERR")))
                        fail("a refused command did not answer ERR");
                    if (!refused.contains(QStringLiteral("wobble")))
                        fail("the refusal did not name what was wrong");

                    if (probe.connectionCount() != 1)
                        fail("the script server miscounted its connections");
                }
            }
        }

        // Every OTHER sample gets its picture here, of the shell in whatever
        // state the run left it. The model toolbar is on the window in all of
        // them, which is the whole reason somebody asks for one.
        if (screenshotPath != nullptr && !screenshotWritten
            && shoot(window, QString::fromUtf8(screenshotPath)))
            screenshotWritten = true;
        // ONE check for one fact, and it covers both ways of getting here: the
        // save failed, or nothing ever tried. "Nobody said it failed" and "it
        // worked" are different sentences, and only one of them is evidence --
        // the rule the mutation harness's own oracle had to learn three times
        // over (ADR-M26-008).
        if (screenshotPath != nullptr && !screenshotWritten)
            fail("--screenshot was given and no file was written");

        // AN UNCAUGHT EXCEPTION IS abort() WITH NO MESSAGE, which is the
        // silent failure this self test exists to prevent -- reachable, it
        // turns out, from inside the self test. It says what threw now.
        try {
        // --- M27's GATE: the window opens an ASSEMBLY -------------------
        //
        // Everything below is reachable only here. AssemblyOutlineTests proves
        // what the tree SAYS; nothing but starting the program can prove that
        // the shell holds an assembly at all, draws its instances where the
        // mates put them, and turns off the commands an assembly does not have.
        //
        // The file is built by the CLI from the same example the docs quote, so
        // this cannot pass against a fixture that has drifted from what a user
        // would actually open.
        {
            const QString assemblyPath = QDir::tempPath() +
                                         QStringLiteral("/ep3d-selftest/m27-hinge.ep3da");
            QDir().mkpath(QFileInfo(assemblyPath).path());
            const QString script = QDir::tempPath() +
                                   QStringLiteral("/ep3d-selftest/m27-hinge.ep3ds");
            {
                QFile out(script);
                if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    fail("could not write the assembly script the self test opens");
                std::string text;
                {
                    QFile source(QStringLiteral("examples/hinge.ep3ds"));
                    if (!source.open(QIODevice::ReadOnly))
                        fail("examples/hinge.ep3ds is not readable from the working directory");
                    text = source.readAll().toStdString();
                }
                // Save it WHERE THIS TEST CAN FIND IT, without editing the
                // example: the last `save` line decides, and appending one wins.
                text += "\nsave " + assemblyPath.toStdString() + "\n";
                out.write(text.c_str());
            }
            const QString ran = window.runScriptFile(script);
            if (ran.contains(QStringLiteral("stopped")))
                fail(("the hinge script did not run: " + ran.toStdString()).c_str());

            const QString opened = window.openDocumentFile(assemblyPath);
            if (opened.contains(QStringLiteral("Could not open")))
                fail(("the window could not open an assembly: " + opened.toStdString()).c_str());

            // IT IS AN ASSEMBLY, and the shell knows it.
            if (window.openedDocumentType() != DocumentType::Assembly)
                fail("File > Open read an assembly file as something else");

            // THE TREE IS INSTANCES AND MATES.
            const OutlineNode root = window.probeOutline();
            std::size_t instanceRows = 0;
            std::size_t mateRows = 0;
            const std::function<void(const OutlineNode&)> count = [&](const OutlineNode& node) {
                if (node.kind == OutlineKind::Instance) ++instanceRows;
                if (node.kind == OutlineKind::Mate) ++mateRows;
                for (const OutlineNode& child : node.children) count(child);
            };
            count(root);
            if (instanceRows != 2)
                fail("the assembly tree does not show its two instances");
            if (mateRows != 1) fail("the assembly tree does not show its mate");

            // AND ON SCREEN, each where its mates put it. Two instances of two
            // different parts, so two shapes -- and NOT at the same place,
            // which is the difference between drawing an assembly and drawing
            // one part twice.
            const std::vector<DocumentPresenter::DisplayedShape> drawn =
                presenter.displayableShapes();
            if (drawn.size() != 2)
                fail("the viewer is not drawing both instances of the assembly");
            const Vec3 a = drawn[0].placement.translation;
            const Vec3 b = drawn[1].placement.translation;
            if (std::fabs(a.x - b.x) < 1e-9 && std::fabs(a.y - b.y) < 1e-9 &&
                std::fabs(a.z - b.z) < 1e-9)
                fail("both instances are drawn in the same place, so the mate did not place them");

            // AND THE PART COMMANDS ARE OFF. part() throws by design; the menus
            // are the reason it never has to.
            if (window.insertPadEnabled())
                fail("Pad is offered on an assembly, which has no sketches to pad");

            // --- M28's GATE: build a three-part assembly with the COMMANDS ---
            //
            // Insert, move, ground, pattern, delete -- then save, reopen, and
            // check the places survived. Driven through the command methods
            // because the dialogs are the only part a self test cannot work,
            // which is the split importDxfFile established.
            {
                const QString partFile = QDir::tempPath() +
                                         QStringLiteral("/ep3d-selftest/m28-block.ep3d");
                {
                    // A part to instance, built by the script interpreter so
                    // the fixture cannot drift from what the CLI produces.
                    const QString buildScript = QDir::tempPath() +
                                                QStringLiteral("/ep3d-selftest/m28-block.ep3ds");
                    QFile out(buildScript);
                    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                        fail("could not write the part script M28 instances");
                    out.write(("sketch B\ntool rect\nclick -10 -10\nclick 10 10\n"
                               "pad Block 20 as M28Thick\nsolve\nsave " +
                               partFile.toStdString() + "\n").c_str());
                    out.close();
                    window.newDocumentCommand();
                    const QString made = window.runScriptFile(buildScript);
                    if (made.contains(QStringLiteral("stopped")))
                        fail(("M28's part did not build: " + made.toStdString()).c_str());
                }

                // A FRESH ASSEMBLY, adopted the way File > Open adopts one.
                window.adoptAssemblyForTesting("M28Rig");
                if (window.openedDocumentType() != DocumentType::Assembly)
                    fail("the window did not adopt an assembly");

                // INSERT THREE. The name is derived from the file and made
                // unique, because several of one part is the ordinary case.
                for (int i = 0; i < 3; ++i) {
                    const QString said = window.insertInstanceCommand(partFile);
                    if (!said.contains(QStringLiteral("Inserted")))
                        fail(("Insert Part did not insert: " + said.toStdString()).c_str());
                }
                if (window.instanceCountForTesting() != 3)
                    fail("three inserts did not make three instances");

                // ...WITH DIFFERENT NAMES. Three rows all called "m28-block"
                // is a tree a user cannot navigate.
                if (window.instanceNamesForTesting().size() != 3)
                    fail("the three instances do not have three distinct names");

                // MOVE the one that is selected -- insert leaves it selected,
                // which is what makes "insert then place" two clicks.
                // NOBODY WAS THERE BEFORE. Asserting the destination without
                // this would pass for a command that moved nothing, if some
                // instance happened to sit there already.
                const auto anyInstanceAt = [&](double x) {
                    for (const Vec3& place : window.instancePlacesForTesting())
                        if (std::fabs(place.x - x) < 1e-6) return true;
                    return false;
                };
                if (anyInstanceAt(60.0))
                    fail("an instance was already at x=60 before anything was moved there");

                const QString moved = window.placeSelectedInstance(Vec3{60.0, 0.0, 0.0});
                if (!moved.contains(QStringLiteral("Moved")))
                    fail(("an instance would not move: " + moved.toStdString()).c_str());
                // ...AND IT ACTUALLY MOVED. The message saying "Moved" is the
                // report; this is the fact. A mutation that dropped the
                // translation passed every other check in this block.
                if (!anyInstanceAt(60.0))
                    fail("an instance reported a move and did not move");

                // GROUND it, and say so both ways.
                const QString grounded = window.toggleGroundSelectedInstance();
                if (!grounded.contains(QStringLiteral("grounded")))
                    fail(("Ground did not report grounding: " + grounded.toStdString()).c_str());
                const QString ungrounded = window.toggleGroundSelectedInstance();
                if (!ungrounded.contains(QStringLiteral("free")))
                    fail(("Unground did not report it: " + ungrounded.toStdString()).c_str());
                window.toggleGroundSelectedInstance(); // leave it grounded

                // PATTERN it, and hear the cost of changing it later.
                const QString patterned =
                    window.patternSelectedInstance(3, Vec3{0.0, 40.0, 0.0});
                if (!patterned.contains(QStringLiteral("delete them and pattern again")))
                    fail(("Pattern did not say what changing the count costs: " +
                          patterned.toStdString())
                             .c_str());
                if (window.instanceCountForTesting() != 5)
                    fail("a pattern of 3 on top of 3 instances did not leave 5");

                // SAVE, REOPEN, AND THE PLACES SURVIVED. This is the gate: an
                // assembly built by hand that comes back different is an
                // assembly nobody can build by hand.
                const QString rigPath =
                    QDir::tempPath() + QStringLiteral("/ep3d-selftest/m28-rig.ep3da");
                const QString saved = window.saveDocumentFile(rigPath);
                if (!saved.contains(QStringLiteral("Saved")))
                    fail(("the assembly would not save: " + saved.toStdString()).c_str());

                const std::vector<Vec3> before = window.instancePlacesForTesting();
                window.newDocumentCommand();
                const QString reopened = window.openDocumentFile(rigPath);
                if (reopened.contains(QStringLiteral("Could not open")))
                    fail(("the saved assembly would not reopen: " + reopened.toStdString())
                             .c_str());
                const std::vector<Vec3> after = window.instancePlacesForTesting();
                if (before.size() != after.size())
                    fail("the reopened assembly has a different number of instances");
                for (std::size_t i = 0; i < before.size(); ++i) {
                    if (std::fabs(before[i].x - after[i].x) > 1e-6 ||
                        std::fabs(before[i].y - after[i].y) > 1e-6 ||
                        std::fabs(before[i].z - after[i].z) > 1e-6)
                        fail("an instance came back in a different place than it was saved in");
                }

                // DELETE, on an instance NOTHING names: it takes only itself.
                window.selectFirstInstanceForTesting();
                const QString deleted = window.deleteSelectedInstance();
                if (!deleted.contains(QStringLiteral("Deleted")))
                    fail(("Delete did not delete: " + deleted.toStdString()).c_str());
                if (window.instanceCountForTesting() != after.size() - 1)
                    fail("Delete removed the wrong number of instances");
                if (deleted.contains(QStringLiteral("mate")))
                    fail(("Delete claimed to take mates that do not exist: " +
                          deleted.toStdString())
                             .c_str());
            }

            // --- DELETE SAYS WHAT ELSE IT TOOK ------------------------------
            //
            // A mate names two instances and cannot outlive either of them.
            // Finding that out afterwards -- from a tree with fewer rows than
            // expected -- is how a user learns not to trust Delete.
            //
            // The hinge is reopened rather than mated here, because building a
            // mate needs connectors and a mate dialog, and neither exists until
            // M29. Using the assembly the CLI already builds keeps this honest
            // about what the SHELL can currently do.
            {
                const QString hinge = QDir::tempPath() +
                                      QStringLiteral("/ep3d-selftest/m27-hinge.ep3da");
                const QString reopened = window.openDocumentFile(hinge);
                if (reopened.contains(QStringLiteral("Could not open")))
                    fail("the hinge would not reopen for the delete check");
                if (window.mateCountForTesting() != 1)
                    fail("the hinge came back without its mate");

                window.selectFirstInstanceForTesting();
                const QString deleted = window.deleteSelectedInstance();
                if (!deleted.contains(QStringLiteral("mate")))
                    fail(("Delete did not say it took the mate with it: " +
                          deleted.toStdString())
                             .c_str());
                if (window.mateCountForTesting() != 0)
                    fail("the mate outlived the instance it named");
            }

            // --- M29's GATE: mate a hinge with the SHELL, and turn it -------
            //
            // The parts come from examples/hinge.ep3ds, which defines their
            // connectors ON THE PARTS (§21). What is built here is the ASSEMBLY
            // side: insert, ground, mate, drive, limit -- through the command
            // methods, because the dialogs are the only part a self test cannot
            // work.
            {
                const QString base = QDir::tempPath() +
                                     QStringLiteral("/ep3d-selftest/m29-bracket.ep3d");
                const QString arm = QDir::tempPath() +
                                    QStringLiteral("/ep3d-selftest/m29-arm.ep3d");
                {
                    // TWO SEPARATE DOCUMENTS, one per file. `part` in the
                    // script language switches BACK to the same part document
                    // rather than starting a new one -- so writing both halves
                    // in one script gave the arm file the bracket's connector
                    // too, and the gate caught it.
                    const auto buildPart = [&](const QString& scriptName, const std::string& body,
                                               const std::string& connector,
                                               const QString& savePath) {
                        const QString script =
                            QDir::tempPath() + QStringLiteral("/ep3d-selftest/") + scriptName;
                        QFile out(script);
                        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                            fail("could not write one of M29's part scripts");
                        out.write(("sketch S\ntool rect\nclick -20 -10\nclick 44 10\n"
                                   "pad " + body + " 12 as " + body + "T\n" + connector +
                                   "\nsolve\nsave " + savePath.toStdString() + "\n").c_str());
                        out.close();
                        window.newDocumentCommand();
                        const QString made = window.runScriptFile(script);
                        if (made.contains(QStringLiteral("stopped")))
                            fail(("M29's part did not build: " + made.toStdString()).c_str());
                    };
                    // Their +Z SHARED, which is what makes a revolute a hinge
                    // rather than a random rotation (examples/hinge.ep3ds says
                    // the same).
                    buildPart(QStringLiteral("m29-bracket.ep3ds"), "Bracket",
                              "connector Pivot 0 0 12 0 0 1", base);
                    // OFF THE ARM'S OWN ORIGIN. A connector AT the origin means
                    // the hinge axis runs through it, so turning the arm moves
                    // nothing measurable -- which is how the first draft of
                    // this gate managed to assert a rotation that never showed.
                    buildPart(QStringLiteral("m29-arm.ep3ds"), "Arm",
                              "connector Eye -20 0 0 0 0 1", arm);
                }

                window.adoptAssemblyForTesting("M29Hinge");
                if (!window.insertInstanceCommand(base, QStringLiteral("Bracket"))
                         .contains(QStringLiteral("Inserted")))
                    fail("M29 could not insert the bracket");
                if (!window.insertInstanceCommand(arm, QStringLiteral("Arm"))
                         .contains(QStringLiteral("Inserted")))
                    fail("M29 could not insert the arm");

                const std::vector<ObjectId> made = window.allInstancesForTesting();
                if (made.size() != 2) fail("M29 did not end up with two instances");

                // WHAT EACH OFFERS TO BE MATED BY -- its part's connectors,
                // re-read from the part file (§21). The shell lists them; it
                // does not invent them.
                if (window.connectorsOfInstance(made[0]).size() != 1)
                    fail("the bracket instance does not offer its part's connector");
                if (window.connectorsOfInstance(made[1]).size() != 1)
                    fail("the arm instance does not offer its part's connector");

                // SOMETHING HAS TO BE FIXED, or a mate says where a thing is
                // relative to nothing.
                window.selectObject(made[0]);
                window.toggleGroundSelectedInstance();

                // A CONNECTOR THAT IS NOT THERE is refused BY NAME, before the
                // solve -- a mate that resolves to nothing fails later naming
                // an id, which is a message about the wrong thing.
                const QString wrong = window.createMateCommand(
                    MateType::Revolute, made[0], QStringLiteral("NoSuchConnector"), made[1],
                    QStringLiteral("Eye"));
                if (!wrong.contains(QStringLiteral("no connector called")))
                    fail(("a bad connector name was not refused by name: " +
                          wrong.toStdString())
                             .c_str());
                if (window.mateCountForTesting() != 0)
                    fail("a refused mate was created anyway");

                // ...AND THE REAL ONE.
                const QString mated = window.createMateCommand(
                    MateType::Revolute, made[0], QStringLiteral("Pivot"), made[1],
                    QStringLiteral("Eye"));
                if (!mated.contains(QStringLiteral("Mated")))
                    fail(("the hinge would not mate: " + mated.toStdString()).c_str());
                if (window.mateCountForTesting() != 1) fail("the mate was not created");

                // IT IS A HINGE, not a weld: the arm has exactly one freedom
                // left, and it turns. §20.3 asks for this PER INSTANCE.
                if (window.instanceFreedomForTesting(made[1]) != 1)
                    fail("a revolute mate did not leave the arm exactly one degree of freedom");

                // DRIVEN, and the arm actually moves. Asserting the message
                // alone is what M28's mutation run caught, so this asks the
                // geometry.
                const Vec3 atZero = window.instanceWorldPlaceForTesting(made[1]);
                const QString driven = window.driveSelectedMateForTesting(0.6);
                if (!driven.contains(QStringLiteral("driven")))
                    fail(("the mate would not drive: " + driven.toStdString()).c_str());
                const Vec3 turned = window.instanceWorldPlaceForTesting(made[1]);
                if (std::fabs(atZero.x - turned.x) < 1e-6 &&
                    std::fabs(atZero.y - turned.y) < 1e-6 &&
                    std::fabs(atZero.z - turned.z) < 1e-6)
                    fail("the mate reported driving and the arm did not move");

                // ...AND THE VALUE STICKS. A value set on a mate the solver is
                // still free to move is a value the next solve overwrites --
                // which is the defect examples/four-bar.ep3ds shipped with
                // (ADR-M25-006). Driving must MARK the mate driven, and the
                // only way to see the difference is to solve again and look.
                (void)window.recomputeForTesting();
                if (std::fabs(window.mateValueForTesting() - 0.6) > 1e-6)
                    fail("a driven mate did not keep its value across a solve");
                // THE FLAG, not only its consequence -- and that is a deliberate
                // weakening, said out loud. The consequence (a solve overwriting
                // the value) is only observable in a CLOSED LOOP, which is what
                // ADR-M25-006 is about; a single revolute off a grounded base
                // has nothing to overwrite it, so the value check above passes
                // either way. Asserting the flag pins the intent until this gate
                // grows a loop to show it in.
                if (!window.mateIsDrivenForTesting())
                    fail("driving a mate did not mark it driven, so a loop would overwrite it");

                // AN INSTANCE CANNOT BE MATED TO ITSELF. A mate holds two
                // things; one thing is not two, and the solve would be
                // trivially satisfied by doing nothing.
                const QString itself = window.createMateCommand(
                    MateType::Revolute, made[1], QStringLiteral("Eye"), made[1],
                    QStringLiteral("Eye"));
                if (!itself.contains(QStringLiteral("that is one")))
                    fail(("mating an instance to itself was allowed: " + itself.toStdString())
                             .c_str());

                // TWO SELECTED ROWS COME BACK IN DOCUMENT ORDER, whichever
                // order they were clicked in. Which instance is LEADING decides
                // which one the mate moves, and Qt does not keep click order --
                // so an answer that depended on it would place the wrong part
                // on alternate attempts.
                window.selectInstancesForTesting({made[1], made[0]});
                const std::vector<ObjectId> both = window.selectedInstancesForTesting();
                if (both.size() != 2) fail("two selected instances did not come back as two");
                if (both[0] != made[0] || both[1] != made[1])
                    fail("selected instances came back in click order, not document order");

                // LIMITED, AND CLAMPED RATHER THAN REFUSED (§22). Asking for
                // more than the stop allows leaves it AT the stop -- not an
                // error, and not a hinge bent past its stop.
                const QString limited = window.limitSelectedMateForTesting(0.0, 0.5);
                if (!limited.contains(QStringLiteral("held at the nearer end")))
                    fail(("Limit did not say what happens outside it: " +
                          limited.toStdString())
                             .c_str());
                window.driveSelectedMateForTesting(2.0);
                const Vec3 clamped = window.instanceWorldPlaceForTesting(made[1]);
                window.driveSelectedMateForTesting(0.5);
                const Vec3 atStop = window.instanceWorldPlaceForTesting(made[1]);
                if (std::fabs(clamped.x - atStop.x) > 1e-6 ||
                    std::fabs(clamped.y - atStop.y) > 1e-6 ||
                    std::fabs(clamped.z - atStop.z) > 1e-6)
                    fail("driving past the limit did not stop at the limit");

                // DELETING A MATE FREES ITS INSTANCES rather than taking them.
                window.selectFirstMateForTesting();
                const QString unmated = window.deleteSelectedMate();
                if (!unmated.contains(QStringLiteral("free again")))
                    fail(("deleting a mate did not say the instances survive: " +
                          unmated.toStdString())
                             .c_str());
                if (window.instanceCountForTesting() != 2)
                    fail("deleting a mate took its instances with it");
            }

            // --- M30's GATE: the three state mechanisms, kept three ---------
            //
            // §49 splits them because they capture three different KINDS of
            // thing: a named position is a geometric evaluation INPUT, an
            // exploded view is a derived PRESENTATION transform, and a display
            // state is what is hidden. What this gate checks is not that each
            // works, but that they stay apart -- that exploding does not move
            // the model, and hiding does not change it at all.
            //
            // The hinge from M29 is still open, mated and driveable.
            {
                const std::vector<ObjectId> made = window.allInstancesForTesting();
                if (made.size() != 2) fail("M30 expected M29's two instances");
                const ObjectId arm = made[1];

                // M29's block ends by DELETING its mate, to prove the instances
                // survive it -- so there is nothing here to drive until one is
                // put back. Caught by this gate on its first run, which is what
                // the "or this proves nothing" check below is for.
                if (window.mateCountForTesting() == 0)
                    window.createMateCommand(MateType::Revolute, made[0],
                                             QStringLiteral("Pivot"), made[1],
                                             QStringLiteral("Eye"));
                if (window.mateCountForTesting() != 1)
                    fail("M30 could not get a mate to drive");

                // --- NAMED POSITIONS ------------------------------------------
                window.selectFirstMateForTesting();
                window.driveSelectedMateForTesting(0.0);
                const Vec3 shut = window.instanceWorldPlaceForTesting(arm);
                if (!window.captureNamedPositionCommand(QStringLiteral("Shut"))
                         .contains(QStringLiteral("Captured")))
                    fail("a named position would not capture");

                window.driveSelectedMateForTesting(0.5);
                const Vec3 open = window.instanceWorldPlaceForTesting(arm);
                if (std::fabs(shut.x - open.x) < 1e-6 && std::fabs(shut.y - open.y) < 1e-6)
                    fail("driving the hinge did not move the arm, so this gate proves nothing");

                // APPLYING PUTS IT BACK, and in ONE undo step -- a position is
                // one thing the user chose, and undoing it a mate at a time
                // would stop somewhere that was never any position at all.
                window.selectNamedPositionForTesting(QStringLiteral("Shut"));
                const std::size_t undoBefore = window.undoDepthForTesting();
                if (!window.applySelectedNamedPosition().contains(QStringLiteral("Moved to")))
                    fail("a named position would not apply");
                if (window.undoDepthForTesting() != undoBefore + 1)
                    fail("applying a position was not one undo step");
                const Vec3 backAgain = window.instanceWorldPlaceForTesting(arm);
                if (std::fabs(backAgain.x - shut.x) > 1e-6 ||
                    std::fabs(backAgain.y - shut.y) > 1e-6)
                    fail("applying a named position did not put the assembly back");

                // --- EXPLODED VIEWS -------------------------------------------
                if (!window.addExplodeViewCommand(QStringLiteral("Apart"))
                         .contains(QStringLiteral("Added")))
                    fail("an exploded view would not be created");
                const ObjectId view = window.selectedExplodeViewForTesting();
                if (view == kInvalidObjectId) fail("the new exploded view was not selected");

                window.selectObject(arm);
                if (!window.addExplodeStepCommand(view, Vec3{0.0, 0.0, 50.0})
                         .contains(QStringLiteral("Added step")))
                    fail("an explode step would not be added");

                // THE MODEL IS UNTOUCHED BY THE PICTURE. This is §49's whole
                // point and the reason the three are separate: showing an
                // exploded view must not move anything.
                const Vec3 modelBefore = window.instanceWorldPlaceForTesting(arm);
                if (!window.showExplodeView(view).contains(QStringLiteral("picture")))
                    fail("showing an exploded view did not say it is a picture");
                const Vec3 modelAfter = window.instanceWorldPlaceForTesting(arm);
                if (std::fabs(modelBefore.z - modelAfter.z) > 1e-9)
                    fail("showing an exploded view MOVED the model");

                // ...AND THE DRAWING DID CHANGE, or the view is a no-op that
                // only claims to be a picture.
                const std::vector<DocumentPresenter::DisplayedShape> exploded =
                    presenter.displayableShapes();
                bool sawOffset = false;
                for (const auto& shape : exploded)
                    if (shape.id == arm &&
                        std::fabs(shape.placement.translation.z - modelAfter.z - 50.0) < 1e-6)
                        sawOffset = true;
                if (!sawOffset)
                    fail("the exploded view is shown but nothing is drawn moved");

                // ITS OWN ROLLBACK BAR (§49 point 2), and it CLAMPS -- a count
                // past the end means "all of it" rather than an error.
                if (!window.setExplodePreviewCommand(view, 0)
                         .contains(QStringLiteral("0 of 1")))
                    fail("the explode preview would not step back to nothing");
                const std::vector<DocumentPresenter::DisplayedShape> unexploded =
                    presenter.displayableShapes();
                for (const auto& shape : unexploded)
                    if (shape.id == arm &&
                        std::fabs(shape.placement.translation.z - modelAfter.z) > 1e-6)
                        fail("a preview of 0 steps still drew the explosion");
                if (!window.setExplodePreviewCommand(view, 99)
                         .contains(QStringLiteral("1 of 1")))
                    fail("a preview past the end was not clamped to all of it");

                window.showExplodeView(kInvalidObjectId);

                // --- DISPLAY STATE: presentation, and it stays out of Core ---
                const std::size_t undoBeforeHide = window.undoDepthForTesting();
                window.selectObject(arm);
                if (!window.showHideSelectedInstance().contains(QStringLiteral("hidden")))
                    fail("an instance would not hide");
                if (window.undoDepthForTesting() != undoBeforeHide)
                    fail("hiding recorded an undo step, so it reached the document");
                // STILL THERE AND STILL SOLVED -- hidden is not deleted.
                if (window.allInstancesForTesting().size() != 2)
                    fail("hiding an instance removed it");
                // ...and not drawn.
                for (const auto& shape : presenter.displayableShapes())
                    if (shape.id == arm) fail("a hidden instance is still being drawn");
                window.showHideSelectedInstance();

                // --- THE ASSEMBLY TOOLBAR (M30.2) -------------------------------
                //
                // Before this, an open assembly showed the MODEL toolbar --
                // seventeen greyed part buttons and no assembly buttons at all.
                // A menu can afford to be visible-and-disabled because it is
                // opened deliberately; a toolbar is always in view.
                {
                    const int buttons = window.assemblyToolbarButtonCount();
                    if (buttons < 10)
                        fail("the assembly toolbar is missing commands");
                    if (!window.assemblyToolbarVisible())
                        fail("an assembly does not show the assembly toolbar");
                    if (window.modelToolbarVisible())
                        fail("an assembly is still showing the part toolbar");

                    // DISTINCT icons, and none of them blank. "Every button has
                    // an icon" is satisfied by giving them all the same one,
                    // which is exactly what a copy-paste slip produces.
                    std::vector<unsigned long long> prints;
                    for (int i = 0; i < buttons; ++i) {
                        const unsigned long long print =
                            window.assemblyToolbarIconFingerprint(i);
                        if (print == 0) fail("an assembly toolbar icon rendered as nothing");
                        for (std::size_t j = 0; j < prints.size(); ++j)
                            if (prints[j] == print)
                                fail("two assembly toolbar buttons carry the SAME icon");
                        prints.push_back(print);
                    }

                    // NAMED, so a renamed or dropped button says which one.
                    for (const char* wanted : {"Insert", "Ground", "Mate", "Drive", "Limit",
                                               "Relation", "Pattern", "Position", "Explode",
                                               "Interference"}) {
                        bool found = false;
                        for (int i = 0; i < buttons; ++i)
                            if (window.assemblyToolbarLabel(i).find(wanted) != std::string::npos)
                                found = true;
                        if (!found) {
                            std::string message = "the assembly toolbar has no ";
                            message += wanted;
                            message += " button";
                            fail(message.c_str());
                        }
                    }
                }

                // --- INTERFERENCE ----------------------------------------------
                const QString interference = window.checkInterferenceCommand();
                if (interference.isEmpty())
                    fail("the interference check said nothing at all");
            }

            // --- M31's GATE: relations, on the screen -----------------------
            //
            // A relation couples two freedoms a mate solve would otherwise
            // choose independently (§20.5). What this asks is not that the
            // object exists but that the SECOND ARM MOVES when the first one
            // is driven: a relation stored and never applied is the shape this
            // milestone exists to avoid, and it would pass every check that
            // only reads messages.
            {
                const QString armFile = QDir::tempPath() +
                                        QStringLiteral("/ep3d-selftest/m29-arm.ep3d");
                const std::vector<ObjectId> made = window.allInstancesForTesting();
                if (made.size() != 2) fail("M31 expected M30's two instances");
                const ObjectId bracket = made[0];
                const ObjectId firstArm = made[1];

                // A SECOND ARM on the same bracket, so there are two hinges to
                // couple. Both turn about the bracket's one connector.
                if (!window.insertInstanceCommand(armFile, QStringLiteral("Arm"))
                         .contains(QStringLiteral("Inserted")))
                    fail("M31 could not insert a second arm");
                const std::vector<ObjectId> three = window.allInstancesForTesting();
                if (three.size() != 3) fail("M31 did not end up with three instances");
                const ObjectId secondArm = three[2];

                if (window.mateCountForTesting() == 0)
                    window.createMateCommand(MateType::Revolute, bracket,
                                             QStringLiteral("Pivot"), firstArm,
                                             QStringLiteral("Eye"));
                const QString secondMate = window.createMateCommand(
                    MateType::Revolute, bracket, QStringLiteral("Pivot"), secondArm,
                    QStringLiteral("Eye"));
                if (!secondMate.contains(QStringLiteral("Mated")))
                    fail(("M31 could not mate the second arm: " + secondMate.toStdString())
                             .c_str());
                const std::vector<ObjectId> mates = window.allMatesForTesting();
                if (mates.size() != 2) fail("M31 expected two mates to couple");
                const std::string driveName = window.objectNameForTesting(mates[0]);
                const std::string drivenName = window.objectNameForTesting(mates[1]);

                // WHERE THE SECOND ARM SITS AT TWICE THE ANGLE, measured by
                // driving it there BY HAND first. That is the place the gear
                // has to put it, and comparing against a number computed here
                // would only be checking this gate's own arithmetic.
                window.driveMateForTesting(mates[1], 0.6);
                const Vec3 atTwiceTheAngle = window.instanceWorldPlaceForTesting(secondArm);
                window.driveMateForTesting(mates[1], 0.0);
                const Vec3 atRest = window.instanceWorldPlaceForTesting(secondArm);
                if (std::fabs(atRest.x - atTwiceTheAngle.x) < 1e-6 &&
                    std::fabs(atRest.y - atTwiceTheAngle.y) < 1e-6)
                    fail("driving the second hinge did not move it, so this gate proves "
                         "nothing");

                // THE MENU OFFERS IT EXACTLY WHEN IT WOULD WORK: two mates
                // selected is a gear, nothing selected is neither.
                window.selectObject(kInvalidObjectId);
                if (window.addRelationEnabledForTesting())
                    fail("Add Relation is offered with nothing selected");
                window.selectInstancesForTesting({mates[0], mates[1]});
                if (!window.addRelationEnabledForTesting())
                    fail("Add Relation is not offered with two mates selected");

                // A GEAR, 2:1 -- and it RELEASES the by-hand drive on the end
                // it now decides, rather than leaving the tree claiming two
                // authorities for one number.
                const QString geared = window.createRelationForTesting(
                    RelationType::Gear, QString::fromStdString(driveName),
                    QString::fromStdString(drivenName), 2.0);
                if (!geared.contains(QStringLiteral("now drives")))
                    fail(("the gear would not be made: " + geared.toStdString()).c_str());
                if (!geared.contains(QStringLiteral("no longer driven by hand")))
                    fail(("a relation took over a hand-driven mate without saying so: " +
                          geared.toStdString())
                             .c_str());
                if (window.relationCountForTesting() != 1) fail("the relation was not created");

                // THE ARM ACTUALLY FOLLOWS. Half the angle on the driver puts
                // the driven arm exactly where driving it to 0.6 put it.
                window.driveMateForTesting(mates[0], 0.3);
                const Vec3 followed = window.instanceWorldPlaceForTesting(secondArm);
                if (std::fabs(followed.x - atTwiceTheAngle.x) > 1e-6 ||
                    std::fabs(followed.y - atTwiceTheAngle.y) > 1e-6 ||
                    std::fabs(followed.z - atTwiceTheAngle.z) > 1e-6)
                    fail("the gear reported a coupling and the second arm did not follow");

                // ...AND THE DRIVEN END IS NO LONGER THE USER'S TO SET.
                const QString refused = window.driveMateForTesting(mates[1], 0.9);
                if (!refused.contains(QStringLiteral("driven by the relation")))
                    fail(("driving a relation-owned freedom was not refused by name: " +
                          refused.toStdString())
                             .c_str());
                const Vec3 unmoved = window.instanceWorldPlaceForTesting(secondArm);
                if (std::fabs(unmoved.x - followed.x) > 1e-9 ||
                    std::fabs(unmoved.y - followed.y) > 1e-9)
                    fail("a refused drive moved the arm anyway");

                // THE TREE SHOWS IT, as its own kind. A relation that only
                // exists in the file is one nobody can select, rename or
                // delete.
                {
                    const OutlineNode tree = window.probeOutline();
                    std::size_t relationRows = 0;
                    std::string sentence;
                    const std::function<void(const OutlineNode&)> walk =
                        [&](const OutlineNode& node) {
                            if (node.kind == OutlineKind::Relation) {
                                ++relationRows;
                                sentence = node.diagnostic;
                            }
                            for (const OutlineNode& child : node.children) walk(child);
                        };
                    walk(tree);
                    if (relationRows != 1)
                        fail("the assembly tree does not show its relation");
                    // WHAT IT DOES, in the unit it was typed in. A row showing
                    // only a name tells a reader nothing they could not guess.
                    if (sentence.find("2.000") == std::string::npos)
                        fail(("the relation row does not say its ratio: " + sentence).c_str());
                    // ...AND IT REACHED THE WIDGET. The outline saying it and
                    // the tree showing it are two claims, and a State column
                    // nothing ever read is where the first one goes to die.
                    const std::string shown = window.treeStateFor("[Rel] ");
                    if (shown.find("2.000") == std::string::npos)
                        fail(("the tree's State column does not show the ratio: " + shown)
                                 .c_str());
                }

                // THE PANEL, asked of the WIDGET -- and it has to name BOTH
                // ends, or a gear cannot be told from the gear next to it.
                const std::vector<std::string> relationNames = window.relationNamesForTesting();
                if (relationNames.size() != 1) fail("M31 expected one relation to inspect");
                window.selectRelationForTesting(QString::fromStdString(relationNames.front()));
                if (window.propertyRowValue("Ratio").find("2.000") == std::string::npos)
                    fail(("a selected relation shows no ratio: " +
                          window.propertyRowValue("Ratio"))
                             .c_str());
                if (window.propertyRowValue("Driven by").find(driveName) != 0)
                    fail("the panel does not say which mate drives the relation");
                if (window.propertyRowValue("Drives").find(drivenName) != 0)
                    fail("the panel does not say which mate the relation drives");

                // A PICTURE OF THE ASSEMBLY SHELL, next to the one the run
                // already takes of the part shell.
                //
                // The existing screenshot is written a long way above this,
                // before any assembly is open, so every picture this project
                // has ever taken is of a PART -- which is how "[Part]" on an
                // assembly root and a greyed part toolbar both survived until
                // somebody opened the program by hand. A second file, beside
                // the first, so the part picture keeps meaning what it did.
                if (screenshotPath != nullptr) {
                    std::string beside = screenshotPath;
                    const std::size_t dot = beside.rfind('.');
                    beside = dot == std::string::npos ? beside + "-assembly"
                                                      : beside.substr(0, dot) + "-assembly" +
                                                            beside.substr(dot);
                    if (!shoot(window, QString::fromStdString(beside)))
                        fail("could not write the assembly screenshot");
                }

                // REVERSED, and the arm goes the OTHER way.
                if (!window.reverseSelectedRelation().contains(QStringLiteral("the other way")))
                    fail("a relation would not reverse");
                const Vec3 backwards = window.instanceWorldPlaceForTesting(secondArm);
                if (std::fabs(backwards.x - followed.x) < 1e-6 &&
                    std::fabs(backwards.y - followed.y) < 1e-6)
                    fail("reversing a gear did not turn the arm the other way");

                // DELETED, and the freedom goes back to the solve -- the
                // opposite of deleting a MATE, and the message says so.
                const QString gone = window.deleteSelectedRelation();
                if (!gone.contains(QStringLiteral("solve's again")))
                    fail(("deleting a relation did not say what happens to the freedom: " +
                          gone.toStdString())
                             .c_str());
                if (window.relationCountForTesting() != 0)
                    fail("the relation was not deleted");
                if (window.allMatesForTesting().size() != 2)
                    fail("deleting a relation took its mates with it");

                // ...AND DELETING A MATE TAKES ITS RELATIONS. The other
                // direction, on the screen: a relation reads a mate's freedom,
                // so it cannot outlive it.
                window.createRelationForTesting(RelationType::Gear,
                                                QString::fromStdString(driveName),
                                                QString::fromStdString(drivenName), 2.0);
                if (window.relationCountForTesting() != 1)
                    fail("M31 could not re-make the relation");
                window.selectObject(mates[1]);
                window.deleteSelectedMate();
                if (window.relationCountForTesting() != 0)
                    fail("a relation outlived the mate whose freedom it reads");
            }

            // --- M32's GATE: a drawing, on the screen ------------------------
            //
            // Everything below is reachable only here. The Core suite proves a
            // drawing is a DOCUMENT and the kernel suite proves a view
            // PROJECTS; nothing but starting the program can prove that the
            // paper is drawn, that the curves reach the canvas, and that the
            // shell swaps to the right bar and the right page.
            {
                const QString partFile =
                    QDir::tempPath() + QStringLiteral("/ep3d-selftest/m32-block.ep3d");
                {
                    const QString script =
                        QDir::tempPath() + QStringLiteral("/ep3d-selftest/m32-block.ep3ds");
                    QFile out(script);
                    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                        fail("could not write M32's part script");
                    // ...WITH HOLES IN IT (M39). A block with none makes the
                    // hole-table gate below assert nothing at all: an empty
                    // table has as many rows as tags, and "0 == 0" is a check
                    // that can never fail. ADR-M26-009 in the small -- a
                    // fingerprint that always matches is not evidence.
                    out.write(("sketch S\ntool rect\nclick 0 0\nclick 80 30\n"
                               "pad Block 20 as BlockT\n"
                               "sketch H\ntool point\nclick 15 8\nclick 65 8\n"
                               "click 15 22\n"
                               "hole Block H 6.6\nsolve\nsave " +
                               partFile.toStdString() + "\n")
                                  .c_str());
                    out.close();
                    window.newDocumentCommand();
                    const QString made = window.runScriptFile(script);
                    if (made.contains(QStringLiteral("stopped")))
                        fail(("M32's part did not build: " + made.toStdString()).c_str());
                }

                window.adoptDrawingForTesting("M32Sheet");
                if (window.openedDocumentType() != DocumentType::Drawing)
                    fail("the window did not adopt a drawing");
                if (!window.drawingCanvasVisibleForTesting())
                    fail("a drawing is open and the 3D view is still on screen");
                if (!window.drawingToolbarVisible())
                    fail("a drawing does not show the drawing toolbar");
                if (window.modelToolbarVisible() || window.assemblyToolbarVisible())
                    fail("a drawing is showing another document type's toolbar");

                // A VIEW OF A REAL FILE, and it has to DRAW.
                const QString added = window.addBaseViewCommand(
                    partFile, QStringLiteral("Block"), ViewDirection::Front, Vec2{150.0, 150.0});
                if (!added.contains(QStringLiteral("curves")))
                    fail(("the base view would not draw: " + added.toStdString()).c_str());
                if (window.drawingViewCountForTesting() != 1)
                    fail("the view was not created");

                // ...AND THE CURVES REACHED THE CANVAS. "The document holds
                // them" and "they are on screen" are two claims, and the gap
                // between them is the M6.14 defect class this shell exists to
                // catch.
                window.repaint();
                if (window.drawnCurveCountForTesting() == 0)
                    fail("the view holds curves and the canvas drew none of them");

                // A PROJECTED VIEW, off the selected one, LINED UP.
                window.selectDrawingViewForTesting(QStringLiteral("Front"));
                const QString projected =
                    window.addProjectedViewCommand(ViewDirection::Top, 70.0);
                if (!projected.contains(QStringLiteral("angle")))
                    fail(("the projected view did not say which convention it used: " +
                          projected.toStdString())
                             .c_str());
                if (window.drawingViewCountForTesting() != 2)
                    fail("the projected view was not created");

                // THE TREE SHOWS THEM, NESTED -- a projected view IS under the
                // one it came from, and a flat list would hide the
                // relationship that decides where half the drawing sits.
                {
                    const OutlineNode tree = window.probeOutline();
                    std::size_t viewRows = 0;
                    std::size_t nested = 0;
                    const std::function<void(const OutlineNode&, bool)> walk =
                        [&](const OutlineNode& node, bool underView) {
                            if (node.kind == OutlineKind::DrawingView) {
                                ++viewRows;
                                if (underView) ++nested;
                            }
                            for (const OutlineNode& child : node.children)
                                walk(child, node.kind == OutlineKind::DrawingView);
                        };
                    walk(tree, false);
                    if (viewRows != 2) fail("the drawing tree does not show its two views");
                    if (nested != 1)
                        fail("the projected view is not shown under the one it came from");
                    if (tree.kind != OutlineKind::Drawing)
                        fail("the drawing's root row does not say it is a drawing");
                }

                // THE SHEET ANSWERS ON THE ROOT ROW. Selecting it is how a
                // user asks "what size is this paper", and a root that showed
                // nothing would send them hunting for a dialog.
                window.selectObject(window.probeOutline().id);
                if (window.propertyRowValue("Size").empty())
                    fail("selecting the sheet says nothing about the paper");

                // SHEET SETUP IS ONE UNDO STEP for what was one dialog.
                const std::size_t undoBefore = window.undoDepthForTesting();
                const QString sheet = window.setSheetCommand(SheetSize::A2,
                                                             SheetOrientation::Portrait,
                                                             QStringLiteral("1:2"),
                                                             ProjectionAngle::Third);
                if (!sheet.contains(QStringLiteral("A2")))
                    fail(("the sheet command said nothing useful: " + sheet.toStdString())
                             .c_str());
                if (window.undoDepthForTesting() != undoBefore + 1)
                    fail("a sheet setup was more than one undo step");

                // --- THE DRAWING TOOLBAR ----------------------------------------
                {
                    const int buttons = window.drawingToolbarButtonCount();
                    if (buttons < 5) fail("the drawing toolbar is missing commands");
                    std::vector<unsigned long long> prints;
                    for (int i = 0; i < buttons; ++i) {
                        const unsigned long long print =
                            window.drawingToolbarIconFingerprint(i);
                        if (print == 0) fail("a drawing toolbar icon rendered as nothing");
                        for (std::size_t j = 0; j < prints.size(); ++j)
                            if (prints[j] == print)
                                fail("two drawing toolbar buttons carry the SAME icon");
                        prints.push_back(print);
                    }
                    for (const char* wanted :
                         {"View", "Project", "Update", "Sheet", "Layer"}) {
                        bool found = false;
                        for (int i = 0; i < buttons; ++i)
                            if (window.drawingToolbarLabel(i).find(wanted) != std::string::npos)
                                found = true;
                        if (!found) {
                            std::string message = "the drawing toolbar has no ";
                            message += wanted;
                            message += " button";
                            fail(message.c_str());
                        }
                    }
                }

                // A PICTURE OF THE DRAWING SHELL, beside the part and assembly
                // ones -- for the reason M31 added the second: every
                // screenshot this project takes should be of a shell somebody
                // will actually look at.
                if (screenshotPath != nullptr) {
                    std::string beside = screenshotPath;
                    const std::size_t dot = beside.rfind('.');
                    beside = dot == std::string::npos
                                 ? beside + "-drawing"
                                 : beside.substr(0, dot) + "-drawing" + beside.substr(dot);
                    if (!shoot(window, QString::fromStdString(beside)))
                        fail("could not write the drawing screenshot");
                }

                // --- M33/M34's GATE: geometry and dimensions on the paper ----
                //
                // Everything here is reachable only by starting the program.
                // The Core suite proves a dimension MEASURES; nothing but this
                // can prove that a user can put one on the paper, that it
                // reaches the canvas, and that it says the same thing on the
                // screen as it does in the document.
                {
                    // A RECTANGLE, DRAWN THE WAY A USER WOULD: arm the tool,
                    // click twice. Not by calling addEntity -- the point of
                    // this gate is the path a hand takes.
                    window.setDrawingToolCommand(DrawingTool::Rectangle);
                    if (window.drawingToolForTesting() != DrawingTool::Rectangle)
                        fail("the rectangle tool did not arm");
                    window.pickOnSheetForTesting(Vec2{40.0, 40.0});
                    window.pickOnSheetForTesting(Vec2{140.0, 100.0});
                    if (window.drawingEntityCountForTesting() != 4)
                        fail("drawing a rectangle did not put four lines on the sheet");
                    // ...AND THE TOOL DISARMED. A tool that stayed armed is
                    // how a user ends up with nine rectangles.
                    if (window.drawingToolForTesting() != DrawingTool::None)
                        fail("the rectangle tool stayed armed after drawing one");
                    // ONE GESTURE, ONE UNDO STEP -- four lines came from two
                    // clicks, and four Ctrl+Z would contradict what the user
                    // did.
                    const std::size_t afterRect = window.undoDepthForTesting();
                    window.undoCommand();
                    if (window.drawingEntityCountForTesting() != 0)
                        fail("undoing a rectangle left some of its lines behind");
                    window.redoCommand();
                    if (window.drawingEntityCountForTesting() != 4 ||
                        window.undoDepthForTesting() != afterRect)
                        fail("redoing a rectangle did not put it back in one step");

                    // A CIRCLE, so there is something round to put a diameter
                    // on.
                    window.setDrawingToolCommand(DrawingTool::Circle);
                    window.pickOnSheetForTesting(Vec2{90.0, 70.0});
                    window.pickOnSheetForTesting(Vec2{110.0, 70.0});
                    if (window.drawingEntityCountForTesting() != 5)
                        fail("the circle tool drew nothing");

                    // --- A DIMENSION ON THE BOTTOM EDGE ----------------------
                    window.selectObject(window.drawingEntityIdForTesting(0));
                    const QString dimensioned =
                        window.addDimensionCommand(DimensionKind::Linear,
                                                   LinearDirection::Horizontal);
                    if (window.dimensionCountForTesting() != 1)
                        fail(("the dimension was not created: " + dimensioned.toStdString())
                                 .c_str());
                    // IT SAYS THE SIZE OF THE LINE, not of the picture of it.
                    if (!dimensioned.contains(QStringLiteral("100.00")))
                        fail(("a 100 mm line was dimensioned and it reads: " +
                              dimensioned.toStdString())
                                 .c_str());

                    // ...AND IT REACHED THE CANVAS. "The document holds it"
                    // and "it is on screen" are two claims -- the M6.14 gap
                    // this shell exists to catch.
                    window.repaint();
                    if (window.drawnDimensionCountForTesting() != 1)
                        fail("the drawing holds a dimension and the canvas drew none");

                    // A DIAMETER ON THE CIRCLE, which is a different rule
                    // about which snap points a pick means -- and it goes
                    // through the same proposal, so this proves the one rule
                    // covers both.
                    window.selectObject(window.drawingEntityIdForTesting(4));
                    const QString round =
                        window.addDimensionCommand(DimensionKind::Diameter);
                    if (window.dimensionCountForTesting() != 2)
                        fail(("the diameter was refused: " + round.toStdString()).c_str());
                    if (!round.contains(QStringLiteral("40.00")))
                        fail(("a radius-20 circle reads: " + round.toStdString()).c_str());

                    // A DIAMETER ON A LINE IS REFUSED, LOUDLY. Inventing an
                    // answer would put a number on the drawing that measures
                    // something the user did not point at.
                    window.selectObject(window.drawingEntityIdForTesting(0));
                    const QString nonsense =
                        window.addDimensionCommand(DimensionKind::Diameter);
                    if (window.dimensionCountForTesting() != 2)
                        fail("a diameter was invented for a straight line");
                    if (!nonsense.contains(QStringLiteral("No dimension")))
                        fail(("refusing a diameter on a line said nothing: " +
                              nonsense.toStdString())
                                 .c_str());

                    // --- IT FOLLOWS THE GEOMETRY ------------------------------
                    //
                    // The whole point of the module. A dimension that stored
                    // its number would keep reading 100 after the line became
                    // 60 long, and it would look completely right.
                    const ObjectId edge = window.drawingEntityIdForTesting(0);
                    const ObjectId theDimension = window.dimensionIdForTesting(0);
                    window.scaleEntityForTesting(edge, Vec2{40.0, 40.0}, 0.6);
                    if (window.dimensionTextForTesting(theDimension) !=
                        QStringLiteral("60.00"))
                        fail(("the line became 60 long and the dimension reads " +
                              window.dimensionTextForTesting(theDimension).toStdString())
                                 .c_str());

                    // --- AND IT DANGLES LOUDLY WHEN IT LOSES IT ---------------
                    window.selectObject(edge);
                    window.deleteSelectedDrawingObject();
                    if (window.danglingDimensionCountForTesting() != 1)
                        fail("the line a dimension measured was deleted and nothing dangled");
                    if (window.dimensionTextForTesting(theDimension) !=
                        QStringLiteral("<?>"))
                        fail("a dangling dimension is still showing a number");
                    window.repaint();
                    if (window.danglingDrawnForTesting() != 1)
                        fail("a dimension is dangling and the canvas is not saying so");
                    // THE TREE SAYS SO TOO. A reader who notices the paper
                    // looks wrong goes to the tree, and a healthy-looking root
                    // over a dangling dimension is the tree lying.
                    if (window.probeOutline().state != OutlineState::Failed)
                        fail("a dimension is dangling and the drawing's root row looks fine");
                    window.undoCommand();
                    if (window.danglingDimensionCountForTesting() != 0)
                        fail("undoing the delete did not put the dimension back on its line");

                    // --- RESTYLING REACHES EVERY DIMENSION --------------------
                    window.selectObject(kInvalidObjectId);
                    window.editDimensionStyleCommand(5.0, 5.0, 0, QStringLiteral(" mm"));
                    if (window.dimensionTextForTesting(theDimension) !=
                        QStringLiteral("60 mm"))
                        fail(("restyling did not reach the dimension, which reads " +
                              window.dimensionTextForTesting(theDimension).toStdString())
                                 .c_str());

                    // --- AN OVERRIDE CHANGES THE TEXT, NEVER THE MEASUREMENT --
                    window.selectObject(theDimension);
                    const QString said = window.setDimensionTextCommand(QStringLiteral("2x TYP"));
                    if (window.dimensionTextForTesting(theDimension) !=
                        QStringLiteral("2x TYP"))
                        fail("the override did not reach the drawing");
                    if (!said.contains(QStringLiteral("still measures 60")))
                        fail(("overriding did not say what it still measures: " +
                              said.toStdString())
                                 .c_str());
                    window.setDimensionTextCommand(QString());

                    // --- M37's GATE: tolerances ------------------------------
                    //
                    // The Core suite proves the ISO tables and that a fit
                    // derives its numbers. Only starting the program can prove
                    // a user can put one on a dimension and that it reaches
                    // the paper.
                    {
                        const ObjectId sized = window.dimensionIdForTesting(0);
                        // A SYMMETRIC TOLERANCE, and the size still reads the
                        // size: a tolerance that replaced the number would be
                        // a drawing that had stopped stating what it measures.
                        const QString applied = window.setDimensionToleranceCommand(
                            ToleranceKind::Symmetric, 0.1, -0.1);
                        if (!applied.contains(QStringLiteral("60")))
                            fail(("the tolerance replaced the size: " +
                                  applied.toStdString())
                                     .c_str());
                        if (window.dimensionToleranceTextForTesting(sized).isEmpty())
                            fail("the tolerance did not reach the dimension");

                        // A FIT DERIVES ITS NUMBERS FROM THE SIZE. The line is
                        // 60 long, so H7 is +0.030/0 -- and if it were still
                        // reading the 100 it was before the scale, it would
                        // say +0.035.
                        window.setDimensionToleranceCommand(ToleranceKind::Fit, 0.0, 0.0,
                                                            QStringLiteral("H7"));
                        const QString fit =
                            window.dimensionToleranceTextForTesting(sized);
                        if (!fit.contains(QStringLiteral("H7")))
                            fail(("the fit did not reach the dimension: " +
                                  fit.toStdString())
                                     .c_str());
                        // THE VALUE, not the digit count. 0.03 and 0.030 are
                        // the same number, and asserting the second would be
                        // testing the formatting rather than the fit.
                        if (!fit.contains(QStringLiteral("0.03")))
                            fail(("H7 at 60 mm came out as " + fit.toStdString() +
                                  ", and the published value is 0.030")
                                     .c_str());

                        // A FIT THIS BUILD CANNOT WORK OUT IS REFUSED, and
                        // says which one -- "refused" alone would send a user
                        // hunting through their own typing.
                        const QString refused = window.setDimensionToleranceCommand(
                            ToleranceKind::Fit, 0.0, 0.0, QStringLiteral("J7"));
                        if (!refused.contains(QStringLiteral("J7")))
                            fail(("refusing an unknown fit said: " + refused.toStdString())
                                     .c_str());
                        if (!window.dimensionToleranceTextForTesting(sized).contains(
                                QStringLiteral("H7")))
                            fail("a refused fit replaced the one that was there");

                        // THE SHEET'S GENERAL CLASS, printed once.
                        window.setGeneralToleranceCommand(GeneralToleranceClass::Medium);
                        if (window.generalToleranceNoteForTesting() !=
                            QStringLiteral("ISO 2768-m"))
                            fail("the general tolerance note is not what the sheet says");

                        // IT ALL REACHED THE CANVAS.
                        window.repaintDrawingForTesting();
                        if (window.drawnDimensionCountForTesting() == 0)
                            fail("the toleranced dimensions stopped being drawn");
                    }

                    // M38's GATE RUNS AFTER M37's, and the order is not
                    // arbitrary: making a section selects the new view, and
                    // the tolerance checks need a DIMENSION selected. Put
                    // first, it left them asking a drawing with nothing chosen
                    // and they failed with "select a dimension first" -- which
                    // reads as a broken command rather than a test in the
                    // wrong order.
                    // --- M38's GATE: a section view --------------------------
                    //
                    // The kernel suite proves the knife cuts and the Core
                    // suite proves the hatch fills. Only starting the program
                    // can prove a user can make a section and that the hatch,
                    // the cut line and the arrows reach the paper.
                    {
                        window.selectDrawingViewForTesting(QStringLiteral("Top"));
                        const QString made = window.addSectionViewCommand(
                            QStringLiteral("SectionA"), Vec2{40.0, -30.0}, Vec2{40.0, 30.0},
                            1, 90.0);
                        if (!made.contains(QStringLiteral("A-A")))
                            fail(("the section was not made: " + made.toStdString()).c_str());
                        if (made.contains(QStringLiteral("would not cut")))
                            fail(("the section would not cut: " + made.toStdString()).c_str());

                        window.repaintDrawingForTesting();
                        // THE HATCH. Without it a section is a drawing of the
                        // inside of a part with no way to tell cut material
                        // from what is behind it.
                        if (window.drawnHatchLinesForTesting() == 0)
                            fail("the section drew no hatch, so nothing says where the "
                                 "knife went");
                        if (window.unhatchedSectionsForTesting() != 0)
                            fail("a section's cut face could not be hatched");
                        // THE CUT LINE ON THE PARENT, with an arrow at each
                        // end. A section with no line on its parent is one a
                        // reader cannot locate.
                        if (window.drawnSectionArrowsForTesting() < 2)
                            fail("the cut line was drawn without arrows at both ends");

                        // A PICTURE OF A SECTION. Whether a section READS --
                        // whether the hatch is dense enough, whether the cut
                        // line is findable, whether A-A sits where a reader
                        // looks -- is a judgement no assertion makes.
                        if (screenshotPath != nullptr) {
                            std::string beside = screenshotPath;
                            const std::size_t dot = beside.rfind('.');
                            beside = dot == std::string::npos
                                         ? beside + "-section"
                                         : beside.substr(0, dot) + "-section" +
                                               beside.substr(dot);
                            if (!shoot(window, QString::fromStdString(beside)))
                                fail("could not write the section screenshot");
                        }
                    }

                    // --- M39's GATE: a hole table on the paper ----------------
                    //
                    // The Core suite proves the rows are counted right and the
                    // kernel suite proves the holes come out the size the
                    // standard says. Only starting the program can prove a
                    // user can put the table on a sheet and that the TAGS
                    // reach the view -- and it is the pair that matters: rows
                    // nobody can find on the drawing describe an unmarked
                    // part.
                    {
                        window.selectDrawingViewForTesting(QStringLiteral("Top"));
                        const QString made = window.addHoleTableCommand(
                            // CLEAR OF EVERYTHING ELSE. The first run put it
                            // over the circle and the 60 mm dimension in the
                            // corner -- legible in the tally, unreadable on
                            // the paper, which is the whole reason a
                            // screenshot is taken as well as a count.
                            QStringLiteral("Holes"), Vec2{30.0, 390.0}, Vec2{0.0, 0.0});
                        if (made.contains(QStringLiteral("refused")))
                            fail(("the hole table was refused: " + made.toStdString()).c_str());
                        if (made.contains(QStringLiteral("could not be read")))
                            fail(("the hole table could not read its part: " +
                                  made.toStdString())
                                     .c_str());

                        window.repaintDrawingForTesting();
                        if (window.uncountedHoleTablesForTesting() != 0)
                            fail("a hole table on the sheet could not count its part");
                        // EVERY ROW HAS A TAG AND EVERY TAG HAS A ROW. Counted
                        // apart on purpose: either number alone looks healthy
                        // when the two disagree, and what the reader gets is a
                        // hole described in a table and marked nowhere.
                        if (window.drawnHoleRowsForTesting() !=
                            window.drawnHoleTagsForTesting())
                            fail("the hole table drew a different number of rows and tags");
                        // AND THERE HAS TO BE SOMETHING IN IT. Without this the
                        // check above is "0 == 0" -- a fingerprint that always
                        // matches, which is not evidence of anything
                        // (ADR-M26-009). The part this view is of has three
                        // holes drilled into it.
                        if (window.drawnHoleRowsForTesting() != 3)
                            fail("the hole table did not draw the part's three holes");

                        // A PICTURE OF IT. Whether a hole table READS -- the
                        // columns wide enough for their callouts, the tags
                        // findable beside the holes, the rows lined up --
                        // is a judgement no assertion makes (ADR-M26-009).
                        if (screenshotPath != nullptr) {
                            std::string beside = screenshotPath;
                            const std::size_t dot = beside.rfind('.');
                            beside = dot == std::string::npos
                                         ? beside + "-holes"
                                         : beside.substr(0, dot) + "-holes" +
                                               beside.substr(dot);
                            if (!shoot(window, QString::fromStdString(beside)))
                                fail("could not write the hole table screenshot");
                        }
                    }

                    // --- M40's GATE: the editing tools -----------------------
                    //
                    // Every one of these does something PLAUSIBLE to the wrong
                    // piece when it is wrong, so what the gate checks is not
                    // that a command succeeded: it is that the count of things
                    // on the sheet moved the way the tool says it does.
                    {
                        window.setDrawingToolCommand(DrawingTool::Line);
                        window.pickOnSheetForTesting(Vec2{20.0, 40.0});
                        window.pickOnSheetForTesting(Vec2{120.0, 40.0});
                        const std::size_t afterFirst = window.drawingEntityCountForTesting();
                        const ObjectId across =
                            window.drawingEntityIdForTesting(afterFirst - 1);
                        window.setDrawingToolCommand(DrawingTool::Line);
                        window.pickOnSheetForTesting(Vec2{70.0, 10.0});
                        window.pickOnSheetForTesting(Vec2{70.0, 70.0});
                        const std::size_t afterBoth = window.drawingEntityCountForTesting();
                        const ObjectId upright = window.drawingEntityIdForTesting(afterBoth - 1);
                        if (afterBoth != afterFirst + 1)
                            fail("the gate could not draw the two lines it needs");

                        // TRIM: cut the long line where the upright crosses it
                        // and throw away the piece the pick is on. One line in,
                        // one line out -- and the count does not move.
                        const QString trimmed = window.sheetEditCommand(
                            QStringLiteral("Trim"), {upright, across}, Vec2{100.0, 40.0}, 0.0,
                            0.0, 0);
                        if (!trimmed.contains(QStringLiteral("1 object")))
                            fail(("trim did not leave one line: " + trimmed.toStdString())
                                     .c_str());
                        if (window.drawingEntityCountForTesting() != afterBoth)
                            fail("trim did not replace the line it cut");

                        // OFFSET KEEPS ITS ORIGINAL: it is a copy, not a move,
                        // which is the difference between offsetting a wall and
                        // dragging it.
                        const QString offset = window.sheetEditCommand(
                            QStringLiteral("Offset"), {upright}, Vec2{90.0, 40.0}, 10.0, 0.0,
                            0);
                        if (!offset.contains(QStringLiteral("1 object")))
                            fail(("offset did not make a copy: " + offset.toStdString())
                                     .c_str());
                        if (window.drawingEntityCountForTesting() != afterBoth + 1)
                            fail("offset consumed the line it was copying");

                        // ARRAY INCLUDES THE ORIGINAL as its first copy, so
                        // four across replaces one line with four.
                        const QString arrayed = window.sheetEditCommand(
                            QStringLiteral("Array"), {upright}, Vec2{}, 25.0, 0.0, 4);
                        if (!arrayed.contains(QStringLiteral("4 object")))
                            fail(("array did not make four: " + arrayed.toStdString()).c_str());
                        const std::size_t afterArray = window.drawingEntityCountForTesting();
                        if (afterArray != afterBoth + 4)
                            fail("array did not replace its original with four copies");

                        // ...AND ONE UNDO TAKES THE WHOLE EDIT BACK. Half-
                        // undone, an array leaves three strangers on the sheet
                        // and the original still missing.
                        window.undoCommand();
                        if (window.drawingEntityCountForTesting() != afterBoth + 1)
                            fail("undoing an array did not take all of it back");
                        window.redoCommand();
                        if (window.drawingEntityCountForTesting() != afterArray)
                            fail("redoing an array did not put it back in one step");
                    }

                    // --- THE TOOLBAR CARRIES THEM -----------------------------
                    for (const char* wanted : {"Line", "Circle", "Rect", "Dim", "Dia",
                                               "Angle", "Style", "Title"}) {
                        bool found = false;
                        const int buttons = window.drawingToolbarButtonCount();
                        for (int i = 0; i < buttons; ++i)
                            if (window.drawingToolbarLabel(i).find(wanted) !=
                                std::string::npos)
                                found = true;
                        if (!found) {
                            std::string message = "the drawing toolbar has no ";
                            message += wanted;
                            message += " button";
                            fail(message.c_str());
                        }
                    }

                    // --- M35's GATE: the frame and the title block -----------
                    //
                    // The Core suite proves the frame is DERIVED and the block
                    // cannot be typed into. Only starting the program can
                    // prove they reach the paper.
                    if (window.drawnFrameLinesForTesting() == 0)
                        fail("the drawing has a frame and the canvas drew none of it");
                    if (window.drawnTitleBlockRowsForTesting() == 0)
                        fail("the drawing has a title block and the canvas drew no rows");

                    // WHAT IT SAYS IS WHAT THE SHEET SAYS. The sheet was set
                    // to A2 at 1:2 in third angle further up; the corner has to
                    // agree, and there is no code path that could make it
                    // disagree.
                    if (window.titleBlockValueForTesting(QStringLiteral("Scale")) !=
                        QStringLiteral("1:2"))
                        fail(("the title block says the scale is " +
                              window.titleBlockValueForTesting(QStringLiteral("Scale"))
                                  .toStdString() +
                              " and the sheet is at 1:2")
                                 .c_str());
                    if (window.titleBlockValueForTesting(QStringLiteral("Size")) !=
                        QStringLiteral("A2"))
                        fail("the title block and the sheet disagree about the paper size");

                    // TYPING INTO A DERIVED ROW IS REFUSED, AND SAYS SO. A
                    // dialog that quietly discarded what was typed would teach
                    // the user the program ignores them.
                    const QString refused =
                        window.setTitleBlockFieldCommand(QStringLiteral("Scale"),
                                                         QStringLiteral("1:99"));
                    if (!refused.contains(QStringLiteral("from the sheet")))
                        fail(("typing a scale into the title block said: " +
                              refused.toStdString())
                                 .c_str());
                    if (window.titleBlockValueForTesting(QStringLiteral("Scale")) !=
                        QStringLiteral("1:2"))
                        fail("a scale was typed into the title block after all");

                    // ...and a TYPED row takes what it is given, and shows it.
                    window.setTitleBlockFieldCommand(QStringLiteral("Title"),
                                                     QStringLiteral("Bearing Housing"));
                    if (window.titleBlockValueForTesting(QStringLiteral("Title")) !=
                        QStringLiteral("Bearing Housing"))
                        fail("the title did not reach the title block");

                    // THE FRAME FOLLOWS THE PAPER. Resizing the sheet has to
                    // move the border and the block with it -- a frame made of
                    // entities would stay A2 sized on an A4 sheet and look
                    // completely plausible.
                    {
                        const OutlineNode before = window.probeOutline();
                        (void)before;
                        window.setSheetCommand(SheetSize::A4, SheetOrientation::Portrait,
                                               QStringLiteral("1:2"), ProjectionAngle::Third);
                        window.repaint();
                        if (window.drawnFrameLinesForTesting() == 0)
                            fail("the sheet was resized and the frame stopped drawing");
                        if (window.titleBlockValueForTesting(QStringLiteral("Size")) !=
                            QStringLiteral("A4"))
                            fail("the sheet is A4 and its title block still says otherwise");
                        // ...and back, so the screenshot below is of the sheet
                        // the rest of this gate built.
                        window.undoCommand();
                        window.repaint();
                    }

                    // TURNING THE FRAME OFF TURNS IT OFF (M35-22). A
                    // "hidden" thing that still draws is a checkbox that lies,
                    // and the user finds out by plotting.
                    {
                        window.setFrameVisibleCommand(false);
                        window.repaint();
                        if (window.drawnFrameLinesForTesting() != 0)
                            fail("the frame was hidden and the canvas drew it anyway");
                        window.setTitleBlockVisibleCommand(false);
                        window.repaint();
                        if (window.drawnTitleBlockRowsForTesting() != 0)
                            fail("the title block was hidden and the canvas drew its rows");
                        window.setFrameVisibleCommand(true);
                        window.setTitleBlockVisibleCommand(true);
                        window.repaint();
                        if (window.drawnFrameLinesForTesting() == 0 ||
                            window.drawnTitleBlockRowsForTesting() == 0)
                            fail("turning the frame and the title block back on did nothing");
                    }

                    // MARGINS WIDER THAN THE PAPER ARE REFUSED, LOUDLY. A
                    // frame that quietly stopped drawing would be found by
                    // somebody holding a plot with no border.
                    {
                        const QString tooWide = window.setFrameMarginsCommand(400.0, 400.0);
                        if (!tooWide.contains(QStringLiteral("wider than the paper")))
                            fail(("absurd margins were accepted: " + tooWide.toStdString())
                                     .c_str());
                        window.repaint();
                        if (window.drawnFrameLinesForTesting() == 0)
                            fail("a refused margin took the frame with it");
                    }

                    // --- A DRAWING CAN BE SAVED AND OPENED AGAIN --------------
                    //
                    // THIS SHIPPED BROKEN FOR FOUR MILESTONES.
                    //
                    // saveDocumentFile handled parts and assemblies and fell
                    // through to `part()` for a drawing, which THROWS -- so
                    // Ctrl+S on any drawing built since M32 aborted, and every
                    // view, dimension, frame, title block and parts list could
                    // be made and never written back. openDocumentFile has
                    // read all three kinds since M27; the two halves were kept
                    // by hand and only one was finished.
                    //
                    // Nothing caught it because the self test built drawings
                    // and never saved one. It does now.
                    {
                        const QString path =
                            QDir::tempPath() +
                            QStringLiteral("/ep3d-selftest/m35-roundtrip.ep3dd");
                        QFile::remove(path);
                        const std::size_t views = window.drawingViewCountForTesting();
                        const std::size_t dimensions = window.dimensionCountForTesting();
                        const std::size_t entities = window.drawingEntityCountForTesting();
                        const QString title =
                            window.titleBlockValueForTesting(QStringLiteral("Title"));

                        const QString saved = window.saveDocumentFile(path);
                        if (!saved.contains(QStringLiteral("Saved")))
                            fail(("a drawing would not save: " + saved.toStdString()).c_str());
                        if (!QFile::exists(path)) fail("the drawing said it saved and did not");

                        const QString opened = window.openDocumentFile(path);
                        if (!opened.contains(QStringLiteral("Opened")))
                            fail(("the saved drawing would not open: " + opened.toStdString())
                                     .c_str());
                        if (window.openedDocumentType() != DocumentType::Drawing)
                            fail("a saved drawing came back as something else");
                        // EVERYTHING THAT WAS ON IT IS STILL ON IT. Counting
                        // is the point: a save that dropped the dimensions
                        // would open cleanly and be a different drawing.
                        if (window.drawingViewCountForTesting() != views)
                            fail("the reopened drawing has lost views");
                        if (window.dimensionCountForTesting() != dimensions)
                            fail("the reopened drawing has lost dimensions");
                        if (window.drawingEntityCountForTesting() != entities)
                            fail("the reopened drawing has lost geometry");
                        if (window.titleBlockValueForTesting(QStringLiteral("Title")) != title)
                            fail("the reopened drawing has lost its title block");
                        // ...AND A LOADED DOCUMENT HAS AN EMPTY UNDO HISTORY
                        // (ADR-M9-001).
                        if (window.undoDepthForTesting() != 0)
                            fail("a reopened drawing arrived with an undo history");
                    }

                    // --- M35.4's GATE: the plot ------------------------------
                    //
                    // "A file appeared" is not a plot. What has to be true is
                    // that the PAGE IS THE SHEET'S SIZE -- true size is the
                    // whole promise a drawing makes, and a page that came out
                    // A4 with an A2 drawing shrunk onto it makes every
                    // dimension on it a lie a reader can check with a rule.
                    {
                        const QString pdf =
                            QDir::tempPath() + QStringLiteral("/ep3d-selftest/m35-plot.pdf");
                        QFile::remove(pdf);
                        const QString plotted = window.plotToPdfCommand(pdf);
                        if (!plotted.contains(QStringLiteral("1:1")))
                            fail(("the plot did not report true size: " + plotted.toStdString())
                                     .c_str());
                        if (!QFile::exists(pdf)) fail("the plot said it wrote a file and did not");
                        if (QFileInfo(pdf).size() < 1000)
                            fail("the plot wrote a file with nothing in it");

                        // THE PAGE SIZE, READ BACK OUT OF THE FILE. A PDF
                        // states its MediaBox in points; A2 is 420 x 594 mm,
                        // which is 1190.55 x 1683.78 pt. Asserting the command
                        // said "A2" would only be asserting the report.
                        QFile file(pdf);
                        if (!file.open(QIODevice::ReadOnly)) fail("the plot cannot be read back");
                        const QByteArray bytes = file.readAll();
                        file.close();
                        const int box = bytes.indexOf("/MediaBox");
                        if (box < 0) fail("the plot has no page size in it at all");
                        const QByteArray media = bytes.mid(box, 64);
                        // 1190 and 1683, to the point -- loose enough for
                        // rounding, tight enough that A3 or A4 would fail.
                        if (!media.contains("119") || !media.contains("168"))
                            fail(("the plotted page is not A2: " + std::string(media.constData()))
                                     .c_str());
                        // THE DRAWING ON THE PAGE IS TRUE SIZE TOO.
                        //
                        // M35-18 survived the first mutation run: halving the
                        // plot's scale left the MediaBox untouched, so a page
                        // that came out A2 with the drawing on it at half
                        // size passed every check above. That is the failure a
                        // reader finds with a rule, after the drawing has been
                        // sent out -- so the transform is asserted directly.
                        {
                            const DrawingTransform page = PageTransformFor(594.0, 600);
                            const double perMm = 600.0 / 25.4;
                            if (std::fabs(page.pixelsPerMm - perMm) > 1e-9)
                                fail("a plotted millimetre is not a millimetre");
                            // Sheet (0, 0) is the page's BOTTOM-left, and the
                            // sheet's top-left is the page's (0, 0).
                            const QPointF bottomLeft = page.toScreen(Vec2{0.0, 0.0});
                            const QPointF topLeft = page.toScreen(Vec2{0.0, 594.0});
                            if (std::fabs(bottomLeft.y() - 594.0 * perMm) > 1e-6)
                                fail("the plot puts the sheet's bottom edge in the wrong place");
                            if (std::fabs(topLeft.y()) > 1e-6)
                                fail("the plot puts the sheet's top edge off the page");
                            // ...and 100 mm across the sheet is 100 mm across
                            // the page, which is the whole promise.
                            const QPointF hundred = page.toScreen(Vec2{100.0, 0.0});
                            if (std::fabs(hundred.x() - bottomLeft.x() - 100.0 * perMm) > 1e-6)
                                fail("100 mm on the sheet is not 100 mm on the page");
                        }

                        // --- M35.5's GATE: the DXF -----------------------
                        //
                        // A DXF of the drawing this gate built -- which has a
                        // projected VIEW in it, and that is the part no Core
                        // test can reach: the writer flattens a view into
                        // place, and a view needs the kernel to have anything
                        // to flatten.
                        {
                            const QString dxf =
                                QDir::tempPath() + QStringLiteral("/ep3d-selftest/m35.dxf");
                            QFile::remove(dxf);
                            const QString exported = window.exportDxfCommand(dxf);
                            if (!exported.contains(QStringLiteral("Exported")))
                                fail(("the DXF export failed: " + exported.toStdString())
                                         .c_str());
                            if (!QFile::exists(dxf)) fail("the DXF said it wrote a file and did not");

                            // WHAT IS IN IT, read as text -- the viewer cannot
                            // link the GPL reader (ADR-M6-001), so this checks
                            // the file rather than parsing it. The round trip
                            // through the real parser is the import suite's
                            // job; what only this can check is that a VIEW's
                            // curves came out at all.
                            QFile file(dxf);
                            if (!file.open(QIODevice::ReadOnly))
                                fail("the DXF cannot be read back");
                            const QByteArray bytes = file.readAll();
                            file.close();
                            if (!bytes.contains("AC1009"))
                                fail("the DXF does not say which version it is");
                            if (!bytes.contains("$INSUNITS"))
                                fail("the DXF does not say its numbers are millimetres");
                            if (!bytes.contains("\nLINE\n"))
                                fail("the DXF has no geometry in it at all");
                            // THE VIEW'S OWN LAYER. A recipient who wants the
                            // hidden detail off needs a switch, and
                            // "everything on layer 0" is not one.
                            if (!bytes.contains("Front"))
                                fail("the projected view did not get a layer of its own");
                            // ...and the drawn circle, at the size it was
                            // drawn: 20 mm radius, written with six decimals.
                            if (!bytes.contains("\n40\n20.000000"))
                                fail("the circle did not reach the DXF at the size it was drawn");

                            // NO ENTITY NAMES A TABLE ENTRY THE FILE DOES NOT
                            // DECLARE.
                            //
                            // This SHIPPED once. Views went out on a layer
                            // named after the view and hidden edges on a
                            // linetype called HIDDEN, and neither was in
                            // either table -- while the comment claiming that
                            // could not happen sat right above the code that
                            // did it. Found by reading the file back and
                            // looking at it.
                            //
                            // It is checked HERE and not in the import suite
                            // because the case that broke needs a real
                            // projected view, which needs a model file and a
                            // kernel.
                            {
                                const QList<QByteArray> lines = bytes.split('\n');
                                QList<QByteArray> declaredLayers;
                                QList<QByteArray> declaredLinetypes;
                                QList<QByteArray> usedLayers;
                                QList<QByteArray> usedLinetypes;
                                QByteArray table;
                                for (int i = 0; i + 3 < lines.size(); ++i) {
                                    const QByteArray code = lines[i].trimmed();
                                    const QByteArray value = lines[i + 1].trimmed();
                                    if (code == "0" && value == "TABLE")
                                        table = lines[i + 3].trimmed();
                                    if (code == "0" && value == "ENDTAB") table.clear();
                                    if (code == "0" && value == "LAYER" && table == "LAYER" &&
                                        lines[i + 2].trimmed() == "2")
                                        declaredLayers << lines[i + 3].trimmed();
                                    if (code == "0" && value == "LTYPE" && table == "LTYPE" &&
                                        lines[i + 2].trimmed() == "2")
                                        declaredLinetypes << lines[i + 3].trimmed();
                                    if (table.isEmpty()) {
                                        if (code == "8") usedLayers << value;
                                        if (code == "6") usedLinetypes << value;
                                    }
                                }
                                for (const QByteArray& name : usedLayers)
                                    if (!declaredLayers.contains(name))
                                        fail(("an entity is on layer '" +
                                              std::string(name.constData()) +
                                              "', which the DXF never declares")
                                                 .c_str());
                                for (const QByteArray& name : usedLinetypes)
                                    if (!declaredLinetypes.contains(name))
                                        fail(("an entity uses linetype '" +
                                              std::string(name.constData()) +
                                              "', which the DXF never declares")
                                                 .c_str());
                                // ...and the view's layer really is one of
                                // them, so this is not passing because nothing
                                // used a layer at all.
                                if (!declaredLayers.contains(QByteArray("Front")))
                                    fail("the projected view's layer is not in the DXF's "
                                         "layer table");

                                // HIDDEN HAS TO HAVE DASHES IN IT.
                                //
                                // A DXF has no flag for "this edge is hidden";
                                // the LINETYPE is the meaning. Declared with
                                // an empty pattern it plots SOLID, and the
                                // drawing then says the opposite of what it
                                // means -- an edge behind the part drawn as
                                // one in front of it.
                                //
                                // Checked here because the drawing never
                                // declares HIDDEN itself: the writer supplies
                                // it for a view's hidden edges, and a view
                                // needs the kernel.
                                if (declaredLinetypes.contains(QByteArray("HIDDEN"))) {
                                    int segments = -1;
                                    for (int i = 0; i + 3 < lines.size(); ++i) {
                                        if (lines[i].trimmed() != "2" ||
                                            lines[i + 1].trimmed() != "HIDDEN")
                                            continue;
                                        for (int j = i; j + 1 < lines.size() && j < i + 12; ++j)
                                            if (lines[j].trimmed() == "73")
                                                segments = lines[j + 1].trimmed().toInt();
                                        break;
                                    }
                                    if (segments <= 0)
                                        fail("HIDDEN went out with no dashes, so every hidden "
                                             "edge in this drawing plots solid");
                                }
                            }
                        }

                        // KEPT, not removed: it sits beside the golden
                        // screenshots so a person can open the plot and look
                        // at it. A byte check can say the page is A2 and still
                        // not say whether the drawing on it reads.
                    }

                    // A PICTURE OF A DRAWING WITH SIZES ON IT. Dimension
                    // rendering is a JUDGEMENT: geometry tests can say an
                    // arrowhead exists and points the right way and still not
                    // say whether the result reads as a drawing.
                    if (screenshotPath != nullptr) {
                        std::string beside = screenshotPath;
                        const std::size_t dot = beside.rfind('.');
                        beside = dot == std::string::npos
                                     ? beside + "-dimensions"
                                     : beside.substr(0, dot) + "-dimensions" +
                                           beside.substr(dot);
                        if (!shoot(window, QString::fromStdString(beside)))
                            fail("could not write the dimensions screenshot");
                    }
                }

                // DELETING A VIEW TAKES THE ONES PROJECTED OFF IT, and says so.
                window.selectDrawingViewForTesting(QStringLiteral("Front"));
                const QString deleted = window.deleteSelectedDrawingObject();
                if (!deleted.contains(QStringLiteral("projected from it")))
                    fail(("deleting a base view did not say what went with it: " +
                          deleted.toStdString())
                             .c_str());
                if (window.drawingViewCountForTesting() != 0)
                    fail("a projected view outlived its parent");

                // --- M35.6's GATE: the parts list ---------------------------
                //
                // The Core suite proves a list COUNTS, that identical parts
                // are one row, and that the numbers are never stored. Only
                // starting the program can prove a user can put one on the
                // paper and that its rows reach the screen.
                //
                // It runs here, near the end, because it replaces the document
                // twice -- once for the assembly it counts and once for the
                // drawing that counts it.
                {
                    const QString boltFile =
                        QDir::tempPath() + QStringLiteral("/ep3d-selftest/m35-bolt.ep3d");
                    const QString rigFile =
                        QDir::tempPath() + QStringLiteral("/ep3d-selftest/m35-rig.ep3da");
                    {
                        const QString script =
                            QDir::tempPath() + QStringLiteral("/ep3d-selftest/m35-bolt.ep3ds");
                        QFile out(script);
                        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                            fail("could not write M35's bolt script");
                        out.write(("sketch S\ntool rect\nclick 0 0\nclick 6 6\n"
                                   "pad Bolt 20 as BoltT\nsolve\nsave " +
                                   boltFile.toStdString() + "\n")
                                      .c_str());
                        out.close();
                        window.newDocumentCommand();
                        const QString made = window.runScriptFile(script);
                        if (made.contains(QStringLiteral("stopped")))
                            fail(("M35's bolt did not build: " + made.toStdString()).c_str());
                    }

                    // FOUR OF ONE BOLT, so the quantity column has something
                    // to say that a row count could not.
                    window.adoptAssemblyForTesting("M35Rig");
                    for (int i = 0; i < 4; ++i)
                        window.insertInstanceCommand(boltFile, QStringLiteral("Bolt"));
                    if (window.instanceCountForTesting() != 4)
                        fail("M35's rig did not take its four bolts");
                    if (!window.saveDocumentFile(rigFile).contains(QStringLiteral("Saved")))
                        fail("M35's rig would not save");

                    window.adoptDrawingForTesting("M35Parts");
                    // WHERE THE MENU WOULD PUT IT. A test that picked its own
                    // position would be checking a placement no user ever
                    // gets -- and the first draft did, which is how it came to
                    // sit across the title block in the golden screenshot.
                    const QString added = window.addBomTableCommand(
                        QStringLiteral("Parts"), rigFile, window.defaultBomPositionMm());
                    // ONE LINE, FOUR PARTS. A list that gave each instance its
                    // own row would say four lines, and somebody would order
                    // four lines of one bolt.
                    if (!added.contains(QStringLiteral("1 lines")) ||
                        !added.contains(QStringLiteral("4 parts")))
                        fail(("the parts list counted wrongly: " + added.toStdString())
                                 .c_str());
                    if (window.bomTableCountForTesting() != 1)
                        fail("the parts list was not created");

                    // IT REACHED THE CANVAS. "The document can count it" and
                    // "the rows are on screen" are two claims, and the gap
                    // between them is the M6.14 defect class.
                    window.repaintDrawingForTesting();
                    if (window.drawnBomRowsForTesting() != 1)
                        fail("the parts list counted one line and the canvas drew none");
                    if (window.drawnUncountedBomsForTesting() != 0)
                        fail("a countable parts list was drawn as uncountable");
                    if (window.staleBomCountForTesting() != 0)
                        fail("a freshly made parts list is already stale");

                    // A PICTURE OF A DRAWING WITH A PARTS LIST ON IT.
                    // Whether a table READS -- whether the columns are wide
                    // enough, whether the heading is where a reader expects --
                    // is a judgement, and no assertion makes it.
                    if (screenshotPath != nullptr) {
                        std::string beside = screenshotPath;
                        const std::size_t dot = beside.rfind('.');
                        beside = dot == std::string::npos
                                     ? beside + "-parts"
                                     : beside.substr(0, dot) + "-parts" + beside.substr(dot);
                        if (!shoot(window, QString::fromStdString(beside)))
                            fail("could not write the parts list screenshot");
                    }

                    // A LIST WHOSE ASSEMBLY HAS GONE SAYS SO, ON THE PAPER.
                    // An empty box and a list of nothing look the same, and
                    // only one of them is a drawing anybody can build from.
                    if (!QFile::remove(rigFile)) fail("could not take M35's rig away");
                    if (QFile::exists(rigFile)) fail("the rig file is still there after removal");
                    // THE DOCUMENT FIRST, then the screen: if the document can
                    // still count it, the problem is the file and not the
                    // paint, and saying which saves an afternoon.
                    if (window.bomTotalQuantityForTesting() != 0)
                        fail(("the assembly is gone and the document still counts " +
                              std::to_string(window.bomTotalQuantityForTesting()) + " parts")
                                 .c_str());
                    window.repaintDrawingForTesting();
                    if (window.drawnUncountedBomsForTesting() != 1)
                        fail("the assembly went away and the parts list still drew rows");
                    if (window.drawnBomRowsForTesting() != 0)
                        fail("a parts list with no assembly still drew its old rows");
                    // ...and the TREE says so too, on the root, because a
                    // reader who notices the paper looks wrong goes there.
                    if (window.probeOutline().state != OutlineState::Failed)
                        fail("a parts list cannot be counted and the drawing's root row "
                             "looks fine");
                }

                // --- M36's GATE: a schematic ---------------------------------
                //
                // A REAL CIRCUIT, drawn the way a hand would: place the parts,
                // arm the wire tool, click. The Core suite proves the crossing
                // rule and the terminal rule; only starting the program can
                // prove a user can build a circuit and that it reaches the
                // screen.
                //
                // The circuit: a fuse, a coil and a terminal in series, wired
                // top to bottom -- the smallest thing that is actually a
                // control circuit rather than two wires in a row.
                {
                    window.adoptDrawingForTesting("M36Schematic");

                    if (!window.placeSymbolCommand(QStringLiteral("Fuse"), Vec2{100.0, 200.0})
                             .contains(QStringLiteral("-F1")))
                        fail("the fuse was not tagged from its symbol's kind");
                    if (!window.placeSymbolCommand(QStringLiteral("Coil"), Vec2{100.0, 150.0})
                             .contains(QStringLiteral("-K1")))
                        fail("the coil was not tagged -K1");
                    window.placeSymbolCommand(QStringLiteral("Terminal"), Vec2{100.0, 100.0});
                    if (window.symbolCountForTesting() != 3)
                        fail("the three components were not placed");

                    // A SECOND FUSE GETS -F2, NOT -F1. Two parts with one tag
                    // is a wiring list that sends an electrician to whichever
                    // they find first.
                    if (window.nextTagFor(QStringLiteral("Fuse")) != QStringLiteral("-F2"))
                        fail(("a second fuse would be tagged " +
                              window.nextTagFor(QStringLiteral("Fuse")).toStdString())
                                 .c_str());

                    // --- THE WIRES, THROUGH THE TOOL ------------------------
                    window.setDrawingToolCommand(DrawingTool::Wire);
                    if (window.drawingToolForTesting() != DrawingTool::Wire)
                        fail("the wire tool did not arm");
                    // Fuse pin 2 (bottom, 8 mm down) to coil A1 (top, 7.5 up).
                    window.pickOnSheetForTesting(Vec2{100.0, 192.0});
                    window.pickOnSheetForTesting(Vec2{100.0, 157.5});
                    if (window.wireCountForTesting() != 1)
                        fail("the wire tool drew nothing");
                    if (window.drawingToolForTesting() != DrawingTool::None)
                        fail("the wire tool stayed armed after drawing one");
                    // Coil A2 (7.5 down) to the terminal's top pin (4 up).
                    window.setDrawingToolCommand(DrawingTool::Wire);
                    window.pickOnSheetForTesting(Vec2{100.0, 142.5});
                    window.pickOnSheetForTesting(Vec2{100.0, 104.0});
                    if (window.wireCountForTesting() != 2)
                        fail("the second wire was not drawn");

                    // --- WHAT THE CIRCUIT SAYS -------------------------------
                    //
                    // THREE nets, and the count is worth working through
                    // because a first draft said four:
                    //
                    //   1. the fuse's top pin, alone -- the circuit's live end
                    //   2. fuse bottom + coil A1, through the first wire
                    //   3. coil A2 + BOTH terminal pins, through the second --
                    //      both, because a terminal is one node (M36_SYM_003)
                    //
                    // Four would mean the terminal had split its own net in
                    // two, which is exactly the mistake that makes a wiring
                    // list claim twice the connections there are.
                    if (window.netCountForTesting() != 3)
                        fail(("the circuit came out as " +
                              std::to_string(window.netCountForTesting()) + " nets")
                                 .c_str());
                    // ONE net goes nowhere: the fuse's top pin. The
                    // terminal's spare pin does NOT, because it shares a net
                    // with the coil through the terminal itself.
                    if (window.danglingNetCountForTesting() != 1)
                        fail(("the circuit has " +
                              std::to_string(window.danglingNetCountForTesting()) +
                              " nets going nowhere, and should have one")
                                 .c_str());

                    // IT REACHED THE CANVAS. "The document holds a circuit" and
                    // "it is on screen" are two claims.
                    window.repaintDrawingForTesting();
                    if (window.drawnWiresForTesting() != 2)
                        fail("the schematic holds two wires and the canvas drew none");
                    if (window.drawnSymbolsForTesting() != 3)
                        fail("the schematic holds three components and the canvas drew none");
                    if (window.drawnUnknownSymbolsForTesting() != 0)
                        fail("a known symbol was drawn as unknown");

                    // --- NUMBERING -------------------------------------------
                    const QString numbered = window.numberNetsCommand();
                    if (!numbered.contains(QStringLiteral("Numbered 2")))
                        fail(("numbering said: " + numbered.toStdString()).c_str());
                    // ...AND IT SAYS WHAT IS STILL WRONG. A command reporting
                    // only its success would let a user walk away from a
                    // schematic with wires that go nowhere.
                    if (!numbered.contains(QStringLiteral("go nowhere")))
                        fail("numbering did not mention the nets that reach nothing");
                    // W1 IS THE TOP WIRE. A schematic is read down its rungs,
                    // and sheet Y runs upward -- so a first draft that sorted
                    // ascending numbered the bottom rung W1 and the drawing
                    // counted backwards. Checked here rather than read off a
                    // screenshot, because squinting at 2 mm text does not
                    // scale.
                    if (window.topmostWireLabelForTesting() != QStringLiteral("W1"))
                        fail(("the topmost wire is labelled " +
                              window.topmostWireLabelForTesting().toStdString() +
                              ", so the sheet was numbered from the bottom up")
                                 .c_str());

                    // Numbering again changes nothing, because every net has a
                    // name now.
                    if (!window.numberNetsCommand().contains(QStringLiteral("Numbered 0")))
                        fail("numbering twice renamed the wires");

                    // --- THE TREE --------------------------------------------
                    {
                        const OutlineNode tree = window.probeOutline();
                        std::size_t components = 0;
                        std::size_t nets = 0;
                        const std::function<void(const OutlineNode&)> walk =
                            [&](const OutlineNode& node) {
                                if (node.kind == OutlineKind::Component) ++components;
                                if (node.kind == OutlineKind::NetNode) ++nets;
                                for (const OutlineNode& child : node.children) walk(child);
                            };
                        walk(tree);
                        if (components != 3) fail("the tree does not show the three components");
                        if (nets != 3) fail("the tree does not show the circuit's nets");
                        // A wire that goes nowhere makes the DRAWING wrong, and
                        // the root is where a reader looks first when the paper
                        // seems right and the circuit does not work.
                        if (tree.state != OutlineState::Failed)
                            fail("a net goes nowhere and the drawing's root row looks fine");
                    }

                    // THE COMPONENTS WERE DRAWN WHERE THEY WERE PLACED.
                    //
                    // Counting them is not enough: a symbol drawn at the
                    // origin while its pins are at the placement would CONNECT
                    // in one place and DRAW in another, and the count would be
                    // identical. The three parts sit in a column at x = 100,
                    // between y = 96 and y = 208.
                    {
                        const Box2D extent = window.drawnSymbolExtentForTesting();
                        if (extent.empty) fail("the components drew no geometry at all");
                        if (std::fabs(extent.min.x - 100.0) > 12.0 ||
                            std::fabs(extent.max.x - 100.0) > 12.0)
                            fail(("the components were drawn around x = " +
                                  std::to_string((extent.min.x + extent.max.x) / 2.0) +
                                  ", and they were placed at 100")
                                     .c_str());
                        if (extent.min.y < 80.0 || extent.max.y > 220.0)
                            fail(("the components were drawn from y = " +
                                  std::to_string(extent.min.y) + " to " +
                                  std::to_string(extent.max.y) +
                                  ", which is not where they were placed")
                                     .c_str());
                    }

                    // --- A COMPONENT WHOSE SYMBOL IS NOT IN THE LIBRARY ------
                    //
                    // Drawn as a red box with its tag, because a component
                    // drawn as NOTHING looks exactly like one that was never
                    // placed.
                    {
                        const QString refused = window.placeSymbolCommand(
                            QStringLiteral("Thyristor"), Vec2{200.0, 150.0});
                        if (!refused.contains(QStringLiteral("no symbol")))
                            fail(("an unknown symbol was placed: " + refused.toStdString())
                                     .c_str());
                        if (window.symbolCountForTesting() != 3)
                            fail("a component with no symbol was placed anyway");
                    }

                    // ...BUT A FILE CAN STILL CARRY ONE, and that is the case
                    // the red box exists for: a drawing made against a bigger
                    // library, opened here. A component drawn as NOTHING looks
                    // exactly like one that was never placed, so the reader
                    // would never know a part had gone missing.
                    {
                        const QString path =
                            QDir::tempPath() +
                            QStringLiteral("/ep3d-selftest/m36-foreign.ep3dd");
                        if (!window.saveDocumentFile(path).contains(QStringLiteral("Saved")))
                            fail("the schematic would not save");
                        QFile file(path);
                        if (!file.open(QIODevice::ReadOnly)) fail("the schematic cannot be read");
                        QByteArray bytes = file.readAll();
                        file.close();
                        // Rename the coil's symbol to one this build has never
                        // heard of -- exactly what a newer library would leave
                        // behind.
                        bytes.replace("\"symbol\": \"Coil\"",
                                      "\"symbol\": \"Thyristor\"");
                        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
                            fail("the schematic cannot be rewritten");
                        file.write(bytes);
                        file.close();

                        const QString opened = window.openDocumentFile(path);
                        if (!opened.contains(QStringLiteral("Opened")))
                            fail(("the edited schematic would not open: " +
                                  opened.toStdString())
                                     .c_str());
                        window.repaintDrawingForTesting();
                        if (window.drawnUnknownSymbolsForTesting() != 1)
                            fail("a component naming a symbol this build does not have was "
                                 "drawn as nothing");
                        if (window.drawnSymbolsForTesting() != 3)
                            fail("the unknown component was dropped instead of marked");
                        // ...and the TREE says which one, because a red box on
                        // the paper with nothing in the tree is a reader
                        // hunting for what is wrong.
                        {
                            const OutlineNode tree = window.probeOutline();
                            bool sawFailed = false;
                            const std::function<void(const OutlineNode&)> walk =
                                [&](const OutlineNode& node) {
                                    if (node.kind == OutlineKind::Component &&
                                        node.state == OutlineState::Failed)
                                        sawFailed = true;
                                    for (const OutlineNode& child : node.children) walk(child);
                                };
                            walk(tree);
                            if (!sawFailed)
                                fail("a component with no symbol is not marked in the tree");
                        }
                    }

                    // A PICTURE OF A SCHEMATIC. Whether a circuit READS -- the
                    // junction dots, the wire numbers, the tags beside the
                    // parts -- is a judgement, and no assertion makes it.
                    if (screenshotPath != nullptr) {
                        std::string beside = screenshotPath;
                        const std::size_t dot = beside.rfind('.');
                        beside = dot == std::string::npos
                                     ? beside + "-schematic"
                                     : beside.substr(0, dot) + "-schematic" +
                                           beside.substr(dot);
                        if (!shoot(window, QString::fromStdString(beside)))
                            fail("could not write the schematic screenshot");
                    }
                }

                // AN EMPTY SHEET IS NOT HANDED OVER AS A FINISHED PLOT
                // (M35-20). A blank page a program said it wrote is the worst
                // of both: nobody looks at it until it matters.
                //
                // LAST, because it replaces the document -- everything above
                // needs the drawing this gate built, and a check that quietly
                // swapped it out from under them would break them in ways that
                // look like their own failures.
                {
                    window.adoptDrawingForTesting("Blank");
                    window.setFrameVisibleCommand(false);
                    window.setTitleBlockVisibleCommand(false);
                    const QString empty = window.plotToPdfCommand(
                        QDir::tempPath() + QStringLiteral("/ep3d-selftest/m35-blank.pdf"));
                    if (!empty.contains(QStringLiteral("nothing on this sheet")))
                        fail(("an empty sheet plotted as if it were a drawing: " +
                              empty.toStdString())
                                 .c_str());
                }
            }

            window.newDocumentCommand();
            if (window.openedDocumentType() != DocumentType::Part)
                fail("the self test did not get back to a part document");
            // ...AND THE BARS SWAP BACK. A one-way switch would be a
            // different defect wearing the same clothes.
            if (!window.modelToolbarVisible())
                fail("a part does not show the part toolbar");
            if (window.assemblyToolbarVisible())
                fail("a part is still showing the assembly toolbar");

            // ...and going back to a PART turns them on again, because a
            // one-way switch would be a different defect.
            window.newDocumentCommand();
            if (window.openedDocumentType() != DocumentType::Part)
                fail("File > New did not go back to a part document");
        }

        } catch (const std::exception& problem) {
            fail((std::string("uncaught exception: ") + problem.what()).c_str());
        } catch (...) {
            fail("an exception that is not a std::exception escaped the self test");
        }

        if (status == 0) std::printf("SELFTEST OK\n");
        app.quit();
    });
    app.exec();
    return status;
}
