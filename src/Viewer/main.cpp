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
#include "Viewer/DocumentPresenter.h"
#include "Viewer/MainWindow.h"
#include "Viewer/OcctViewWidget.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QTimer>
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
    CircleR20       // D2: r = 20 padded 30 -- volume must be 4x D1
};

struct DemoModel {
    PartDocument document{"ViewerDemo"};
    OcctGeometryKernel kernel;
    Parameter* padLength = nullptr;
    PadFeature* pad = nullptr;

    explicit DemoModel(Sample sample) {
        document.setGeometryKernel(&kernel);
        document.addMaterial("Aluminium", 2700.0);

        const bool circle = sample == Sample::CircleR10 || sample == Sample::CircleR20;
        padLength = &document.addParameter("PadLength", circle ? 30.0 : 20.0,
                                           UnitType::Millimeter);
        Sketch& sketch = document.addSketch("Sketch001");

        if (circle) {
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
};

Sample SampleFromName(const char* name) {
    if (name == nullptr) return Sample::Rectangle;
    if (std::strcmp(name, "m4-failed-profile") == 0) return Sample::FailedProfile;
    if (std::strcmp(name, "m4-circle-r10") == 0) return Sample::CircleR10;
    if (std::strcmp(name, "m4-circle-r20") == 0) return Sample::CircleR20;
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
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--scenario") == 0) scenario = argv[i + 1];
        if (std::strcmp(argv[i], "--sample") == 0) sampleName = argv[i + 1];
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
        if (!window.isVisible()) fail("main window is not visible");
        if (window.width() < 800 || window.height() < 500) fail("main window is undersized");
        // The document produced a solid and the viewer is willing to show it.
        if (SampleFromName(sampleName) != Sample::FailedProfile &&
            presenter.displayableSolids().size() != 1)
            fail("expected exactly one displayable solid");
        // Mass properties are current and match the analytical oracle, so the
        // status bar cannot be showing stale or wrong numbers.
        // The expected volume depends on which sample was built, so the check
        // is a real oracle for each rather than one hardcoded number that only
        // happens to hold for the default.
        const Sample built = SampleFromName(sampleName);
        const double kPi = 3.14159265358979323846;
        double expectedVolume = 100000.0;                 // 100 x 50 x 20
        if (built == Sample::CircleR10) expectedVolume = kPi * 100.0 * 30.0;
        if (built == Sample::CircleR20) expectedVolume = kPi * 400.0 * 30.0;

        const MassProperties& mp = model->document.massProperties();
        if (built == Sample::FailedProfile) {
            // This sample is SUPPOSED to fail: an open profile must leave the
            // document reporting no current mass and nothing to draw.
            if (mp.valid) fail("a broken profile still reports current mass properties");
            if (!presenter.displayableSolids().empty())
                fail("a broken profile still offers a solid to draw");
        } else {
            if (!mp.valid) fail("mass properties are not current");
            if (std::fabs(mp.volumeMm3 - expectedVolume) >
                1e-6 * std::max(1.0, expectedVolume))
                fail("volume does not match the sample's analytical value");
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
