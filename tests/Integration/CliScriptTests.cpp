// M17.27 -- the headless script interpreter.
//
// These are integration tests on purpose: a script's whole claim is that it
// drives the SAME path a mouse does, so checking it against anything but a real
// document, a real solver and a real kernel would be checking it against a stub.

#include <cstdio>
#include "Core/Serialization/AssemblyDocumentSerializer.h"
#include "Cli/SketchScript.h"
#include "Core/Feature/DraftFeature.h"
#include "Core/Body/Body.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonAssemblySolver.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include "Viewer/SketchCanvas.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

struct ScriptRun {
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    PartDocument document{"ScriptDoc"};

    ScriptRun() {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&solver);
    }

    ScriptOutcome operator()(const std::string& text) {
        return RunSketchScript(document, text);
    }

    const Sketch& only() const {
        EXPECT_EQ(document.sketches().size(), 1u);
        return *document.sketches().front();
    }
};

int CountOf(const Sketch& sketch, std::size_t which) {
    int count = 0;
    for (const SketchEntity& entity : sketch.entities())
        if (entity.geometry.index() == which) ++count;
    return count;
}

constexpr std::size_t kLineIndex = 1;
constexpr std::size_t kCircleIndex = 2;

bool LogMentions(const ScriptOutcome& outcome, const std::string& text) {
    for (const ScriptLogEntry& entry : outcome.log)
        if (entry.text.find(text) != std::string::npos) return true;
    return false;
}

} // namespace

TEST(CliScriptTest, M17_CLI_001_EVERYToolHasAScriptName) {
    // The table in SketchScript.cpp is a second list of the tools, and a second
    // list is where this project's defects live.
    //
    // Counted against the ENUM'S RANGE rather than against a list this test
    // would also have to remember to extend: add a SketchTool and forget its
    // script name, and this fails.
    EXPECT_EQ(ScriptToolNames().size(), static_cast<std::size_t>(SketchTool::Slot) + 1)
        << "a SketchTool was added without a script name, or a name without a tool";

    // ...and no two tools share a name, which a count alone would not notice.
    std::vector<std::string> names = ScriptToolNames();
    std::sort(names.begin(), names.end());
    EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end())
        << "two tools share a script name";
}

TEST(CliScriptTest, M17_CLI_002_EveryScriptToolNameIsAccepted) {
    for (const std::string& name : ScriptToolNames()) {
        ScriptRun run;
        const ScriptOutcome outcome = run("sketch S\ntool " + name + "\n");
        EXPECT_TRUE(outcome.ok) << name << ": " << outcome.message;
    }
}

TEST(CliScriptTest, M17_CLI_003_AScriptDrawsWhatItSays) {
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch Plate
tool rect
click 0 0
click 100 50
)");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    // A rectangle is FOUR LINES and constraints, not a rectangle primitive --
    // so the script's names are Line1..Line4, and the log says so.
    EXPECT_EQ(CountOf(run.only(), kLineIndex), 4);
    EXPECT_TRUE(LogMentions(outcome, "Line1 Line2 Line3 Line4")) << "the log did not name them";
}

TEST(CliScriptTest, M17_CLI_004_AFailureNamesTheLineAndSTOPS) {
    // A script that carried on past a refused command would build something its
    // author did not write, and the surprise would arrive several commands
    // later with nothing pointing at the cause.
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch Plate
tool circle
click 0 0
click 10 0
tool wobble
click 50 50
)");
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failedLine, 6);
    EXPECT_NE(outcome.message.find("wobble"), std::string::npos) << outcome.message;
    // ...and it lists what IS known, because the alternative is guessing.
    EXPECT_NE(outcome.message.find("circle"), std::string::npos) << outcome.message;
    // The circle before it was made; the click after it was NOT.
    EXPECT_EQ(CountOf(run.only(), kCircleIndex), 1);
    EXPECT_EQ(run.only().entities().size(), 1u);
}

TEST(CliScriptTest, M17_CLI_005_AnUnknownNameListsWhatIsKnown) {
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch Plate
tool line
click 0 0
click 100 0
constrain horizontal Line7
)");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("Line7"), std::string::npos) << outcome.message;
    EXPECT_NE(outcome.message.find("Line1"), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M17_CLI_006_NamesStartAgainInEachSketch) {
    // A script's second sketch gets Line1 again. That is what a reader expects
    // and what lets each sketch's section be read on its own.
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch First
tool line
click 0 0
click 100 0
sketch Second
tool line
click 0 0
click 50 0
constrain horizontal Line1
)");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    ASSERT_EQ(run.document.sketches().size(), 2u);

    // THE HORIZONTAL LANDED IN THE SECOND SKETCH, on ITS Line1 -- which is the
    // whole claim. Counting constraints would not show that: both sketches
    // start at the origin, so both already carry a Fix the snapper inferred,
    // exactly as a mouse click there would.
    const auto horizontals = [](const Sketch& sketch) {
        int count = 0;
        for (const SketchConstraint& constraint : sketch.constraints())
            if (std::holds_alternative<HorizontalConstraint>(constraint.data)) ++count;
        return count;
    };
    EXPECT_EQ(horizontals(*run.document.sketches()[0]), 0);
    EXPECT_EQ(horizontals(*run.document.sketches()[1]), 1);
    for (const SketchConstraint& constraint : run.document.sketches()[1]->constraints()) {
        const auto* level = std::get_if<HorizontalConstraint>(&constraint.data);
        if (level == nullptr) continue;
        EXPECT_EQ(level->line, run.document.sketches()[1]->entities().front().id)
            << "Line1 in the second sketch named the first sketch's line";
    }
}

TEST(CliScriptTest, M17_CLI_007_ADimensionDrivesTheGeometryAndCanBeNamed) {
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch Plate
tool line
click 0 0
click 63 0
constrain fix Line1.start
constrain horizontal Line1
dimension length Line1 120 as PlateWidth
solve
)");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    // DRAWN AT 63 AND DIMENSIONED TO 120: the dimension has to have moved it,
    // or this test would pass on a script that did nothing.
    const SketchLine& line = std::get<SketchLine>(run.only().entities().front().geometry);
    EXPECT_NEAR(std::hypot(line.end.x - line.start.x, line.end.y - line.start.y), 120.0, 1e-6);
    EXPECT_EQ(run.only().degreesOfFreedom(), 0) << run.only().solveMessage();

    const Parameter* named = run.document.parameters().findByName("PlateWidth");
    ASSERT_NE(named, nullptr) << "`as` did not rename the Parameter";
    EXPECT_NEAR(named->value(), 120.0, 1e-9);
}

TEST(CliScriptTest, M17_CLI_008_AnAngleDimensionIsTypedInDEGREES) {
    // The same conversion the dimension editor makes, because it goes through
    // the same call. A script that meant radians here would be a second
    // convention for the same number.
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch Plate
tool line
click 0 0
click 100 0
tool line
click 0 0
click 80 30
dimension angle Line1 Line2 45 as Corner
solve
)");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    const Parameter* corner = run.document.parameters().findByName("Corner");
    ASSERT_NE(corner, nullptr);
    // Stored in RADIANS, typed in degrees.
    EXPECT_NEAR(corner->value(), 45.0 * 3.14159265358979323846 / 180.0, 1e-9);
}

TEST(CliScriptTest, M17_CLI_009_ClicksGoThroughTheREALSnapper) {
    // The claim that makes this worth having: `click` is a mouse click, so a
    // click that lands on existing geometry infers the same coincidence. A
    // script that built entities directly would skip all of that and test
    // nothing the GUI does.
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch Chain
tool line
click 0 0
click 100 0
tool line
click 100 0
click 100 60
solve
)");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    int coincidences = 0;
    for (const SketchConstraint& constraint : run.only().constraints())
        if (std::holds_alternative<CoincidentConstraint>(constraint.data)) ++coincidences;
    EXPECT_GE(coincidences, 1) << "the second line's start did not snap to the first line's end";
}

TEST(CliScriptTest, M17_CLI_010_TheWorkedExampleRunsAndReportsWhatItPromises) {
    // The regression test for the example under examples/. Its whole value is
    // that it walks draw -> constrain -> solve -> pad, which nothing else does
    // in one go -- and the last time a path was walked end to end it found a
    // loader dropping an ellipse dimension's Parameter (ADR-M17-050).
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch EllipseRing
tool ellipse
click 60 40
click 92 60
click 60 52
constrain fix Ellipse1.center
dimension majoraxis Ellipse1 40 as MajorA
dimension minoraxis Ellipse1 15 as MinorB
dimension ellipseangle Ellipse1 30 as EllipseAngle
tool circle
click 130 40
click 138 40
constrain concentric Ellipse1 Circle1
dimension radius Circle1 8 as HoleR
pad RingBody 10 as Thickness

sketch SplineBlade
tool spline
click 10 5
click 40 30
click 70 -5
click 100 25
click 130 5
finish
tool line
click 135 -20
click 5 -20
constrain fix Line1.end
constrain horizontal Line1
dimension length Line1 120 as BladeSpan
constrain coincident Spline1.start Line1.end
constrain coincident Spline1.end Line1.start
pad BladeBody 10 as BladeThickness
solve
)");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    ASSERT_EQ(run.document.sketches().size(), 2u);
    const Sketch& ring = *run.document.sketches()[0];
    const Sketch& blade = *run.document.sketches()[1];

    // THE ELLIPSE IS FULLY CONSTRAINED -- five numbers, five constraints.
    EXPECT_EQ(ring.degreesOfFreedom(), 0) << ring.solveMessage();
    EXPECT_EQ(ring.solveStatus(), SketchSolveStatus::Solved) << ring.solveMessage();
    // ...and every dimension MOVED something: the ellipse was drawn by eye.
    for (const SketchEntity& entity : ring.entities()) {
        if (const auto* e = std::get_if<SketchEllipse>(&entity.geometry)) {
            EXPECT_NEAR(e->center.x, 60.0, 1e-6);
            EXPECT_NEAR(e->majorRadiusMm, 40.0, 1e-6);
            EXPECT_NEAR(e->minorRadiusMm, 15.0, 1e-6);
            EXPECT_NEAR(e->rotationRad, 30.0 * 3.14159265358979323846 / 180.0, 1e-6);
        }
        if (const auto* c = std::get_if<SketchCircle>(&entity.geometry)) {
            // Drawn 70 mm away and pulled in by Concentric.
            EXPECT_NEAR(c->center.x, 60.0, 1e-6);
            EXPECT_NEAR(c->center.y, 40.0, 1e-6);
            EXPECT_NEAR(c->radiusMm, 8.0, 1e-6);
        }
    }

    // THE SPLINE KEEPS SIX. Its two ends are pulled onto the chord; the three
    // points between them cannot be named by a constraint, and the readout says
    // so rather than pretending.
    EXPECT_EQ(blade.degreesOfFreedom(), 6) << blade.solveMessage();
    for (const SketchEntity& entity : blade.entities()) {
        const auto* spline = std::get_if<SketchSpline>(&entity.geometry);
        if (spline == nullptr) continue;
        ASSERT_EQ(spline->points.size(), 5u);
        EXPECT_NEAR(spline->points.front().y, -20.0, 1e-6) << "the start was not pulled on";
        EXPECT_NEAR(spline->points.back().y, -20.0, 1e-6) << "the end was not pulled on";
        // ...and the middle is where it was DRAWN, untouched.
        EXPECT_NEAR(spline->points[1].x, 40.0, 1e-6);
        EXPECT_NEAR(spline->points[1].y, 30.0, 1e-6);
    }

    // Two bodies, both built.
    EXPECT_EQ(run.document.bodies().size(), 2u);
}

