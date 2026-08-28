// M56.3/.4 -- a skeleton becomes steel, and the steel becomes a cut list.
//
// The failures this file is here to catch, in the order they would cost money:
//
//   * a mitre computed at the wrong end, so a frame that will not close
//   * a cut list that counted the members itself, and so agrees with the parts
//     list today and stops agreeing the day one is added
//   * a three-way joint mitred anyway, on a guess about which member runs
//     through
//   * a length read as the long point when the axis was meant, or the other
//     way round -- 40 mm on a 600 mm member, twice, on every stick

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Drawing/BomTable.h"
#include "Core/Frame/CutList.h"
#include "Core/Frame/FrameLayout.h"
#include "Core/Frame/FrameProfile.h"
#include "Core/Geometry/Transform.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>

namespace {

using namespace paramcad;

FrameProfile Section(std::string_view designation) {
    const std::optional<FrameProfile> found = LookUpSection(designation);
    EXPECT_TRUE(found.has_value()) << designation;
    return found.value_or(FrameProfile{});
}

// A 600 x 400 rectangle of centrelines, lying on the world XY plane, walked
// round in order so that every corner is a joint of exactly two.
std::vector<SkeletonLine> Rectangle(double width = 600.0, double height = 400.0) {
    const Vec3 corners[4] = {Vec3{0.0, 0.0, 0.0}, Vec3{width, 0.0, 0.0},
                             Vec3{width, height, 0.0}, Vec3{0.0, height, 0.0}};
    std::vector<SkeletonLine> lines;
    for (int i = 0; i < 4; ++i) lines.push_back(SkeletonLine{corners[i], corners[(i + 1) % 4]});
    return lines;
}

TEST(FrameGeneratorTest, M56_FRAME_001_ARectangleOfLinesComesBackAsFourMitredMembers) {
    const FrameLayout layout = GenerateFrame(Rectangle(), Section("SHS 40x40x3"));
    ASSERT_TRUE(layout.ok) << layout.why;
    ASSERT_EQ(layout.members.size(), 4u);
    EXPECT_TRUE(layout.notes.empty()) << layout.notes.front();

    for (const FrameMemberPlacement& member : layout.members) {
        // EVERY CORNER IS 45 AT BOTH ENDS. A mitre computed at one end and
        // defaulted at the other is the failure that leaves a frame open by
        // the width of the section, and both ends being 45 is what says it did
        // not happen.
        EXPECT_NEAR(member.spec.angleADeg, 45.0, 1e-9) << member.name;
        EXPECT_NEAR(member.spec.angleBDeg, 45.0, 1e-9) << member.name;
        // The axis length is the CENTRELINE length -- the skeleton's own -- and
        // the mitres reach 20 past it at each end on a 40 wide tube.
        EXPECT_NEAR(member.spec.longPointMm(), member.spec.lengthMm + 40.0, 1e-9);
        EXPECT_NEAR(member.spec.shortPointMm(), member.spec.lengthMm - 40.0, 1e-9);
    }
    EXPECT_NEAR(layout.members[0].spec.lengthMm, 600.0, 1e-9);
    EXPECT_NEAR(layout.members[1].spec.lengthMm, 400.0, 1e-9);

    // THE PATH IS THE MEMBER. Two members of the same length and cut share one,
    // which is what makes the cut list a way of reading the parts list.
    EXPECT_EQ(layout.members[0].sourcePath(), "frm:SHS 40x40x3 L=600 A=45 B=45");
    EXPECT_EQ(layout.members[0].sourcePath(), layout.members[2].sourcePath());
    EXPECT_NE(layout.members[0].sourcePath(), layout.members[1].sourcePath());
}

TEST(FrameGeneratorTest, M56_FRAME_002_TheMitreLeansTowardsTheOutsideOfTheCorner) {
    // THE SIGN THAT COST A REBUILD. A mitre has a hand: the long point belongs
    // on the OUTSIDE of the corner, and a convention that puts it on the inside
    // produces four members that each look right and cannot be welded up.
    //
    // The member's own +X is where the mitre arithmetic measures from, and the
    // long point sits at local x = -b/2. Placing that local point in the
    // assembly is what says which side of the rectangle it fell on.
    const FrameLayout layout = GenerateFrame(Rectangle(), Section("SHS 40x40x3"));
    ASSERT_TRUE(layout.ok) << layout.why;

    // Member 1 runs along +X at y = 0, and the frame's inside is y > 0.
    const FrameMemberPlacement& bottom = layout.members[0];
    const Vec3 longPoint = ApplyTransform(bottom.placement, Vec3{-20.0, 0.0, -20.0});
    EXPECT_NEAR(longPoint.y, -20.0, 1e-9)
        << "the mitre's long point landed inside the frame, so the corners will not close";
    EXPECT_NEAR(longPoint.x, -20.0, 1e-9);

    // ...and the short point is on the inside, 20 the other way.
    const Vec3 shortPoint = ApplyTransform(bottom.placement, Vec3{20.0, 0.0, 20.0});
    EXPECT_NEAR(shortPoint.y, 20.0, 1e-9);
    EXPECT_NEAR(shortPoint.x, 20.0, 1e-9);
}

TEST(FrameGeneratorTest, M56_FRAME_003_AThreeWayJointIsLeftSquareAndSAIDRatherThanGuessed) {
    // Which member runs through a three-way joint changes two lengths and two
    // cuts, and it is a fabrication decision. A generator that picked one would
    // produce a frame that looks finished.
    std::vector<SkeletonLine> skeleton = Rectangle();
    // The bottom rail split in two, with a mullion standing on the split.
    skeleton[0] = SkeletonLine{Vec3{0.0, 0.0, 0.0}, Vec3{300.0, 0.0, 0.0}};
    skeleton.push_back(SkeletonLine{Vec3{300.0, 0.0, 0.0}, Vec3{600.0, 0.0, 0.0}});
    skeleton.push_back(SkeletonLine{Vec3{300.0, 0.0, 0.0}, Vec3{300.0, 400.0, 0.0}});

    const FrameLayout layout = GenerateFrame(skeleton, Section("SHS 40x40x3"));
    ASSERT_TRUE(layout.ok) << layout.why;
    ASSERT_EQ(layout.members.size(), 6u);

    // THREE ENDS MEET AT (300, 0): the two halves of the rail and the mullion.
    // All three are left square, and all three say so.
    int threeWay = 0;
    for (const std::string& note : layout.notes)
        if (note.find("3 members meet here") != std::string::npos) {
            ++threeWay;
            EXPECT_NE(note.find("left cut square"), std::string::npos) << note;
        }
    EXPECT_EQ(threeWay, 3);

    const FrameMemberPlacement& mullion = layout.members.back();
    EXPECT_NEAR(mullion.spec.angleADeg, 90.0, 1e-9);
    EXPECT_EQ(mullion.sourcePath(), "frm:SHS 40x40x3 L=400");
}

TEST(FrameGeneratorTest, M56_FRAME_003B_AnEndLandingMidSpanIsNamedRatherThanIgnored) {
    // FOUND BY THE TEST ABOVE, whose first draft assumed a mullion running to
    // the middle of a rail was a joint. It is not one -- a joint here is a
    // point two ENDS share -- so this end was passing through as "free" and
    // being cut square, which on a centreline skeleton drives it half a section
    // deep into the rail it lands on. A real interference, produced quietly.
    std::vector<SkeletonLine> skeleton = Rectangle();
    skeleton.push_back(SkeletonLine{Vec3{300.0, 0.0, 0.0}, Vec3{300.0, 400.0, 0.0}});

    const FrameLayout layout = GenerateFrame(skeleton, Section("SHS 40x40x3"));
    ASSERT_TRUE(layout.ok) << layout.why;
    ASSERT_EQ(layout.members.size(), 5u);

    // BOTH of the mullion's ends land part way along a rail, and both are said.
    ASSERT_EQ(layout.notes.size(), 2u);
    for (const std::string& note : layout.notes) {
        EXPECT_NE(note.find("Member 5"), std::string::npos) << note;
        EXPECT_NE(note.find("lands part way along"), std::string::npos) << note;
        EXPECT_NE(note.find("run into the member it meets"), std::string::npos) << note;
    }
    // The rails themselves are untouched: an end sitting on a member's span is
    // not a joint FOR THAT MEMBER either, so their own mitres are unaffected.
    EXPECT_NEAR(layout.members[0].spec.angleADeg, 45.0, 1e-9);
    EXPECT_NEAR(layout.members[0].spec.angleBDeg, 45.0, 1e-9);
}

TEST(FrameGeneratorTest, M56_FRAME_004_ACompoundCutIsRefusedRatherThanApproximated) {
    // Two members meeting out of the plane of the section's up direction need a
    // cut in two axes, and a member's path carries ONE angle per end. Inventing
    // a second number would put something in the path nothing downstream can
    // read -- and, worse, a member that builds and is wrong.
    std::vector<SkeletonLine> skeleton;
    skeleton.push_back(SkeletonLine{Vec3{0.0, 0.0, 0.0}, Vec3{600.0, 0.0, 0.0}});
    // Away at 45 degrees between +Y and +Z, so neither member runs along the
    // up direction and the bisecting plane still leans out of it.
    skeleton.push_back(SkeletonLine{Vec3{600.0, 0.0, 0.0}, Vec3{600.0, 300.0, 300.0}});

    const FrameLayout layout =
        GenerateFrame(skeleton, Section("SHS 40x40x3"), Vec3{0.0, 0.0, 1.0});
    ASSERT_TRUE(layout.ok) << layout.why;
    ASSERT_FALSE(layout.notes.empty());
    EXPECT_NE(layout.notes.front().find("compound cut"), std::string::npos)
        << layout.notes.front();
    // Member 1 keeps a square end rather than a mitre nobody can cut.
    EXPECT_NEAR(layout.members[0].spec.angleBDeg, 90.0, 1e-9);

    // AND MEMBER 2 DOES NOT, WHICH IS NOT A BUG. Whether a cut is compound is
    // a question about the MEMBER, not about the joint: it asks whether the
    // bisecting plane lies along that member's own section axis, and the two
    // members here are rolled differently. Member 2's does, so member 2 gets
    // an ordinary mitre and member 1 does not, and the joint comes out half
    // cut -- which is exactly the state the note describes and the state a
    // fabricator has to resolve.
    EXPECT_NEAR(std::fabs(layout.members[1].spec.angleADeg - 90.0), 45.0, 1e-9)
        << "member 2's end was refused too, so the check is asking about the joint "
           "rather than about the member";

    // AND THE SAME FRAME WITH THE SECTION ROLLED IS FINE, which is what makes
    // this a limit of the CUT and not of the geometry. Rolling the up
    // direction into the joint's own plane puts the mitre back in one axis.
    const FrameLayout rolled =
        GenerateFrame(skeleton, Section("SHS 40x40x3"), Vec3{0.0, 1.0, -1.0});
    ASSERT_TRUE(rolled.ok) << rolled.why;
    EXPECT_TRUE(rolled.notes.empty())
        << "the same joint was still called compound with the section rolled: "
        << rolled.notes.front();
    // MEASURED AS THE ANGLE OFF SQUARE, because 45 and 135 are the same saw
    // setting leaning opposite ways and which one comes out depends on which
    // way the section's own +X ended up pointing. What a shop is promised is
    // that the cut is 45 degrees off square, and that is what is checked.
    EXPECT_NEAR(std::fabs(rolled.members[0].spec.angleBDeg - 90.0), 45.0, 1e-9);
    EXPECT_NEAR(std::fabs(rolled.members[1].spec.angleADeg - 90.0), 45.0, 1e-9);
}

TEST(FrameGeneratorTest, M56_FRAME_005_AMemberRunningAlongTheUpDirectionIsRefusedWithTheReason) {
    std::vector<SkeletonLine> skeleton;
    skeleton.push_back(SkeletonLine{Vec3{0.0, 0.0, 0.0}, Vec3{0.0, 0.0, 400.0}});
    const FrameLayout layout =
        GenerateFrame(skeleton, Section("SHS 40x40x3"), Vec3{0.0, 0.0, 1.0});
    EXPECT_FALSE(layout.ok);
    EXPECT_NE(layout.why.find("no way up"), std::string::npos) << layout.why;
    // The message says what to do about it, because there is something to do.
    EXPECT_NE(layout.why.find("different up direction"), std::string::npos) << layout.why;
}

TEST(FrameGeneratorTest, M56_FRAME_006_AnOverhangIsMeasuredACROSSTheCutAndNotAlongIt) {
    // FOUND BY THE MUTATION GATE: every mitred member up to here was square,
    // round or an equal angle -- sections whose height and width are the same
    // number -- so measuring the overhang across the wrong one passed all of
    // them. A rectangular tube is the commonest frame section there is.
    //
    // The cut turns about the section's own up axis, so what the mitre reaches
    // past the joint is set by how wide the section is ACROSS that turn, and
    // not by how deep it is along it.
    std::vector<SkeletonLine> skeleton = Rectangle();
    const FrameLayout layout = GenerateFrame(skeleton, Section("RHS 60x40x3"));
    ASSERT_TRUE(layout.ok) << layout.why;

    // RHS 60x40x3 is 60 deep and 40 wide, and it is laid with its width across
    // the cut -- so a 45 degree mitre reaches 20 past the joint, not 30.
    const FrameMemberSpec& rail = layout.members[0].spec;
    EXPECT_NEAR(rail.profile.widthMm, 40.0, 1e-9);
    EXPECT_NEAR(rail.profile.heightMm, 60.0, 1e-9);
    EXPECT_NEAR(rail.overhangAMm(), 20.0, 1e-9)
        << "the overhang was measured along the section's depth instead of across its width";
    EXPECT_NEAR(rail.longPointMm(), 640.0, 1e-9);
    EXPECT_EQ(layout.members[0].sourcePath(), "frm:RHS 60x40x3 L=600 A=45 B=45");
}

TEST(FrameGeneratorTest, M56_CUT_001_TheCutListIsTheParTSListReadDifferently) {
    // THE THESIS OF THE MILESTONE. Not "the two agree" -- they cannot disagree,
    // because there is one count and this is a way of reading it.
    AssemblyDocument assembly{"Trolley"};
    const FrameLayout layout = GenerateFrame(Rectangle(), Section("SHS 40x40x3"));
    ASSERT_TRUE(layout.ok) << layout.why;
    ASSERT_EQ(PlaceFrame(assembly, layout), 4u);

    const BomContents parts = CountAssembly(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(parts.ok) << parts.why;
    const CutListContents cuts = CutListOf(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(cuts.ok) << cuts.why;

    // Two rows, because two rails and two stiles: 2 x 600 and 2 x 400.
    ASSERT_EQ(cuts.rows.size(), 2u);
    EXPECT_EQ(parts.rows.size(), 2u);
    EXPECT_EQ(cuts.rows[0].quantity, 2);
    EXPECT_EQ(cuts.rows[1].quantity, 2);
    EXPECT_NEAR(cuts.rows[0].axisLengthMm(), 600.0, 1e-9);
    EXPECT_NEAR(cuts.rows[0].longPointMm(), 640.0, 1e-9);
    EXPECT_NEAR(cuts.rows[0].shortPointMm(), 560.0, 1e-9);
    EXPECT_NEAR(cuts.rows[0].spec.angleADeg, 45.0, 1e-9);
    EXPECT_EQ(cuts.otherParts, 0);

    // AND THE ITEM NUMBERS RUN 1, 2 -- the cut list's own, not the parts
    // list's, because the two are read side by side on a bench.
    EXPECT_EQ(cuts.rows[0].item, 1);
    EXPECT_EQ(cuts.rows[1].item, 2);

    // ADD A MEMBER AND BOTH MOVE, because there is nothing to keep in step.
    assembly.addInstance("Brace", layout.members[0].sourcePath(), "");
    const CutListContents again = CutListOf(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(again.ok) << again.why;
    EXPECT_EQ(again.rows[0].quantity, 3);
    EXPECT_EQ(CountAssembly(assembly, BomDepth::TopLevel).totalQuantity(), 5);
}

TEST(FrameGeneratorTest, M56_CUT_002_WhatIsNotSteelIsCountedRatherThanDroppedQuietly) {
    // A frame is bolted together. Those bolts belong on the parts list and not
    // on the saw's, and a cut list that silently ignored them would leave "the
    // cut list has 4 lines, the assembly has 12 parts" for somebody to explain.
    AssemblyDocument assembly{"Trolley"};
    // THE FASTENERS GO IN FIRST, and that is the point of the ordering.
    //
    // Found by the mutation gate: with the steel added first, the cut list's
    // own item numbers and the parts list's were the same numbers, and reading
    // one off the other passed. Here the parts list runs screw, washer, rail,
    // stile -- so a cut list that carried its neighbours' numbers would start
    // at 3, and a fabricator would be checking every line against another
    // sheet to use it at all.
    for (int i = 1; i <= 8; ++i)
        assembly.addInstance("Screw" + std::to_string(i), "std:ISO 4762 M8x30", "");
    assembly.addInstance("Washer1", "std:ISO 7089 M8", "");
    const FrameLayout layout = GenerateFrame(Rectangle(), Section("SHS 40x40x3"));
    ASSERT_TRUE(layout.ok) << layout.why;
    PlaceFrame(assembly, layout);

    const BomContents parts = CountAssembly(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(parts.ok) << parts.why;
    ASSERT_EQ(parts.rows.size(), 4u);
    EXPECT_EQ(parts.rows[2].item, 3) << "the steel is the third and fourth line of the PARTS list";

    const CutListContents cuts = CutListOf(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(cuts.ok) << cuts.why;
    EXPECT_EQ(cuts.rows.size(), 2u) << "a fastener reached the cut list";
    EXPECT_EQ(cuts.otherParts, 9);
    EXPECT_EQ(cuts.rows[0].item, 1) << "the cut list is numbered from the parts list";
    EXPECT_EQ(cuts.rows[1].item, 2);

    // The steel weighs what the catalogue says it does: 2 m of 3.30 kg/m.
    EXPECT_NEAR(cuts.totalMassKg(), 2.0 * 3.30, 1e-6);
}

TEST(FrameGeneratorTest, M56_CUT_003_WhatToBuyIsAxisLengthAndNotLongPoint) {
    // A MITRE CONSUMES ITS AXIS LENGTH OF STOCK, not its long point: the wedge
    // sticking out past one joint is the wedge missing from the other. Ordering
    // by long point buys 40 mm too much per member -- which is the same
    // arithmetic that makes a mitre weigh nothing extra.
    AssemblyDocument assembly{"Trolley"};
    const FrameLayout layout = GenerateFrame(Rectangle(), Section("SHS 40x40x3"));
    ASSERT_TRUE(layout.ok) << layout.why;
    PlaceFrame(assembly, layout);

    const CutListContents cuts = CutListOf(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(cuts.ok) << cuts.why;
    const std::vector<CutListContents::StockLine> stock = cuts.stock();
    ASSERT_EQ(stock.size(), 1u) << "one section was ordered as two lines";
    EXPECT_EQ(stock.front().profile.designation(), "SHS 40x40x3");
    EXPECT_NEAR(stock.front().totalLengthMm, 2.0 * 600.0 + 2.0 * 400.0, 1e-9);
    EXPECT_NEAR(stock.front().massKg, 2.0 * 3.30, 1e-6);
}

TEST(FrameGeneratorTest, M56_CUT_004_AMitreThatEatsTheMemberIsRefusedBeforeItIsBuilt) {
    // A very short member between two shallow joints has cuts that meet before
    // it ends. The arithmetic still yields a number and the solid still builds
    // as SOMETHING -- a wedge with a path, a mass and a row on the cut list.
    FrameMemberSpec spec;
    spec.profile = Section("SHS 100x100x6");
    spec.lengthMm = 60.0;
    spec.angleADeg = 30.0;
    spec.angleBDeg = 30.0;
    // 50 * cot(30) is 86.6 at each end: 173 of overhang on 60 of member.
    EXPECT_LT(spec.shortPointMm(), 0.0);
    const std::string why = WhyMemberRefused(spec);
    EXPECT_FALSE(why.empty());
    EXPECT_NE(why.find("no member left"), std::string::npos) << why;

    // ...and the path it would have had is not resolvable, so nothing can
    // place one by writing the string out by hand either.
    EXPECT_FALSE(FrameMemberOfPath(FrameMemberPath(spec)).has_value());

    // THE GENERATOR REFUSES THE WHOLE LAYOUT rather than shipping three good
    // members and one wedge. A frame with a member missing is obvious; a frame
    // with a wedge in it is not.
    std::vector<SkeletonLine> tiny = Rectangle(60.0, 60.0);
    const FrameLayout layout = GenerateFrame(tiny, Section("SHS 100x100x6"));
    EXPECT_FALSE(layout.ok);
    EXPECT_NE(layout.why.find("cannot be made"), std::string::npos) << layout.why;
}

TEST(FrameGeneratorTest, M56_CUT_005_APathRoundTripsThroughItsOwnText) {
    // The path is the identity, so it has to survive being written down and
    // read back -- by the loader, by a parts list, and by a user typing one.
    for (const FrameProfile& profile : StandardSections()) {
        FrameMemberSpec spec;
        spec.profile = profile;
        spec.lengthMm = 1234.5;
        spec.angleADeg = 37.5;
        spec.angleBDeg = 142.5;
        const std::optional<FrameMemberSpec> back = FrameMemberOfPath(FrameMemberPath(spec));
        ASSERT_TRUE(back.has_value()) << FrameMemberPath(spec);
        EXPECT_EQ(back->profile.designation(), profile.designation());
        EXPECT_NEAR(back->lengthMm, 1234.5, 1e-9);
        EXPECT_NEAR(back->angleADeg, 37.5, 1e-9);
        EXPECT_NEAR(back->angleBDeg, 142.5, 1e-9);
    }

    // A SQUARE-CUT MEMBER HAS THE SHORT PATH, and it reads back as square --
    // so a member cut square is one row on the list however it was made.
    FrameMemberSpec plain;
    plain.profile = Section("SHS 40x40x3");
    plain.lengthMm = 250.0;
    EXPECT_EQ(FrameMemberPath(plain), "frm:SHS 40x40x3 L=250");
    const std::optional<FrameMemberSpec> back = FrameMemberOfPath("frm:SHS 40x40x3 L=250");
    ASSERT_TRUE(back.has_value());
    EXPECT_NEAR(back->angleADeg, 90.0, 1e-9);
    EXPECT_NEAR(back->angleBDeg, 90.0, 1e-9);

    // Rubbish is refused rather than half-read.
    EXPECT_FALSE(FrameMemberOfPath("frm:SHS 40x40x3").has_value());
    EXPECT_FALSE(FrameMemberOfPath("frm:SHS 40x40x3 L=250 A=45").has_value());
    EXPECT_FALSE(FrameMemberOfPath("frm:SHS 41x41x3 L=250").has_value());
    EXPECT_FALSE(FrameMemberOfPath("D:/frames/rail.ep3d").has_value());
}

} // namespace
