// Minimal M4 viewer application (spec 17): displays the solid produced by a
// Sketch + Pad, with rotate, pan, zoom, fit-all, whole-object selection and
// refresh-after-recompute.
//
// The document and kernel are owned HERE, not by any Qt object (ADR-M4-006):
// Qt presentation objects never own the semantic CAD model.

#include "Core/Document/PartDocument.h"
#include "Core/Physics/MassProperties.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Feature/PadFeature.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include "Viewer/DocumentOutline.h"
#include "Viewer/DocumentPresenter.h"
#include "Viewer/MainWindow.h"
#include "Viewer/OcctViewWidget.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QString>
#include <QTimer>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>

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
    M8Chain           // Pad 100x50x20 minus Pocket 20x30x10 = 94000 mm^3
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

    // --scenario <name>: drive the shell into a named state so the golden
    // screenshot set (UI spec 16) captures real application states rather than
    // mock-ups. Each name matches a UI-0xx entry in the self-validation report.
    const char* scenario = nullptr;
    const char* sampleName = nullptr;
    const char* importPath = nullptr;
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
            "--expect-skipped", "--dark", "--selftest"};
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
            model->document.removeObject(model->pad->id());
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
        window.selectObject(model->pad->id());
    if (scenario != nullptr && std::strcmp(scenario, "sketch-selected") == 0)
        window.selectObject(model->document.sketches().front()->id());

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
                // The constraint list is in the tree, under its sketch.
                const OutlineNode root = outline.build();
                std::size_t constraintRows = 0;
                const std::function<void(const OutlineNode&)> count =
                    [&](const OutlineNode& node) {
                        if (node.kind == OutlineKind::Constraint) ++constraintRows;
                        for (const OutlineNode& child : node.children) count(child);
                    };
                count(root);
                if (constraintRows != sketch->constraints().size())
                    fail("the constraint list does not show every constraint");

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
                // and all 13 viewer smoke tests stayed green through each.
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
                if (!report->skipped.empty() &&
                    window.displayedPropertyValue("Skipped item").empty())
                    fail("items were skipped but no diagnostic row is shown");
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

        // Selection round-trips through the shell by ObjectId.
        window.selectObject(model->pad->id());
        if (window.selectedObjectId() != model->pad->id()) fail("selection did not round-trip");

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
        window.selectObject(model->pad->id());
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

        if (status == 0) std::printf("SELFTEST OK\n");
        app.quit();
    });
    app.exec();
    return status;
}