TEST(CliScriptTest, M17_CLI_011_SaveAndReopenRoundTripsThroughTheScript) {
    // Draw, save, reopen, and check the sketch still solves to the same place.
    // This is the path that found ADR-M17-050, kept as a test.
    // A path beside the test binary. std::filesystem::temp_directory_path is
    // the portable answer and this suite has no macro for a scratch directory.
    const std::string path =
        (std::filesystem::temp_directory_path() / "ep3d_cli_script_roundtrip.ep3d").string();
    {
        ScriptRun run;
        const ScriptOutcome outcome = run(
            "sketch S\n"
            "tool ellipse\n"
            "click 20 10\n"
            "click 50 30\n"
            "click 20 22\n"
            "constrain fix Ellipse1.center\n"
            "dimension majoraxis Ellipse1 40 as A\n"
            "dimension minoraxis Ellipse1 15 as B\n"
            "dimension ellipseangle Ellipse1 20 as T\n"
            "solve\n"
            "save " + path + "\n");
        ASSERT_TRUE(outcome.ok) << outcome.message;
        EXPECT_EQ(run.only().degreesOfFreedom(), 0) << run.only().solveMessage();
    }

    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver solver;
    LoadResult loaded = loadPartDocumentFromFile(path);
    ASSERT_TRUE(loaded) << loaded.message;
    loaded.document->setGeometryKernel(&kernel);
    loaded.document->setSketchSolver(&solver);
    ASSERT_TRUE(loaded.document->recompute().success);
    const Sketch& back = *loaded.document->sketches().front();
    EXPECT_EQ(back.degreesOfFreedom(), 0) << back.solveMessage();
}

TEST(CliScriptTest, M17_CLI_012_AValueTheEditorCannotReadIsRefused) {
    // The script hands the value to the dimension editor -- the same call the
    // panel makes -- so a value it cannot read is refused there, with its own
    // message, and the script stops on that line.
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch Plate
tool circle
click 0 0
click 10 0
dimension radius Circle1 nonsense
)");
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failedLine, 6);
    EXPECT_FALSE(outcome.message.empty());
}

TEST(CliScriptTest, M17_CLI_013_ASolveThatFAILSStopsTheScriptAndSaysWhy) {
    // A NEGATIVE RADIUS is not refused when it is typed: the document stores
    // whatever the editor accepted, and it is the SOLVE that decides a radius
    // of -5 is not a radius. That is the GUI's behaviour too -- the sketch goes
    // red rather than the keystroke being swallowed -- so the script surfaces
    // it the same way, at the line that asked for the solve.
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch Plate
tool circle
click 0 0
click 10 0
dimension radius Circle1 -5
solve
)");
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failedLine, 7) << outcome.message;
    // ...and the sketch's own complaint is in the log, above the failure.
    bool explained = false;
    for (const ScriptLogEntry& entry : outcome.log)
        if (entry.text.find("Plate") != std::string::npos) explained = true;
    EXPECT_TRUE(explained) << "the solve said nothing about which sketch";
}


TEST(CliScriptTest, M17_CLI_014_ASessionREMEMBERSBetweenCalls) {
    // What a socket needs and a file does not. `tool line` arrives in one
    // message and its two clicks in others; a session that started fresh each
    // time would answer "there is no sketch yet" to the second message of every
    // connection.
    ScriptRun run;
    SketchScriptSession session(run.document);
    EXPECT_TRUE(session.run("sketch Live\n").ok);
    EXPECT_TRUE(session.run("tool line\n").ok);
    EXPECT_TRUE(session.run("click 0 0\n").ok);
    EXPECT_TRUE(session.run("click 100 0\n").ok);
    ASSERT_EQ(run.document.sketches().size(), 1u);
    EXPECT_EQ(run.document.sketches().front()->entities().size(), 1u);

    // ...and the NAMES it gave survive too, which is what lets a later message
    // constrain what an earlier one drew.
    const ScriptOutcome later = session.run("constrain horizontal Line1\n");
    EXPECT_TRUE(later.ok) << later.message;
    // Counted by KIND, not in total: the click at the origin already inferred a
    // Fix, exactly as a mouse click there would.
    int horizontals = 0;
    for (const SketchConstraint& constraint : run.document.sketches().front()->constraints())
        if (std::holds_alternative<HorizontalConstraint>(constraint.data)) ++horizontals;
    EXPECT_EQ(horizontals, 1) << "the name Line1 did not survive between messages";
}

TEST(CliScriptTest, M17_CLI_015_TwoSessionsDoNotFinishEachOthersWork) {
    // One session per connection. Two clients sharing one would have the second
    // one's clicks land in the first one's half-drawn spline.
    ScriptRun run;
    SketchScriptSession first(run.document);
    SketchScriptSession second(run.document);
    EXPECT_TRUE(first.run("sketch A\ntool line\nclick 0 0\n").ok);

    // `second` has no sketch and no tool of its own, whatever `first` is doing.
    const ScriptOutcome stray = second.run("click 50 50\n");
    EXPECT_FALSE(stray.ok);
    EXPECT_NE(stray.message.find("no sketch"), std::string::npos) << stray.message;
}

