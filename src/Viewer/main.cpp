// Minimal M4 viewer application (spec 17): displays the solid produced by a
// Sketch + Pad, with rotate, pan, zoom, fit-all, whole-object selection and
// refresh-after-recompute.
//
// The document and kernel are owned HERE, not by any Qt object (ADR-M4-006):
// Qt presentation objects never own the semantic CAD model.

#include "Core/Document/PartDocument.h"
#include "Core/Physics/MassProperties.h"
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
#include <cstring>
#include <cstdio>
#include <cmath>
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
    M5Circle          // Gate F: fixed-centre circle driven by a Radius Parameter
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

Sample SampleFromName(const char* name) {
    if (name == nullptr) return Sample::Rectangle;
    if (std::strcmp(name, "m4-failed-profile") == 0) return Sample::FailedProfile;
    if (std::strcmp(name, "m4-circle-r10") == 0) return Sample::CircleR10;
    if (std::strcmp(name, "m4-circle-r20") == 0) return Sample::CircleR20;
    if (std::strcmp(name, "m5-rectangle") == 0) return Sample::M5Rectangle;
    if (std::strcmp(name, "m5-underconstrained") == 0) return Sample::M5UnderConstrained;
    if (std::strcmp(name, "m5-conflict") == 0) return Sample::M5Conflict;
    if (std::strcmp(name, "m5-circle") == 0) return Sample::M5Circle;
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
    if (importPath != nullptr) importReport = window.importDxfFile(QString::fromUtf8(importPath));

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
        const Sample sampleBuilt = SampleFromName(sampleName);
        if (sampleBuilt != Sample::FailedProfile && sampleBuilt != Sample::M5Conflict &&
            presenter.displayableSolids().size() != 1)
            fail("expected exactly one displayable solid");

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

        // Selection round-trips through the shell by ObjectId.
        window.selectObject(model->pad->id());
        if (window.selectedObjectId() != model->pad->id()) fail("selection did not round-trip");

        if (status == 0) std::printf("SELFTEST OK\n");
        app.quit();
    });
    app.exec();
    return status;
}
