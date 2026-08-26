// M46 -- the clash answer knows when it stopped being true.
//
// This is what came out of asking whether to build CONTACT SOLVING, and the
// answer was no. A contact is `gap >= 0` -- an inequality -- and everything
// the mate solve does drives residuals to zero. It would need an active set
// (guess which contacts touch, solve those, check the rest, repeat), and the
// guess can legitimately change mid-drag, which is how such solvers chatter.
// Worse, the residual would need GEOMETRY: measureInterference returns a
// volume, which is zero for every pair that is not already overlapping and so
// has no gradient to descend. A signed distance query is what is missing, and
// a B-rep distance per iteration per drag frame is milliseconds where the mate
// solve costs microseconds.
//
// What the user actually loses today is smaller and completely fixable: they
// check for clashes, get "none", drag a link through a wall, and the answer on
// the screen still says none. Nothing lied -- the answer was true when it was
// given, and nothing said when it stopped being.
//
// So: the same shape as a drawing view that is behind its model (M32). The
// answer is remembered, and anything that moves the assembly makes it stale.
// NOT CHECKED and CLEAR are different answers and only one is worth anything.

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Document/PartDocument.h"
#include "Core/Library/StandardParts.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

using namespace paramcad;

struct Scratch {
    std::string path;
    explicit Scratch(const char* name) {
        path = (std::filesystem::temp_directory_path() / (std::string("ep3d-clash-") + name))
                   .string();
        std::remove(path.c_str());
    }
    ~Scratch() { std::remove(path.c_str()); }
};

void WriteBlock(const std::string& path, double side) {
    PartDocument part{"Block"};
    Sketch& outline = part.addSketch("Base");
    part.addSketchEntity(outline.id(), SketchLine{Vec2{0, 0}, Vec2{side, 0}});
    part.addSketchEntity(outline.id(), SketchLine{Vec2{side, 0}, Vec2{side, side}});
    part.addSketchEntity(outline.id(), SketchLine{Vec2{side, side}, Vec2{0, side}});
    part.addSketchEntity(outline.id(), SketchLine{Vec2{0, side}, Vec2{0, 0}});
    Parameter& tall = part.addParameter("H", side, UnitType::Millimeter);
    Body& body = part.addBody("Block");
    part.addPadFeature(body, "Pad", outline.id(), tall.id());
    ASSERT_TRUE(savePartDocumentToFile(part, path));
}

TEST(InterferenceStalenessTest, M46_CLASH_001_NotCheckedIsNotTheSameAnswerAsClear) {
    // A freshly opened assembly has never been looked at, and an empty list of
    // overlaps must not read as a clean bill of health.
    OcctGeometryKernel kernel;
    Scratch block{"a.ep3d"};
    WriteBlock(block.path, 20.0);

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    assembly.addInstance("A", block.path, "Block");
    ASSERT_TRUE(assembly.recompute().success);

    EXPECT_TRUE(assembly.isInterferenceStale())
        << "an assembly nobody has checked claims to know about its clashes";
    EXPECT_TRUE(assembly.lastInterference().overlaps.empty());

    assembly.recheckInterference();
    EXPECT_FALSE(assembly.isInterferenceStale());
    EXPECT_TRUE(assembly.lastInterference().overlaps.empty());
}