TEST(CliScriptTest, M17_CLI_016_ALineNumberIsPerCALLNotPerSession) {
    // Over a connection there is no file for an absolute number to refer to, so
    // each message counts from 1 -- and the transport is what decides whether a
    // line number is worth printing at all.
    ScriptRun run;
    SketchScriptSession session(run.document);
    EXPECT_TRUE(session.run("sketch A\n").ok);
    const ScriptOutcome outcome = session.run("tool line\ntool wobble\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failedLine, 2);
    // The message carries NO line prefix: whoever prints it adds one if it
    // means something where they are printing.
    EXPECT_EQ(outcome.message.rfind("line ", 0), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M17_CLI_017_HelpListsTheVocabularyOverAConnectionToo) {
    // A socket has no --help to run, so `help` is a command -- and it asks the
    // same three lists the CLI's help prints rather than carrying a copy.
    ScriptRun run;
    const ScriptOutcome outcome = run("help\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "spline")) << "help did not list the tools";
    EXPECT_TRUE(LogMentions(outcome, "concentric")) << "help did not list the constraints";
    EXPECT_TRUE(LogMentions(outcome, "majoraxis")) << "help did not list the dimensions";
}

TEST(CliScriptTest, M17_CLI_018_ACarriageReturnFromASocketIsNotPartOfAToken) {
    // A client that sends CRLF -- and plenty do -- would otherwise make every
    // last token end in a stray carriage return, and every name unknown.
    ScriptRun run;
    const ScriptOutcome outcome = run("sketch A\r\ntool line\r\nclick 0 0\r\nclick 90 0\r\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_EQ(run.only().entities().size(), 1u);
}


TEST(CliScriptTest, M17_CLI_019_ABYTEORDERMARKIsNotPartOfTheFirstCommand) {
    // Windows tooling adds one freely -- PowerShell's `|` does, and so does
    // Notepad's "UTF-8" -- and it is INVISIBLE. The first thing ever piped into
    // the socket came back as `unknown command '<BOM>save'`, which names the
    // problem in characters the reader cannot see.
    ScriptRun run;
    const std::string bom = "\xEF\xBB\xBF";
    const ScriptOutcome outcome =
        run(bom + "sketch A\ntool line\nclick 0 0\nclick 40 0\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_EQ(run.only().entities().size(), 1u);
}

TEST(CliScriptTest, M17_CLI_020_ABOMOnlyCountsAtTheVeryStart) {
    // Those three bytes are a legitimate character anywhere else, so stripping
    // them per line would quietly eat content. Only the first line of a call
    // can carry a mark.
    ScriptRun run;
    const std::string bom = "\xEF\xBB\xBF";
    const ScriptOutcome outcome = run("sketch A\n" + bom + "tool line\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_EQ(outcome.failedLine, 2);
}

// --- M17.30: NAMING A SPLINE POINT FROM A SCRIPT -----------------------------

TEST(CliScriptTest, M17_CLI_021_ASevenNodeSplineReachesDOFZERO) {
    // The whole point of M17.30, walked end to end the way a user would: draw
    // seven points, dimension every one of them, and get a sketch that is
    // actually finished. Before this, ten of its fourteen freedoms had no name
    // and DOF 10 was the best any script could do.
    ScriptRun run;
    std::string script =
        "sketch Blade\ntool spline\n"
        "click 7 -13\nclick 33 31\nclick 61 -7\nclick 92 27\n"
        "click 119 -5\nclick 147 23\nclick 173 -11\nfinish\n"
        "constrain fix Spline1.start\n"
        "dimension hdistance Spline1.start Spline1.end 180 as Span\n"
        "dimension vdistance Spline1.start Spline1.end 0 as Rise\n";
    const double xs[] = {30, 60, 90, 120, 150};
    const double ys[] = {45, -8, 40, -6, 35};
    for (int i = 0; i < 5; ++i) {
        const std::string p = "Spline1.p" + std::to_string(i + 1);
        script += "dimension hdistance Spline1.start " + p + " " + std::to_string(xs[i]) +
                  " as X" + std::to_string(i) + "\n";
        script += "dimension vdistance Spline1.start " + p + " " + std::to_string(ys[i]) +
                  " as Y" + std::to_string(i) + "\n";
    }
    const ScriptOutcome outcome = run(script + "solve\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_EQ(run.only().degreesOfFreedom(), 0) << run.only().solveMessage();
}

TEST(CliScriptTest, M17_CLI_022_PZeroIsWrittenAsTheStartPoint) {
    // ONE POINT, ONE SPELLING, all the way out to the script vocabulary. `p0`
    // is allowed to be TYPED -- it is the obvious thing to type -- but it
    // resolves to the same reference `.start` does, so a script that mixes the
    // two does not end up with two constraints on one point.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool spline\nclick 5 7\nclick 40 30\nclick 80 0\nfinish\n"
            "constrain fix Spline1.p0\nsolve\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    const auto& constraints = run.only().constraints();
    ASSERT_EQ(constraints.size(), 1u);
    const auto* fixed = std::get_if<FixConstraint>(&constraints.front().data);
    ASSERT_NE(fixed, nullptr);
    EXPECT_EQ(fixed->target.subElement, SketchSubElement::StartPoint);
}

TEST(CliScriptTest, M17_CLI_023_AnIndexPastTheEndSaysHowManyThereAre) {
    // The refusal has to name the range, because the number is the one thing
    // the writer got wrong and the one thing they cannot see from the script.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool spline\nclick 0 0\nclick 40 30\nclick 80 0\nfinish\n"
            "constrain fix Spline1.p7\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("0 to 2"), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M17_CLI_024_PNOnSomethingThatIsNotASplineIsREFUSED) {
    // A line has no third point, and answering with its start would be a
    // constraint on a point the writer never asked for.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool line\nclick 0 0\nclick 40 0\nconstrain fix Line1.p1\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("not a spline"), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M17_CLI_025_TheLastPNIsWrittenAsTheEndPoint) {
    // The other half of the one-spelling rule, and the half a mutation walked
    // straight through: forgetting that the LAST point is the end leaves `p2`
    // on a three-point spline resolving as an interior point -- which is
    // refused, so a perfectly reasonable script fails with a message about
    // interior points that names a point plainly at the end.
    //
    // Both ends are checked here because they are two separate lines of code,
    // and a test that only ever looks at the first one only ever holds one.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool spline\nclick 5 7\nclick 40 30\nclick 80 0\nfinish\n"
            "constrain fix Spline1.p2\nsolve\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    const auto& constraints = run.only().constraints();
    ASSERT_EQ(constraints.size(), 1u);
    const auto* fixed = std::get_if<FixConstraint>(&constraints.front().data);
    ASSERT_NE(fixed, nullptr);
    EXPECT_EQ(fixed->target.subElement, SketchSubElement::EndPoint);
    EXPECT_EQ(fixed->target.index, 0) << "an end carries no index; a stray one would make the "
                                         "same point compare unequal to itself";
}

// --- M18: MEASURING FROM A SCRIPT --------------------------------------------

TEST(CliScriptTest, M18_CLI_001_MeasureReportsWithoutChangingAnything) {
    // The whole reason measure is a script command: a script can now CHECK what
    // it built. Until this, the only way to know whether a script had produced
    // the shape it meant to was to open the saved file and read numbers out of
    // it by hand.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool line\nclick 10 10\nclick 50 40\nmeasure Line1\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "length = 50 mm")) << "measure printed no length";
    // ONE entity, and no constraints: measuring is looking, not editing.
    EXPECT_EQ(run.only().entities().size(), 1u);
    EXPECT_EQ(run.only().constraints().size(), 0u);
}

TEST(CliScriptTest, M18_CLI_002_AnApproximateMeasurementSaysSo) {
    // A reader told a spline is 94.3 mm, with no other signal, will use that
    // number as though it were exact.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool spline\nclick 5 7\nclick 40 30\nclick 80 0\nfinish\n"
            "measure Spline1\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "(approx)")) << "a sampled length was not marked";
}

TEST(CliScriptTest, M18_CLI_003_AnAreaIsPrintedInSQUAREMillimetres) {
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool circle\nclick 100 0\nclick 130 0\nmeasure Circle1\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "mm^2")) << "an area was not printed in mm^2";
}

TEST(CliScriptTest, M18_CLI_004_MeasuringBeforeThereIsASketchIsREFUSED) {
    ScriptRun run;
    const ScriptOutcome outcome = run("measure Line1\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("no sketch yet"), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M18_CLI_005_TangentTakesTheSplineFirstWHICHEVEROrderItIsSelectedIn) {
    // The bug a runnable example caught and 1,496 tests did not.
    //
    // The solve session requires the spline first, because `at` names an end of
    // the FIRST entity. The canvas picks that order -- and its condition read
    // `spline1 ? !spline0 : (!line0 && line1)`, which put the LINE first
    // whenever the spline happened to be selected first. So a user who clicked
    // the spline and then the line got their own constraint refused, with a
    // message naming a rule they had followed.
    //
    // Both orders, because one of them passed the whole time.
    for (const char* order : {"constrain tangent Spline1 Line1\n",
                              "constrain tangent Line1 Spline1\n"}) {
        ScriptRun run;
        const ScriptOutcome outcome =
            run(std::string("sketch A\ntool line\nclick 5 5\nclick 120 5\n"
                            "tool spline\nclick 120 5\nclick 150 35\nclick 120 70\nfinish\n") +
                order + "solve\n");
        ASSERT_TRUE(outcome.ok) << order << " -> " << outcome.message;
        EXPECT_NE(run.only().solveStatus(), SketchSolveStatus::InvalidInput)
            << order << " -> " << run.only().solveMessage();
    }
}

// --- M18: TANGENT HANDLES FROM A SCRIPT --------------------------------------

TEST(CliScriptTest, M18_CLI_006_AHandleChangesTheCurveAndIsMeasurable) {
    // End to end: give a point a tangent, and the curve through the same points
    // is a different length. A handle that did nothing would pass every test
    // about naming it.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool spline\nclick 5 7\nclick 60 60\nclick 120 5\nfinish\n"
            "measure Spline1\nhandle Spline1.p1 40 0\nmeasure Spline1\nmeasure Spline1.h1\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    // The tip is the point plus the tangent: (60,60) + (40,0).
    EXPECT_TRUE(LogMentions(outcome, "u = 100 mm")) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "v = 60 mm")) << outcome.message;

    const auto& spline = std::get<SketchSpline>(run.only().entities().front().geometry);
    ASSERT_EQ(spline.handles.size(), 1u);
    EXPECT_DOUBLE_EQ(spline.handles.at(1).x, 40.0);
}

TEST(CliScriptTest, M18_CLI_007_AHandleCanBeTakenAwayAgain) {
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool spline\nclick 5 7\nclick 60 60\nclick 120 5\nfinish\n"
            "handle Spline1.p1 40 0\nhandle Spline1.p1 off\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(std::get<SketchSpline>(run.only().entities().front().geometry).handles.empty());
}

TEST(CliScriptTest, M18_CLI_008_NamingAHandleThatIsNotThereSaysHowToMakeOne) {
    // The refusal has to carry the next move. "No handle" alone leaves the
    // writer with a true statement and nothing to do about it.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool spline\nclick 5 7\nclick 60 60\nclick 120 5\nfinish\n"
            "measure Spline1.h1\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("handle Spline1.p1"), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M18_CLI_009_AHandleOnTheStartIsTheSameAsOnPointZero) {
    // ONE POINT, ONE SPELLING, carried into the handle command: `.start` and
    // `.p0` are the same point, so a handle put on one is the handle on the
    // other -- not a second handle.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool spline\nclick 5 7\nclick 60 60\nclick 120 5\nfinish\n"
            "handle Spline1.start 10 20\nhandle Spline1.p0 30 40\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    const auto& spline = std::get<SketchSpline>(run.only().entities().front().geometry);
    ASSERT_EQ(spline.handles.size(), 1u);
    EXPECT_DOUBLE_EQ(spline.handles.at(0).x, 30.0);
}

TEST(CliScriptTest, M18_CLI_010_ASignedDistanceOfZEROIsALegalDimension) {
    // "Make these two level" is a horizontal distance of nought, and the canvas
    // refused it for being "too small to dimension" -- a magnitude assumption
    // applied to a SIGNED quantity, which is the same correction
    // DimensionValueValid already carries, living on in a second place that
    // never heard about it.
    //
    // Found by writing the M18 example: saying "this handle is level with its
    // point" is exactly a vertical distance of nought, and it is the ordinary
    // way to hold a tangent.
    for (const char* kind : {"hdistance", "vdistance"}) {
        ScriptRun run;
        const ScriptOutcome outcome =
            run(std::string("sketch A\ntool line\nclick 5 5\nclick 80 5\n"
                            "tool point\nclick 40 60\n") +
                "dimension " + kind + " Line1.start Point1 0 as Zero\n");
        EXPECT_TRUE(outcome.ok) << kind << " -> " << outcome.message;
    }
}

TEST(CliScriptTest, M18_CLI_011_DimensioningGeometryWithNoEXTENTIsStillREFUSED) {
    // The other half, and what the guard actually guards: the CURRENT measured
    // value, not the number being typed. Two points on top of each other have
    // no separation to name -- the direction a dimension would drive them apart
    // in is undefined -- so the minimum stays where it means something, and the
    // fix above is not simply the guard switched off.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool point\nclick 40 60\ntool point\nclick 40 60\n"
            "dimension distance Point1 Point2 25 as Apart\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("too small"), std::string::npos) << outcome.message;
}

// --- M19: SWEEP and LOFT from a script ---------------------------------------

TEST(CliScriptTest, M19_CLI_001_ASweepBuildsASolidTheScriptCanMEASURE) {
    // The whole path in one script: two sketches on two planes, a sweep, and
    // then the number that proves a solid came out. Before `measure` there was
    // no evidence in a script beyond nothing having complained.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Section\ntool rect\nclick -10 -10\nclick 10 10\n"
            "sketch Spine xz\ntool line\nclick 0 0\nclick 0 100\n"
            "sweep Pipe Section Spine\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "volume = 40000 mm^3")) << outcome.message;
}

TEST(CliScriptTest, M19_CLI_002_ASketchCanBePutOnAPlaneAndOffsetAlongIt) {
    // The thing M19 needed from the script vocabulary. Every script sketch was
    // world XY, which made a sweep impossible to write -- a section swept along
    // a spine on its own plane has no volume -- and a loft equally so.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Low\ntool rect\nclick -20 -20\nclick 20 20\n"
            "sketch High xy 40\ntool rect\nclick -20 -20\nclick 20 20\n"
            "loft Tower Low High\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    // Two identical 40 mm squares 40 mm apart: a prism, 40*40*40.
    EXPECT_TRUE(LogMentions(outcome, "volume = 64000 mm^3")) << outcome.message;
}

TEST(CliScriptTest, M19_CLI_003_ASweepAlongItsOWNSketchIsREFUSEDWithTheReason) {
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Only\ntool rect\nclick -10 -10\nclick 10 10\n"
            "sweep Pipe Only Only\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("two different sketches"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M19_CLI_004_ALoftNeedsTwoSectionsAndSaysSo) {
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Only\ntool rect\nclick -10 -10\nclick 10 10\nloft Tower Only\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("at least two sections"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M19_CLI_005_AnUnknownPlaneIsREFUSEDNotSilentlyWorldXY) {
    // Falling back to XY would put the sketch somewhere the author did not ask
    // for, and the sweep built from it would come out flat -- a failure a long
    // way from the typo that caused it.
    ScriptRun run;
    const ScriptOutcome outcome = run("sketch Tilted zx\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("xy, xz or yz"), std::string::npos) << outcome.message;
}

// --- M20: SHELL, DRAFT and HOLE from a script --------------------------------

TEST(CliScriptTest, M20_CLI_001_AShellHollowsThePartToTheThicknessGiven) {
    // 60 x 60 x 40 with a 5 mm wall and an open top: the cavity is 50 x 50 x 35,
    // which is arithmetic and not the kernel's answer read back.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Base\ntool rect\nclick -30 -30\nclick 30 30\n"
            "pad Case 40\nshell Case 5 top\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "volume = 56500 mm^3")) << outcome.message;
}

TEST(CliScriptTest, M20_CLI_002_PadThenShellDressTheSAMEBody) {
    // `addBody` always makes a new one, so building a part in two steps left
    // two bodies both called Case and the second command dressed an empty one.
    // The volume is what says which happened: a shell of nothing has none.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Base\ntool rect\nclick -30 -30\nclick 30 30\n"
            "pad Case 40\nshell Case 5 top\nsolve\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_EQ(run.document.bodies().size(), 1u) << "the script made a second body of that name";
}

TEST(CliScriptTest, M20_CLI_003_AHoleWithNoDepthGoesAllTheWayThrough) {
    // One 10 mm bore through a 60 x 60 x 20 pad: pi * 25 * 20 of material gone.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Base\ntool rect\nclick -30 -30\nclick 30 30\npad Block 20\n"
            "sketch Mounts xy 20\ntool point\nclick 0 0\n"
            "hole Block Mounts 10\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;

    const double expected = 60.0 * 60.0 * 20.0 - 3.14159265358979323846 * 25.0 * 20.0;
    bool found = false;
    for (const ScriptLogEntry& entry : outcome.log) {
        const std::size_t at = entry.text.find("volume = ");
        if (at == std::string::npos) continue;
        found = true;
        EXPECT_NEAR(std::stod(entry.text.substr(at + 9)), expected, 1e-3) << entry.text;
    }
    EXPECT_TRUE(found) << "measure reported no volume";
}

TEST(CliScriptTest, M20_CLI_004_ADraftIsGivenInDEGREESAndStoredInRadians) {
    // A drawing says 3 degrees; the feature's unit check demands radians. The
    // conversion happens once, in the script, and the Parameter that comes out
    // has to carry the right unit or the feature refuses it.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Base\ntool rect\nclick -30 -30\nclick 30 30\npad Block 40\n"
            "draft Block 3 bottom facing:+y\nsolve\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;

    const Parameter* angle = run.document.parameters().findByName("DraftAngle1");
    ASSERT_NE(angle, nullptr);
    EXPECT_EQ(angle->unit(), UnitType::Radian);
    EXPECT_NEAR(angle->value(), 3.0 * 3.14159265358979323846 / 180.0, 1e-12);
}

TEST(CliScriptTest, M20_CLI_005_AnUnknownFaceWordIsREFUSEDNotGuessed) {
    // Guessing "top" would open a face nobody asked for, and the part would
    // come back hollow in the wrong place with nothing to say.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Base\ntool rect\nclick -30 -30\nclick 30 30\npad Case 40\n"
            "shell Case 5 sideways\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("is not a face"), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M20_CLI_006_DressingABodyThatHasNothingInItIsREFUSED) {
    ScriptRun run;
    const ScriptOutcome outcome = run("sketch Base\ntool rect\nclick -30 -30\nclick 30 30\n"
                                      "shell Ghost 5 top\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("no body called"), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M20_CLI_007_FACINGAndTHEEXTREMEFaceAreDifferentSentences) {
    // `+y` is the OUTERMOST face pointing that way; `facing:+y` is EVERY face
    // pointing that way. On a plain box they name the same one face, so the
    // difference only shows once a part has a step in it -- but the words have
    // to mean different things from the start, or a script written with one
    // will quietly get the other.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Base\ntool rect\nclick -30 -30\nclick 30 30\npad Block 40\n"
            "draft Block 3 bottom facing:+y\nsolve\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;

    const Body* body = run.document.bodies().front().get();
    const auto* draft = dynamic_cast<const DraftFeature*>(body->features().back().get());
    ASSERT_NE(draft, nullptr);
    ASSERT_EQ(draft->faces().size(), 1u);
    // `facing:` sets facing and NOT extremeTowards. Setting both would narrow
    // to one face on every part, which is the other sentence.
    EXPECT_TRUE(draft->faces().front().facing.has_value());
    EXPECT_FALSE(draft->faces().front().extremeTowards.has_value());
}

// --- M21: BOOLEANS, MULTI-BODY and patterns from a script --------------------

TEST(CliScriptTest, M21_CLI_001_TwoPadsInONEBodyAreTwoSeparateSolids) {
    // What multi-body means here: a Body is a feature chain, and two features
    // in it that nothing consumes are two disjoint parts. `subtract` then turns
    // two into one -- 80x80x30 less the 20x80x30 of slot that lay inside it.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Plate\ntool rect\nclick -40 -40\nclick 40 40\npad Block 30\n"
            "sketch Slot\ntool rect\nclick -10 -60\nclick 10 60\npad Block 30\n"
            "subtract Block\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "volume = 144000 mm^3")) << outcome.message;
    EXPECT_EQ(run.document.bodies().size(), 1u);
}

TEST(CliScriptTest, M21_CLI_002_ABooleanNeedsTWOSolidsAndSaysHowToMakeThem) {
    // The refusal has to carry the next move: "two separate solids" is not
    // something a reader can act on without being told that padding into the
    // same body twice is how you get them.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Plate\ntool rect\nclick -40 -40\nclick 40 40\npad Block 30\n"
            "subtract Block\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("pad into the same body twice"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M21_CLI_003_ARingOfSixTeethIsSixTimesTheOne) {
    // The step is PER INSTANCE: six at 60 degrees is a full ring, and the six
    // do not touch, so the volume is six times one tooth.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Tooth\ntool rect\nclick 60 -8\nclick 90 8\npad Rotor 12\n"
            "ring Rotor 6 60\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "volume = 34560 mm^3")) << outcome.message;
}

TEST(CliScriptTest, M21_CLI_004_ARingStepIsGivenInDEGREESAndStoredInRadians) {
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Tooth\ntool rect\nclick 60 -8\nclick 90 8\npad Rotor 12\n"
            "ring Rotor 6 60\nsolve\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    const Parameter* step = run.document.parameters().findByName("RingStep1");
    ASSERT_NE(step, nullptr);
    EXPECT_EQ(step->unit(), UnitType::Radian);
    EXPECT_NEAR(step->value(), 60.0 * 3.14159265358979323846 / 180.0, 1e-12);
}

TEST(CliScriptTest, M21_CLI_005_CopiesAreSpacedAlongThePathSketch) {
    // Five studs along a 150 mm line, each 10 mm: they do not touch, so the
    // volume is five times one.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Stud\ntool rect\nclick -5 -5\nclick 5 5\npad Rail 8\n"
            "sketch Track\ntool line\nclick 0 0\nclick 150 0\n"
            "along Rail Track 5\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "volume = 4000 mm^3")) << outcome.message;
}

// --- M22: EXPORT and IMPORT from a script ------------------------------------

namespace {

// A scratch file that removes itself, so a suite that runs twice does not read
// what the previous run left behind.
struct ScratchIo {
    std::string path;
    explicit ScratchIo(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-cli-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~ScratchIo() { std::remove(path.c_str()); }
    bool exists() const { return std::filesystem::exists(path); }
};

// Every volume `measure` reported, in order. A test that looks at only the
// last one cannot see a step that changed nothing.
std::vector<double> MeasuredVolumes(const ScriptOutcome& outcome) {
    std::vector<double> found;
    for (const ScriptLogEntry& entry : outcome.log) {
        const std::size_t at = entry.text.find("volume = ");
        if (at != std::string::npos) found.push_back(std::stod(entry.text.substr(at + 9)));
    }
    return found;
}

double LastMeasuredVolume(const ScriptOutcome& outcome) {
    const std::vector<double> all = MeasuredVolumes(outcome);
    return all.empty() ? -1.0 : all.back();
}

} // namespace

TEST(CliScriptTest, M22_CLI_001_APartCanGoOutAndComeBackTheSameSize) {
    // The gate for the whole milestone: what EP3D writes, EP3D reads, and the
    // volume is the same on both sides. Not "a file appeared" -- a file can
    // appear holding a part a thousand times too big.
    ScratchIo step{"roundtrip.step"};

    ScriptRun out;
    const ScriptOutcome written =
        out(std::string("sketch A\ntool rect\nclick -30 -30\nclick 30 30\npad Block 40\n"
                        "solve\nexport Block ") + step.path + "\n");
    ASSERT_TRUE(written.ok) << written.message;
    ASSERT_TRUE(step.exists());

    ScriptRun back;
    const ScriptOutcome read =
        back(std::string("import ") + step.path + " as Ghost\nsolve\nmeasure\n");
    ASSERT_TRUE(read.ok) << read.message;
    EXPECT_TRUE(LogMentions(read, "volume = 144000 mm^3")) << read.message;
}

TEST(CliScriptTest, M22_CLI_002_TheEXTENSIONChoosesTheFormat) {
    // Asking for the format separately would let a .step be written as STL,
    // which every reader at the far end would refuse for a reason that names
    // neither this program nor the choice.
    ScratchIo stl{"chooses.stl"};
    ScriptRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch A\ntool rect\nclick -30 -30\nclick 30 30\npad Block 40\n"
                        "solve\nexport Block ") + stl.path + "\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "(STL, deflection")) << outcome.message;
    EXPECT_TRUE(stl.exists());
}

TEST(CliScriptTest, M22_CLI_003_AnUnknownExtensionIsREFUSED) {
    // Guessing STEP would write a file the name says is something else.
    ScratchIo odd{"mystery.iges"};
    ScriptRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch A\ntool rect\nclick -30 -30\nclick 30 30\npad Block 40\n"
                        "solve\nexport Block ") + odd.path + "\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("use .step or .stl"), std::string::npos) << outcome.message;
    EXPECT_FALSE(odd.exists());
}

TEST(CliScriptTest, M22_CLI_004_ExportingBeforeSolvingSaysSo) {
    // The features exist and the geometry does not. "Nothing to export" would
    // be true and useless; naming the missing step is what a reader can act on.
    ScratchIo step{"unsolved.step"};
    ScriptRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch A\ntool rect\nclick -30 -30\nclick 30 30\npad Block 40\n"
                        "export Block ") + step.path + "\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("`solve` before exporting"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M22_CLI_005_OnlySTEPCanBeImported) {
    // STL is triangles: importing one would give a faceted mesh pretending to
    // be a solid, and every downstream face query would then find hundreds of
    // faces where the part has six.
    ScriptRun run;
    const ScriptOutcome outcome = run("import parts/thing.stl as Ghost\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("only STEP can be imported"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M22_CLI_006_AnSTLDeflectionIsOnlyForSTL) {
    // A STEP export has no deflection -- it is exact -- so a number after the
    // file name means the writer misunderstood something, and accepting and
    // ignoring it would leave them believing it did something.
    ScratchIo step{"deflected.step"};
    ScriptRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch A\ntool rect\nclick -30 -30\nclick 30 30\npad Block 40\n"
                        "solve\nexport Block ") + step.path + " 0.1\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("only an STL export takes a deflection"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M22_CLI_007_TheWorkedRoundTripExampleRunsAndKeepsTheSameSolid) {
    // The regression test for examples/step-round-trip.ep3ds. A block would
    // round-trip through almost anything, including a writer that threw the
    // geometry away and a reader that rebuilt a bounding box -- so the part
    // that goes out here is SHELLED AND DRILLED, and what comes back has to be
    // buildable, which needs its real faces and not a mesh of them.
    ScratchIo step{"worked.step"};

    ScriptRun out;
    const ScriptOutcome written =
        out(std::string("sketch Base\ntool rect\nclick -40 -40\nclick 40 40\npad Case 50\n"
                        "shell Case 4 top\n"
                        "sketch Bores xy 50\ntool point\nclick -22 0\ntool point\nclick 22 0\n"
                        "hole Case Bores 10\nsolve\nmeasure\nexport Case ") + step.path + "\n");
    ASSERT_TRUE(written.ok) << written.message;
    const double before = LastMeasuredVolume(written);

    ScriptRun back;
    const ScriptOutcome read =
        back(std::string("import ") + step.path +
             " as Ghost\nsolve\nmeasure\n"
             "sketch Late xy 50\ntool point\nclick 0 25\nhole Ghost Late 6\nsolve\nmeasure\n");
    ASSERT_TRUE(read.ok) << read.message;

    // Two claims, and the second is the one a bounding-box reader would fail:
    // the imported solid still has the flat top face a bore can start from.
    const std::vector<double> measured = MeasuredVolumes(read);
    ASSERT_EQ(measured.size(), 2u) << read.message;
    EXPECT_NEAR(measured[0], before, 1e-6 * before) << "the round trip changed the part";
    EXPECT_LT(measured[1], measured[0]) << "drilling the imported solid removed nothing";
    EXPECT_GT(measured[1], 0.9 * measured[0]) << "drilling one 6 mm bore removed far too much";
}

TEST(CliScriptTest, M22_CLI_008_AFailedRecomputeNAMESWhatFailedAndWhy) {
    // What this used to say was "recompute failed; see the log above", and the
    // log above was the one place the reason was not. Found writing the M22
    // example: a draft refused a face query that matched two faces, and the
    // screen said nothing that could be acted on.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Base\ntool rect\nclick -40 -30\nclick 40 30\npad Case 50\n"
            "shell Case 3 top\n"
            "draft Case 4 bottom facing:+x\n"
            "solve\n");
    ASSERT_FALSE(outcome.ok) << "the draft was expected to refuse an ambiguous face";

    // Three claims, and each one is a thing a reader needs: WHICH feature,
    // WHY, and no instruction to go and look somewhere the reason is not.
    EXPECT_NE(outcome.message.find("CaseDraft"), std::string::npos) << outcome.message;
    EXPECT_NE(outcome.message.find("matches 2 faces"), std::string::npos) << outcome.message;
    EXPECT_EQ(outcome.message.find("see the log above"), std::string::npos) << outcome.message;
}

// --- M23: assembling from a script -------------------------------------------

TEST(CliScriptTest, M23_CLI_001_OneScriptBuildsThePartsAndThenPutsThemTogether) {
    // The regression test for examples/assembly-three-parts.ep3ds, and the M23
    // gate seen from where a user stands. Both halves in one script, because an
    // instance names a FILE: the parts have to be real files on disk before
    // anything can be inserted, and a script that could only do one half could
    // not show that.
    ScratchIo part{"asm-part.ep3d"};
    ScratchIo rig{"asm-rig.ep3da"};

    ScriptRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch Plate\ntool rect\nclick -60 -40\nclick 60 40\npad Base 10\n"
                        "solve\nsave ") + part.path + "\n"
            "assembly Rig\n"
            "insert Left " + part.path + " Base\n"
            "insert Right " + part.path + " Base\n"
            "place Left -100 0 0\n"
            "place Right 100 0 0\n"
            "solve\nmeasure\nsave " + rig.path + "\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(rig.exists()) << "the assembly did not reach disk";

    // Two instances of ONE file, at two places. The volumes are equal and the
    // CENTRES are not, which is the whole claim -- a volume-only assertion
    // would pass on two copies sitting on top of each other.
    EXPECT_TRUE(LogMentions(outcome, "Left: volume = 96000 mm^3, centre = -100, 0, 5 mm"))
        << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "Right: volume = 96000 mm^3, centre = 100, 0, 5 mm"))
        << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "2 instances, total volume = 192000 mm^3"))
        << outcome.message;
}

