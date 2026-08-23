// M17.27 -- the headless script interpreter.
//
// These are integration tests on purpose: a script's whole claim is that it
// drives the SAME path a mouse does, so checking it against anything but a real
// document, a real solver and a real kernel would be checking it against a stub.

#include "Cli/SketchScript.h"
#include "Core/Feature/DraftFeature.h"
#include "Core/Body/Body.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Kernel/Occt/OcctGeometryKernel.h"
#include "Solver/GaussNewtonSketchSolver.h"
#include "Viewer/SketchCanvas.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
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