TEST(InterferenceStalenessTest, M46_CLASH_002_MovingAnythingMakesTheAnswerOldAgain) {
    // THE FAILURE THIS EXISTS FOR. Check, get "none", move a part, and the
    // answer on the screen is still "none" -- true when it was given, and
    // nothing said when it stopped being.
    OcctGeometryKernel kernel;
    Scratch block{"b.ep3d"};
    WriteBlock(block.path, 20.0);

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    Instance& one = assembly.addInstance("A", block.path, "Block");
    Instance& two = assembly.addInstance("B", block.path, "Block");
    // Well apart to begin with.
    assembly.setInstanceTransform(two.id(), Transform3D{Vec3{100.0, 0.0, 0.0}, Quaternion{}});
    ASSERT_TRUE(assembly.recompute().success);

    ASSERT_TRUE(assembly.recheckInterference().ok);
    ASSERT_TRUE(assembly.lastInterference().overlaps.empty());
    ASSERT_FALSE(assembly.isInterferenceStale());

    // Drag the second one on top of the first.
    ASSERT_TRUE(assembly.setInstanceTransform(two.id(),
                                              Transform3D{Vec3{5.0, 0.0, 0.0}, Quaternion{}}));
    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_TRUE(assembly.isInterferenceStale())
        << "the assembly moved and the clash answer still claims to be current";

    // ...and asking again finds the clash that is now there.
    const AssemblyDocument::InterferenceReport& now = assembly.recheckInterference();
    ASSERT_TRUE(now.ok) << now.message;
    ASSERT_EQ(now.overlaps.size(), 1u) << "two blocks 5 mm apart on a 20 mm side do overlap";
    EXPECT_GT(now.overlaps.front().volumeMm3, 0.0);
    EXPECT_FALSE(assembly.isInterferenceStale());
    (void)one;
}

TEST(InterferenceStalenessTest, M46_CLASH_003_TheAnswerIsREMEMBEREDAndNotJustPrinted) {
    // checkInterference stays const and stateless for callers that only want
    // an answer. What the window uses is the one that remembers -- because a
    // printed answer nobody kept is how the screen came to disagree with the
    // assembly.
    OcctGeometryKernel kernel;
    Scratch block{"c.ep3d"};
    WriteBlock(block.path, 20.0);

    AssemblyDocument assembly{"Rig"};
    assembly.setGeometryKernel(&kernel);
    assembly.addInstance("A", block.path, "Block");
    Instance& two = assembly.addInstance("B", block.path, "Block");
    assembly.setInstanceTransform(two.id(), Transform3D{Vec3{5.0, 0.0, 0.0}, Quaternion{}});
    ASSERT_TRUE(assembly.recompute().success);

    // The stateless one answers and leaves nothing behind.
    const AssemblyDocument::InterferenceReport asked = assembly.checkInterference();
    EXPECT_EQ(asked.overlaps.size(), 1u);
    EXPECT_TRUE(assembly.isInterferenceStale())
        << "the const check quietly marked the assembly as checked";

    // The remembering one answers the same and keeps it.
    EXPECT_EQ(assembly.recheckInterference().overlaps.size(), 1u);
    EXPECT_FALSE(assembly.isInterferenceStale());
    EXPECT_EQ(assembly.lastInterference().overlaps.size(), 1u);
}

TEST(InterferenceStalenessTest, M46_CLASH_004_ABoltFromTheLibraryClashesLikeAnythingElse) {
    // M45's catalogue parts arrive through the same resolver, so they are
    // solids like any other -- including for this. Worth pinning, because "the
    // library part is special" is exactly the assumption that would make a
    // clash check quietly skip them.
    OcctGeometryKernel kernel;
    AssemblyDocument assembly{"Joint"};
    assembly.setGeometryKernel(&kernel);
    const std::optional<FastenerSpec> screw = LookUpFastener("ISO 4762 M8x30");
    ASSERT_TRUE(screw.has_value());
    assembly.addInstance("Screw1", StandardPartPath(*screw), "");
    Instance& second = assembly.addInstance("Screw2", StandardPartPath(*screw), "");
    ASSERT_TRUE(assembly.recompute().success);

    // Both at the origin: entirely inside one another.
    const AssemblyDocument::InterferenceReport onTop = assembly.recheckInterference();
    ASSERT_TRUE(onTop.ok) << onTop.message;
    EXPECT_EQ(onTop.overlaps.size(), 1u) << "two bolts in the same place did not clash";

    ASSERT_TRUE(assembly.setInstanceTransform(
        second.id(), Transform3D{Vec3{50.0, 0.0, 0.0}, Quaternion{}}));
    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_TRUE(assembly.isInterferenceStale());
    EXPECT_TRUE(assembly.recheckInterference().overlaps.empty())
        << "two bolts 50 mm apart still clash";
}

} // namespace