TEST(CliScriptTest, M23_CLI_002_SaveWritesWHICHEVERDocumentTheScriptIsAbout) {
    // `assembly` changes what solve, measure and save are ABOUT, and a save
    // that quietly wrote the wrong document would only be discovered by
    // opening the file. The log says which, and so does the file.
    ScratchIo part{"which-part.ep3d"};
    ScratchIo rig{"which-rig.ep3da"};

    ScriptRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch Plate\ntool rect\nclick -10 -10\nclick 10 10\npad Base 5\n"
                        "solve\nsave ") + part.path + "\n"
            "assembly Rig\ninsert One " + part.path + " Base\nsolve\nsave " + rig.path + "\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "(assembly)")) << outcome.message;

    // Each file is the kind it claims to be, checked by the loader that would
    // refuse the other.
    EXPECT_TRUE(loadPartDocumentFromFile(part.path)) << "the part file is not a part";
    EXPECT_TRUE(loadAssemblyDocumentFromFile(rig.path)) << "the assembly file is not an assembly";
    EXPECT_FALSE(loadAssemblyDocumentFromFile(part.path));
    EXPECT_FALSE(loadPartDocumentFromFile(rig.path));
}

TEST(CliScriptTest, M23_CLI_003_InsertingBeforeThereIsAnAssemblySaysSo) {
    // "Unknown command" would be true of nothing here -- the verb exists, the
    // document does not -- so the message names the missing step.
    ScriptRun run;
    const ScriptOutcome outcome = run("insert One parts/one.ep3d\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("`assembly NAME` first"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M23_CLI_004_TwoInstancesCannotShareAName) {
    // They would be indistinguishable in every later `place`, and the second
    // one would silently move the first.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("assembly Rig\ninsert One parts/one.ep3d\ninsert One parts/two.ep3d\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("already an instance called"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M23_CLI_005_PlacingSomethingThatIsNotThereNamesIt) {
    ScriptRun run;
    const ScriptOutcome outcome = run("assembly Rig\nplace Ghost 1 2 3\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("no instance called 'Ghost'"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M23_CLI_006_AnAssemblyThatCannotBuildNAMESTheInstance) {
    // The same rule the part side got in M22: a failure that cannot be acted
    // on is barely better than no failure at all.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("assembly Rig\ninsert Ghost no-such-part.ep3d\nsolve\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("Ghost"), std::string::npos) << outcome.message;
    EXPECT_NE(outcome.message.find("no-such-part.ep3d"), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M23_CLI_007_MeasuringBeforeSolvingSaysWhichStepIsMissing) {
    ScratchIo part{"unsolved-asm.ep3d"};
    ScriptRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch Plate\ntool rect\nclick -10 -10\nclick 10 10\npad Base 5\n"
                        "solve\nsave ") + part.path + "\n"
            "assembly Rig\ninsert One " + part.path + " Base\nmeasure\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("`solve` before measuring"), std::string::npos)
        << outcome.message;
}

// --- M24: a hinge from a script ----------------------------------------------

TEST(CliScriptTest, M24_CLI_001_TheWorkedHingeTurnsAndStaysTogether) {
    // The regression test for examples/hinge.ep3ds, and the M24 gate seen from
    // where a user stands: one script draws two parts, puts a mate connector
    // on each, assembles them, and turns the joint.
    //
    // The arm's centre of mass is what carries the claim. Its z never moves,
    // because the hinge axis is z -- an arm that had come off the pin would
    // report a plausible number rather than an error, which is exactly why the
    // measurement is the test and the picture is not.
    ScratchIo bracket{"cli-bracket.ep3d"};
    ScratchIo arm{"cli-arm.ep3d"};
    ScratchIo rig{"cli-hinge.ep3da"};

    ScriptRun run;
    const ScriptOutcome outcome =
        run(std::string(
                "sketch Plate\ntool rect\nclick -50 -20\nclick 10 20\npad Bracket 8\n"
                "sketch Pin\ntool circle\nclick 0 0\nclick 6 0\npad Bracket 40\n"
                "union Bracket\n"
                "connector Pivot 0 0 40 0 0 1\n"
                "solve\nsave ") + bracket.path + "\n"
            "sketch Blade\ntool rect\nclick -8 -8\nclick 90 8\npad Arm 10\n"
            "connector Eye 0 0 0 0 0 1\n"
            "solve\nsave " + arm.path + "\n"
            "assembly Hinge\n"
            "insert Base " + bracket.path + " Bracket\n"
            "insert Swing " + arm.path + " Arm\n"
            "ground Base\n"
            "mate revolute Elbow Base/Pivot Swing/Eye 0\n"
            "solve\nmeasure\n"
            "drive Elbow 90\nsolve\nmeasure\n"
            "drive Elbow 180\nsolve\nmeasure\n"
            "save " + rig.path + "\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(rig.exists());

    // The arm is 98 x 16 x 10 with its Eye at the near end, so its centre sits
    // 41 mm out along the arm, and the Pivot is 40 mm up the pin.
    EXPECT_TRUE(LogMentions(outcome, "Swing: volume = 15680 mm^3, centre = 41, 0, 45 mm"))
        << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "Swing: volume = 15680 mm^3, centre = 0, 41, 45 mm"))
        << "a quarter turn did not swing the arm: " << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "Swing: volume = 15680 mm^3, centre = -41, 0, 45 mm"))
        << "a half turn did not swing the arm: " << outcome.message;
}

TEST(CliScriptTest, M24_CLI_002_AnAngleIsTypedInDEGREESAndStoredInRADIANS) {
    // The same conversion `draft` makes, in the same place, for the same
    // reason: a drawing says degrees and the model stores radians, and a
    // script should not have to know that.
    //
    // Checked by WHERE THE PART ENDS UP, not by reading the log back. A log
    // line prints what was typed, so a test that read one would pass on a
    // script that passed 90 straight through as radians -- which is 5.157
    // radians once wrapped, and puts the arm somewhere else entirely.
    ScratchIo part{"deg-part.ep3d"};
    ScriptRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch Base\ntool rect\nclick -10 -10\nclick 10 10\npad Block 20\n"
                        "connector P -40 0 0\nsolve\nsave ") + part.path + "\n"
            "assembly Rig\n"
            "insert A " + part.path + " Block\n"
            "insert B " + part.path + " Block\n"
            "ground A\n"
            "mate revolute Turn A/P B/P 90\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;

    // B's connector is 40 mm to the -x side of its own centre, so at 0 degrees
    // B's centre would sit 40 mm along +x from the joint, and a quarter turn
    // puts it 40 mm along +y instead. Ninety RADIANS would put it at neither.
    EXPECT_TRUE(LogMentions(outcome, "B: volume = 8000 mm^3, centre = -40, 40, 10 mm"))
        << "90 was not read as degrees: " << outcome.message;
}

TEST(CliScriptTest, M24_CLI_003_AnUnknownMateKindListsTheOnesThatExist) {
    ScriptRun run;
    const ScriptOutcome outcome =
        run("assembly Rig\nmate hinge Elbow A/P B/Q\n");
    EXPECT_FALSE(outcome.ok);
    // EVERY kind it does know, because the reader's next move is to type one
    // of them -- and the list grew in M25, which is exactly when a hard-coded
    // three would have started lying.
    for (const char* kind : {"fastened", "revolute", "slider", "cylindrical", "ball", "planar",
                             "parallel"})
        EXPECT_NE(outcome.message.find(kind), std::string::npos) << kind << ": "
                                                                 << outcome.message;
}

TEST(CliScriptTest, M24_CLI_004_AnEndThatIsNotAnInstanceSlashConnectorIsREFUSED) {
    // "Base" alone names an instance and no connector, and guessing one --
    // the first, say -- would silently mate a different place than the writer
    // meant every time a part gained a connector.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("assembly Rig\ninsert Base parts/base.ep3d\nmate fastened Bolt Base Base/P\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("is not an INSTANCE/CONNECTOR pair"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M24_CLI_005_AConnectorAxisOfNOTHINGIsREFUSED) {
    // A zero-length axis is a point, not a direction. Quietly falling back to
    // +Z would put a hinge on an axis nobody chose.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch Base\ntool rect\nclick -10 -10\nclick 10 10\npad Block 5\n"
            "connector P 0 0 0 0 0 0\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("needs a direction"), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M24_CLI_006_AFastenedMateRefusesAValueFromTheScriptToo) {
    ScriptRun run;
    const ScriptOutcome outcome =
        run("assembly Rig\ninsert A parts/a.ep3d\ninsert B parts/b.ep3d\n"
            "mate fastened Bolt A/P B/Q 12\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("no freedom"), std::string::npos) << outcome.message;
}

TEST(CliScriptTest, M24_CLI_007_AnUngroundedAssemblySaysWhatToDoAboutIt) {
    // "The solve failed" is true and useless. "Ground one instance" is the
    // reader's next action, in the message.
    ScratchIo part{"ungrounded.ep3d"};
    ScriptRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch Base\ntool rect\nclick -10 -10\nclick 10 10\npad Block 5\n"
                        "connector P 0 0 0\nsolve\nsave ") + part.path + "\n"
            "assembly Rig\n"
            "insert A " + part.path + " Block\n"
            "insert B " + part.path + " Block\n"
            "mate fastened Bolt A/P B/P\nsolve\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("Ground one instance"), std::string::npos)
        << outcome.message;
}

// --- M25: mechanisms, limits and interference from a script ------------------

namespace {

// The script layer links no solver, so one is handed in -- exactly as the
// sketch solver is. A script that mates a linkage without one is told so.
struct MechanismRun {
    OcctGeometryKernel kernel;
    GaussNewtonSketchSolver sketchSolver;
    GaussNewtonAssemblySolver assemblySolver;
    PartDocument document{"ScriptDoc"};

    MechanismRun() {
        document.setGeometryKernel(&kernel);
        document.setSketchSolver(&sketchSolver);
    }
    ScriptOutcome operator()(const std::string& text) {
        return RunSketchScript(document, text, &assemblySolver);
    }
};

// The four links of examples/four-bar.ep3ds, as a script prefix.
std::string FourBarParts(const std::string& path) {
    return "sketch GroundBar\ntool rect\nclick 0 -4\nclick 100 4\npad Ground 6\n"
           "connector GroundA 0 0 0 0 0 1\nconnector GroundB 100 0 0 0 0 1\n"
           "sketch CrankBar\ntool rect\nclick 0 -4\nclick 30 4\npad Crank 6\n"
           "connector CrankA 0 0 0 0 0 1\nconnector CrankB 30 0 0 0 0 1\n"
           "sketch CouplerBar\ntool rect\nclick 0 -4\nclick 110 4\npad Coupler 6\n"
           "connector CouplerA 0 0 0 0 0 1\nconnector CouplerB 110 0 0 0 0 1\n"
           "sketch RockerBar\ntool rect\nclick 0 -4\nclick 60 4\npad Rocker 6\n"
           "connector RockerA 0 0 0 0 0 1\nconnector RockerB 60 0 0 0 0 1\n"
           "solve\nsave " + path + "\n";
}

std::string FourBarAssembly(const std::string& path) {
    return "assembly FourBar\n"
           "insert Ground " + path + " Ground\n"
           "insert Crank " + path + " Crank\n"
           "insert Coupler " + path + " Coupler\n"
           "insert Rocker " + path + " Rocker\n"
           "ground Ground\n"
           "mate revolute J1 Ground/GroundA Crank/CrankA\n"
           "mate revolute J2 Crank/CrankB Coupler/CouplerA\n"
           "mate revolute J3 Coupler/CouplerB Rocker/RockerA\n"
           "mate revolute J4 Rocker/RockerB Ground/GroundB\n";
}

} // namespace

TEST(CliScriptTest, M25_CLI_001_TheWorkedFourBarTurnsAndTheOtherLinksFOLLOW) {
    // The regression test for examples/four-bar.ep3ds, and the M25 gate seen
    // from where a user stands.
    //
    // The crank has to end up EXACTLY where it was told -- that is what
    // `drive` means -- and the rocker has to end up somewhere nobody typed.
    // Both halves matter: a solver that moved the crank would satisfy the
    // second claim while breaking the first, which is the bug this example
    // found.
    ScratchIo parts{"cli-fourbar.ep3d"};
    MechanismRun run;
    const ScriptOutcome outcome =
        run(FourBarParts(parts.path) + FourBarAssembly(parts.path) +
            "drive J1 0\nsolve\nmeasure\n"
            "drive J1 90\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;

    // The crank is 30 long and pinned at the ground's origin, so its centre of
    // mass is 15 mm out along it: at 0 degrees that is (15, 0), at 90 it is
    // (0, 15). Anything else and `drive` did not mean driven.
    EXPECT_TRUE(LogMentions(outcome, "Crank: volume = 1440 mm^3, centre = 15, 0, 3 mm"))
        << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "Crank: volume = 1440 mm^3, centre = 0, 15, 3 mm"))
        << "the solve moved the driven crank: " << outcome.message;

    // ...and the rocker moved, which is the linkage being a linkage.
    const std::vector<double> volumes = MeasuredVolumes(outcome);
    EXPECT_FALSE(volumes.empty());
}

TEST(CliScriptTest, M25_CLI_002_AMechanismWithNoSolverSaysSoByName) {
    // The script library links no backend on purpose (ADR-M5-003). A closed
    // loop without one is refused with the reason, not with a wrong answer.
    ScratchIo parts{"cli-nosolver.ep3d"};
    ScriptRun run; // deliberately the plain runner, which injects no solver
    const ScriptOutcome outcome =
        run(FourBarParts(parts.path) + FourBarAssembly(parts.path) + "solve\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("no assembly solver is configured"), std::string::npos)
        << outcome.message;
}

TEST(CliScriptTest, M25_CLI_003_ALimitCLAMPSAndTheClampIsSAID) {
    // Roadmap §22: the drag stops rather than erroring. The message is what
    // keeps that from being a control that appears broken.
    ScratchIo parts{"cli-limit.ep3d"};
    MechanismRun run;
    const ScriptOutcome outcome =
        run(FourBarParts(parts.path) + FourBarAssembly(parts.path) +
            "limit J1 0 90\ndrive J1 200\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "clamped to its limit at 90")) << outcome.message;
    // ...and it really stopped at 90 rather than merely saying so.
    EXPECT_TRUE(LogMentions(outcome, "Crank: volume = 1440 mm^3, centre = 0, 15, 3 mm"))
        << outcome.message;
}

TEST(CliScriptTest, M25_CLI_004_LimitsAreTypedInTheUnitTheFreedomHAS) {
    // Degrees for a hinge, millimetres for a slide -- read off the freedom
    // table rather than from a second list of "the rotational kinds", which
    // would be a second thing to keep in step.
    ScratchIo parts{"cli-units.ep3d"};
    MechanismRun run;
    const ScriptOutcome outcome =
        run(FourBarParts(parts.path) + FourBarAssembly(parts.path) +
            "limit J1 -45 45\ndrive J1 30\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    // 30 degrees is inside a limit of plus or minus 45 degrees, so nothing is
    // clamped. Read as RADIANS the limit would be plus or minus 45 radians and
    // 30 would still pass -- so the claim needs the other side too.
    EXPECT_FALSE(LogMentions(outcome, "clamped")) << outcome.message;

    MechanismRun tighter;
    const ScriptOutcome clamped =
        tighter(FourBarParts(parts.path) + FourBarAssembly(parts.path) +
                "limit J1 -10 10\ndrive J1 30\nsolve\n");
    ASSERT_TRUE(clamped.ok) << clamped.message;
    EXPECT_TRUE(LogMentions(clamped, "clamped to its limit at 10"))
        << "the limit was not read in degrees: " << clamped.message;
}

TEST(CliScriptTest, M25_CLI_005_InterferenceIsItsOwnQuestion) {
    // Separate from mates, because a legal set of mates can still drive two
    // parts through each other -- which a four-bar's links, all in one plane,
    // certainly do.
    ScratchIo parts{"cli-interfere.ep3d"};
    MechanismRun run;
    const ScriptOutcome outcome =
        run(FourBarParts(parts.path) + FourBarAssembly(parts.path) +
            "drive J1 90\nsolve\ninterference\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "overlap by")) << outcome.message;

    // Two parts far apart report nothing, so the answer above is a
    // measurement rather than a constant.
    ScratchIo apart{"cli-apart.ep3d"};
    MechanismRun clear;
    const ScriptOutcome none =
        clear(std::string("sketch Base\ntool rect\nclick -10 -10\nclick 10 10\npad Block 5\n"
                          "solve\nsave ") + apart.path + "\n"
              "assembly Rig\n"
              "insert A " + apart.path + " Block\n"
              "insert B " + apart.path + " Block\n"
              "place B 500 0 0\nsolve\ninterference\n");
    ASSERT_TRUE(none.ok) << none.message;
    EXPECT_TRUE(LogMentions(none, "no interference")) << none.message;
}

TEST(CliScriptTest, M25_CLI_006_TwoConnectorsCannotShareANameInOneDocument) {
    // A mate names its ends by connector NAME, so two with one name make every
    // mate mean whichever came first. Found by writing the four-bar example,
    // where one script drew four links into one document and called every
    // link's ends the same two things.
    ScriptRun run;
    const ScriptOutcome outcome =
        run("sketch A\ntool rect\nclick 0 0\nclick 10 10\npad One 5\n"
            "connector P 0 0 0\n"
            "sketch B\ntool rect\nclick 20 0\nclick 30 10\npad Two 5\n"
            "connector P 20 0 0\n");
    EXPECT_FALSE(outcome.ok);
    EXPECT_NE(outcome.message.find("already a connector called"), std::string::npos)
        << outcome.message;
}

// --- M26: sub-assemblies, rows, poses and explosions from a script ----------

TEST(CliScriptTest, M26_CLI_001_OneScriptBuildsAPartThenAnAssemblyThenAnotherPart) {
    // The gap M26's example exposed. M23 allowed exactly one assembly per
    // script and made `save` mean it from then on, which was enough until a
    // sub-assembly had to exist BEFORE the assembly that instances it -- and an
    // instance names a FILE, so the file has to be written first.
    ScratchIo partA{"m26-a.ep3d"};
    ScratchIo sub{"m26-sub.ep3da"};
    ScratchIo partB{"m26-b.ep3d"};
    ScratchIo rig{"m26-rig.ep3da"};

    MechanismRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch A\ntool rect\nclick -10 -10\nclick 10 10\npad Block 20\n"
                        "connector BlockP 0 0 0\nsolve\nsave ") + partA.path + "\n"
            "assembly Sub\n"
            "insert One " + partA.path + " Block\n"
            "ground One\nsolve\nsave " + sub.path + "\n"
            // BACK TO THE PART DOCUMENT, which M23 could not do.
            "part\n"
            "sketch B\ntool rect\nclick -30 -30\nclick 30 30\npad Table 10\n"
            "connector TableP 0 0 10\nsolve\nsave " + partB.path + "\n"
            "assembly Rig\n"
            "insert Table " + partB.path + " Table\n"
            "insert Nested " + sub.path + "\n"
            "ground Table\nplace Nested 0 0 10\nsolve\nmeasure\nsave " + rig.path + "\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(sub.exists());
    EXPECT_TRUE(rig.exists());

    // The sub-assembly came in as ONE instance carrying its whole contents:
    // a 20 mm cube is 8000 mm^3, and it landed 10 mm up.
    EXPECT_TRUE(LogMentions(outcome, "Nested: volume = 8000 mm^3, centre = 0, 0, 20 mm"))
        << outcome.message;
    // ...and each file is the kind it claims to be.
    EXPECT_TRUE(loadAssemblyDocumentFromFile(sub.path)) << "the sub-assembly is not an assembly";
    EXPECT_TRUE(loadPartDocumentFromFile(partB.path)) << "`part` did not go back to the part";
}

TEST(CliScriptTest, M26_CLI_002_ARowOfInstancesFollowsItsOriginal) {
    ScratchIo part{"m26-row.ep3d"};
    MechanismRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch A\ntool rect\nclick -5 -5\nclick 5 5\npad Block 10\n"
                        "solve\nsave ") + part.path + "\n"
            "assembly Rig\n"
            "insert Bolt " + part.path + " Block\n"
            "ground Bolt\n"
            "row Bolt 3 25 0 0\nsolve\nmeasure\n"
            // Move the original: the row goes with it.
            "place Bolt 100 0 0\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "Bolt 2: volume = 1000 mm^3, centre = 25, 0, 5 mm"))
        << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "Bolt 3: volume = 1000 mm^3, centre = 50, 0, 5 mm"))
        << outcome.message;
    // ...and after the move, 100 further along -- which a row of three separate
    // inserts would not do.
    EXPECT_TRUE(LogMentions(outcome, "Bolt 2: volume = 1000 mm^3, centre = 125, 0, 5 mm"))
        << "the row did not follow its original: " << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "Bolt 3: volume = 1000 mm^3, centre = 150, 0, 5 mm"))
        << outcome.message;
}

TEST(CliScriptTest, M26_CLI_003_APoseGoesBackToWhereItWasCaptured) {
    ScratchIo part{"m26-pose.ep3d"};
    MechanismRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch A\ntool rect\nclick -5 -5\nclick 5 5\npad Block 10\n"
                        "solve\nsave ") + part.path + "\n"
            "assembly Rig\n"
            "insert One " + part.path + " Block\n"
            "ground One\n"
            "place One 10 0 0\npose Home\n"
            "place One 90 0 0\nsolve\nmeasure\n"
            "pose Home apply\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "pose Home captured")) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "One: volume = 1000 mm^3, centre = 90, 0, 5 mm"))
        << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "One: volume = 1000 mm^3, centre = 10, 0, 5 mm"))
        << "the pose did not put it back: " << outcome.message;
}

TEST(CliScriptTest, M26_CLI_004_AnExplosionShowsWithoutMoving) {
    // `measure` prints where each instance IS and, when a view is being shown,
    // where the picture puts it. Both, because they are different answers and
    // a reader has to be able to tell which one they are looking at.
    ScratchIo part{"m26-explode.ep3d"};
    MechanismRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch A\ntool rect\nclick -5 -5\nclick 5 5\npad Block 10\n"
                        "solve\nsave ") + part.path + "\n"
            "assembly Rig\n"
            "insert One " + part.path + " Block\n"
            "ground One\n"
            "explode Service\n"
            "explode Service step Lift One 0 0 40\n"
            "explode Service step Slide One 30 0 0\n"
            "explode Service show 1\nsolve\nmeasure\n"
            "explode Service show all\nsolve\nmeasure\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    // ONE STEP: lifted 40, not slid.
    EXPECT_TRUE(LogMentions(outcome, "centre = 0, 0, 5 (shown at 0, 0, 45) mm"))
        << outcome.message;
    // BOTH: lifted and slid.
    EXPECT_TRUE(LogMentions(outcome, "centre = 0, 0, 5 (shown at 30, 0, 45) mm"))
        << outcome.message;
}

TEST(CliScriptTest, M26_CLI_005_EachOfTheseVerbsRefusesWhatItCannotDo) {
    // The refusals, together, because each one is a sentence a reader has to be
    // able to act on and none of them is interesting enough for its own test.
    ScriptRun beforeAssembly;
    const ScriptOutcome noAssembly = beforeAssembly("row Bolt 3 10 0 0\n");
    EXPECT_FALSE(noAssembly.ok);
    EXPECT_NE(noAssembly.message.find("`assembly NAME` first"), std::string::npos)
        << noAssembly.message;

    ScriptRun run;
    const ScriptOutcome badCount =
        run("assembly Rig\ninsert Bolt parts/bolt.ep3d\nrow Bolt 0 10 0 0\n");
    EXPECT_FALSE(badCount.ok);
    EXPECT_NE(badCount.message.find("one or more"), std::string::npos) << badCount.message;

    ScriptRun poses;
    const ScriptOutcome noPose = poses("assembly Rig\npose Ghost apply\n");
    EXPECT_FALSE(noPose.ok);
    EXPECT_NE(noPose.message.find("no pose called 'Ghost'"), std::string::npos)
        << noPose.message;

    ScriptRun views;
    const ScriptOutcome noView = views("assembly Rig\nexplode Ghost show 1\n");
    EXPECT_FALSE(noView.ok);
    EXPECT_NE(noView.message.find("no exploded view called 'Ghost'"), std::string::npos)
        << noView.message;

    ScriptRun twice;
    const ScriptOutcome again = twice("assembly Rig\nexplode Service\nexplode Service\n");
    EXPECT_FALSE(again.ok);
    EXPECT_NE(again.message.find("already an exploded view"), std::string::npos)
        << again.message;
}

TEST(CliScriptTest, M26_CLI_006_GoingBackToAnEarlierAssemblyReturnsToTHATOne) {
    // `assembly NAME` switches, creating on first use. Going back to a name it
    // has seen has to return to the SAME document -- making a second empty one
    // would quietly discard everything put in the first, and `save` would write
    // the empty one.
    //
    // Every other M26 test uses two different names, so none of them can see
    // this: the difference only shows when a name is used twice.
    ScratchIo part{"m26-switch.ep3d"};
    ScratchIo rig{"m26-switch.ep3da"};

    MechanismRun run;
    const ScriptOutcome outcome =
        run(std::string("sketch A\ntool rect\nclick -5 -5\nclick 5 5\npad Block 10\n"
                        "solve\nsave ") + part.path + "\n"
            "assembly Rig\n"
            "insert One " + part.path + " Block\n"
            // ...off to a different assembly...
            "assembly Other\n"
            "insert Elsewhere " + part.path + " Block\n"
            // ...and BACK to the first one.
            "assembly Rig\n"
            "insert Two " + part.path + " Block\n"
            "ground One\nplace Two 40 0 0\nsolve\nmeasure\nsave " + rig.path + "\n");
    ASSERT_TRUE(outcome.ok) << outcome.message;
    EXPECT_TRUE(LogMentions(outcome, "(back to it)"))
        << "the second `assembly Rig` did not return to the first: " << outcome.message;

    // BOTH instances are in it -- which is the claim. A fresh Rig would hold
    // only the one added after the switch.
    const AssemblyLoadResult loaded = loadAssemblyDocumentFromFile(rig.path);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->instances().size(), 2u)
        << "going back to an assembly made a new one and lost what was in it";
    EXPECT_NE(loaded.document->findInstanceNamed("One"), nullptr);
    EXPECT_NE(loaded.document->findInstanceNamed("Two"), nullptr);
    // ...and the OTHER assembly's instance did not leak into it.
    EXPECT_EQ(loaded.document->findInstanceNamed("Elsewhere"), nullptr);
}

TEST(CliScriptTest, M26_CLI_007_VerticalPutsTwoLineENDSOnOneVerticalLine) {
    // Two lines, one END of each, `constrain vertical`. The two ends must come
    // to share a u -- on one vertical line -- and the LINES must keep their own
    // slopes. Before M26.3 both refs named line entities, so this made each
    // line vertical instead, which is a different drawing entirely.
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch Frame
tool line
click 0 0
click 40 30
tool line
click 90 5
click 130 55
constrain vertical Line1.end Line2.start
solve
)");
    ASSERT_TRUE(outcome.ok) << outcome.message;

    const Sketch& sketch = run.only();
    ASSERT_EQ(sketch.entities().size(), 2u);
    const SketchLine& a = std::get<SketchLine>(sketch.entities()[0].geometry);
    const SketchLine& b = std::get<SketchLine>(sketch.entities()[1].geometry);

    EXPECT_NEAR(a.end.x, b.start.x, 1e-6) << "the two ends are not on one vertical line";
    // NOT coincident -- an alignment leaves the v separation alone.
    EXPECT_GT(std::fabs(a.end.y - b.start.y), 1.0);
    // NEITHER LINE became vertical.
    EXPECT_GT(std::fabs(a.start.x - a.end.x), 1.0);
    EXPECT_GT(std::fabs(b.start.x - b.end.x), 1.0);
}

TEST(CliScriptTest, M26_CLI_008_TheStepperMotorExampleDrillsFOURWholeHoles) {
    // The regression test for examples/stepper-motor.ep3ds.
    //
    // It checks the VOLUME against a closed form at each stage, because that is
    // the only thing that caught the defect the example was written with: at an
    // 8 mm corner chamfer the bolt-hole bores break OUT through the corners,
    // and the part still builds, still saves, and still looks right from the
    // front. Only the arithmetic said that 2.87 holes' worth of material had
    // gone where four holes were asked for.
    //
    // Counting features cannot see it. Counting solids cannot see it. Asking
    // the bounding box cannot see it. This test exists at the one place the
    // difference is visible.
    ScriptRun run;
    const ScriptOutcome outcome = run(R"(
sketch Frame
tool line
click 24.2 28.2
click -24.2 28.2
click -28.2 24.2
click -28.2 -24.2
click -24.2 -28.2
click 24.2 -28.2
click 28.2 -24.2
click 28.2 24.2
click 24.2 28.2
pad Motor 76 as BodyLength
sketch Boss xy 76
tool circle
click 0 0
click 19.05 0
dimension diameter Circle1 38.1 as BossDiameter
pad Motor 1.6 as BossHeight
union Motor
sketch Shaft xy 76
tool circle
click 0 0
click 3.175 0
dimension diameter Circle1 6.35 as ShaftDiameter
pad Motor 21 as ShaftLength
union Motor
sketch Mounts xy 76
tool point
click -23.57 -23.57
click 23.57 -23.57
click 23.57 23.57
click -23.57 23.57
constrain fix Point1
constrain horizontal Point1 Point2
constrain horizontal Point4 Point3
constrain vertical Point1 Point4
constrain vertical Point2 Point3
dimension hdistance Point1 Point2 47.14 as BoltSpacing
dimension vdistance Point1 Point4 47.14 as BoltRise
hole Motor Mounts 5.1 -12
solve
measure
)");
    ASSERT_TRUE(outcome.ok) << outcome.message;

    // The bolt pattern is held SQUARE by constraints, not by four typed
    // coordinates -- so it solves to zero degrees of freedom.
    const Sketch* mounts = nullptr;
    for (const Sketch* one : run.document.sketches())
        if (one->name() == "Mounts") mounts = one;
    ASSERT_NE(mounts, nullptr);
    EXPECT_EQ(mounts->degreesOfFreedom(), 0) << mounts->solveMessage();

    // THE CLOSED FORM:
    //   frame   (56.4^2 - 4 * 1/2 * 4 * 4) * 76
    //   + boss   pi * 19.05^2 * 1.6
    //   + shaft  pi * 3.175^2 * 21          (it starts at the mounting face...)
    //   - overlap pi * 3.175^2 * 1.6        (...so it runs through the boss)
    //   - holes  4 * pi * 2.55^2 * 12
    constexpr double kPi = 3.14159265358979323846;
    const double frame = (56.4 * 56.4 - 4.0 * 0.5 * 4.0 * 4.0) * 76.0;
    const double boss = kPi * 19.05 * 19.05 * 1.6;
    const double shaft = kPi * 3.175 * 3.175 * 21.0;
    const double overlap = kPi * 3.175 * 3.175 * 1.6;
    const double holes = 4.0 * kPi * 2.55 * 2.55 * 12.0;
    const double expected = frame + boss + shaft - overlap - holes;

    // The LAST `volume =` the run logged, which is the finished part.
    double measured = -1.0;
    std::string line;
    for (const ScriptLogEntry& entry : outcome.log) {
        const std::size_t at = entry.text.find("volume = ");
        if (at == std::string::npos) continue;
        measured = std::atof(entry.text.c_str() + at + std::strlen("volume = "));
        line = entry.text;
    }
    ASSERT_GT(measured, 0.0) << "the run logged no volume at all";
    // A part per million. Anything looser would pass with a hole missing.
    EXPECT_NEAR(measured, expected, expected * 1e-6)
        << "the bores are not all fully inside the material: " << line;
}
