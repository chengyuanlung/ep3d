// M56.2 -- a length of steel, built from its designation.
//
// THE FIRST TEST IN THIS FILE IS THE POINT OF THE MILESTONE'S TABLE.
//
// Every number table this program has copied down -- M37's fits, M39's threads,
// M41's roughness, M51's k-factors -- had to be proof-read and then trusted,
// because nothing about a thread pitch can be measured against anything else.
// A steel section is different: it publishes a MASS PER METRE, and a mass per
// metre is a fact about the shape. So the catalogue is not proof-read here, it
// is WEIGHED: build a metre of every row, measure it at 7850 kg/m3, and compare.
//
// A mistyped digit, a corner radius left off, a wall thickness on the wrong
// side of the outline -- each of them moves the mass, and each of them would
// otherwise reach a quotation.

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Document/PartDocument.h"
#include "Core/Drawing/BomTable.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Frame/FrameLayout.h"
#include "Core/Frame/FrameProfile.h"
#include "Core/Measure/ModelMeasure.h"
#include "Kernel/Occt/OcctGeometryKernel.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kSteelKgPerM3 = 7850.0;

// The tip solid of a built member, or an invalid shape with the reason.
struct Built {
    KernelShape shape;
    std::string why;
    std::unique_ptr<PartDocument> part;
};

Built Build(OcctGeometryKernel& kernel, const FrameMemberSpec& spec) {
    Built out;
    out.part = BuildFrameMember(spec);
    if (!out.part) {
        out.why = "refused: " + WhyMemberRefused(spec);
        return out;
    }
    out.part->setGeometryKernel(&kernel);
    const DocumentRecomputeReport report = out.part->recompute();
    if (!report.success) {
        for (const RecomputeItemReport& item : report.items)
            if (!item.message.empty())
                out.why += (out.why.empty() ? "" : "; ") + out.part->objectName(item.id) + ": " +
                           item.message;
        if (out.why.empty()) out.why = "did not build";
        return out;
    }
    const ISolidFeature* tip = nullptr;
    for (const auto& feature : out.part->bodies().front()->features())
        if (const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get())) tip = solid;
    if (tip == nullptr) {
        out.why = "no solid feature";
        return out;
    }
    out.shape = tip->currentShape();
    return out;
}

FrameMemberSpec Member(std::string_view section, double lengthMm, double a = 90.0,
                       double b = 90.0) {
    FrameMemberSpec spec;
    const std::optional<FrameProfile> profile = LookUpSection(section);
    EXPECT_TRUE(profile.has_value()) << section;
    if (profile) spec.profile = *profile;
    spec.lengthMm = lengthMm;
    spec.angleADeg = a;
    spec.angleBDeg = b;
    return spec;
}

double MassKgOf(const MeasureResult& measured) {
    for (const MeasureItem& item : measured.items)
        if (item.label == "Mass") return item.value;
    return -1.0;
}

TEST(OcctFrameMemberTest, M56_KRN_001_EVERYRowInTheCatalogueWeighsWhatItSaysItDoes) {
    OcctGeometryKernel kernel;

    // A METRE OF EACH, so the number that comes out is the published one with
    // no arithmetic in between to hide a mistake.
    for (const FrameProfile& profile : StandardSections()) {
        FrameMemberSpec spec;
        spec.profile = profile;
        spec.lengthMm = 1000.0;

        const Built built = Build(kernel, spec);
        ASSERT_TRUE(built.shape.isValid())
            << profile.designation() << " did not build: " << built.why;

        const MeasureResult measured = MeasureSolid(kernel, built.shape, kSteelKgPerM3);
        ASSERT_TRUE(measured.ok) << profile.designation() << ": " << measured.message;
        const double weighed = MassKgOf(measured);
        ASSERT_GT(weighed, 0.0) << profile.designation();

        // HALF A PERCENT. The published figures are rounded to three
        // significant figures, so exact equality is not available -- and half a
        // percent is far tighter than any error the failures this guards
        // against would produce: a missing corner radius is six percent, a
        // wall on the wrong side of the outline is more.
        EXPECT_NEAR(weighed, profile.massPerMetreKgPerM,
                    profile.massPerMetreKgPerM * 0.005)
            << profile.designation() << " is modelled at " << weighed
            << " kg/m and published at " << profile.massPerMetreKgPerM
            << " kg/m -- one of the two is wrong, and the table is the one a "
               "purchase order is written from";
    }
}

double ItemOf(const MeasureResult& measured, const std::string& label) {
    for (const MeasureItem& item : measured.items)
        if (item.label == label) return item.value;
    ADD_FAILURE() << "no item called " << label;
    return 0.0;
}

TEST(OcctFrameMemberTest, M56_KRN_002_AMitreCostsNoSTEELAndTheMassFormulaSaysSo) {
    OcctGeometryKernel kernel;

    // A picture-frame corner: 45 at both ends.
    const FrameMemberSpec mitred = Member("SHS 40x40x3", 300.0, 45.0, 45.0);
    const Built built = Build(kernel, mitred);
    ASSERT_TRUE(built.shape.isValid()) << built.why;

    const KernelBoundsResult bounds = kernel.boundsOfShape(built.shape);
    ASSERT_TRUE(bounds.ok);
    // THE LONG POINT IS WHAT THE SOLID MEASURES: 300 of axis with two 45s on a
    // 40 wide tube reaches 20 past the joint at each end.
    EXPECT_NEAR(mitred.longPointMm(), 340.0, 1e-9);
    EXPECT_NEAR(mitred.shortPointMm(), 260.0, 1e-9);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, mitred.longPointMm(), 0.01)
        << "the cut planes are not where the mitre arithmetic says they are";

    const double weighed = ItemOf(MeasureSolid(kernel, built.shape, kSteelKgPerM3), "Mass");
    const Built square = Build(kernel, Member("SHS 40x40x3", 300.0));
    ASSERT_TRUE(square.shape.isValid()) << square.why;
    const double squareMass =
        ItemOf(MeasureSolid(kernel, square.shape, kSteelKgPerM3), "Mass");

    // AND HERE IS THE FACT THE FIRST DRAFT OF THIS TEST GOT WRONG, by assuming
    // a mitre must weigh less than a square cut. IT WEIGHS EXACTLY THE SAME.
    //
    // The volume between the two cut planes is the integral of their
    // separation over the section, and that separation is L - x*(cotA + cotB).
    // The section is symmetric about its own centre, so the integral of x over
    // it is zero and the whole cut term vanishes: every mitre of a symmetric
    // section takes off exactly as much wedge as it adds.
    //
    // Which is WHY FrameMemberSpec::massKg is the axis length times the
    // published kilogram per metre with no mention of the cuts in it. That was
    // written as an approximation and is in fact exact -- and it is this test,
    // failing, that established it.
    EXPECT_NEAR(weighed, squareMass, squareMass * 1e-6)
        << "a mitre changed the mass, so massKg() cannot ignore the end cuts";
    EXPECT_NEAR(weighed, mitred.massKg(), mitred.massKg() * 0.005)
        << "the solid and the number the cut list prints disagree";

    // ...and it is still a TUBE. A solid 40x40 bar 300 long is 3.77 kg, which
    // is what a bore that missed the mitre would have left behind.
    EXPECT_NEAR(weighed, 0.3 * 3.30, 0.3 * 3.30 * 0.005);
    EXPECT_LT(weighed, 2.0) << "the bore did not go through";
}

TEST(OcctFrameMemberTest, M56_KRN_003_TheTwoCutsLeanIndependently) {
    OcctGeometryKernel kernel;

    // 45 AND 135 IS A PARALLELOGRAM: the two cut planes are parallel, so both
    // long edges are exactly the axis length and neither end sticks out past
    // the other. Reporting this member at 340 -- which taking magnitudes
    // instead of the signed sum would -- is a cut 40 mm too long, twice.
    const FrameMemberSpec skew = Member("SHS 40x40x3", 300.0, 45.0, 135.0);
    EXPECT_NEAR(skew.longPointMm(), 300.0, 1e-9);
    EXPECT_NEAR(skew.shortPointMm(), 300.0, 1e-9);

    const Built built = Build(kernel, skew);
    ASSERT_TRUE(built.shape.isValid()) << built.why;
    const KernelBoundsResult bounds = kernel.boundsOfShape(built.shape);
    ASSERT_TRUE(bounds.ok);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, 340.0, 0.01)
        << "the BOX round a parallelogram still spans one overhang past each end, "
           "even though no single edge is longer than the axis";

    // AND IT IS A DIFFERENT SOLID from the picture-frame one, which is the
    // whole reason the two leans are separate numbers in the path.
    //
    // NOT BY MASS -- KRN_002 established that every mitre of a symmetric
    // section weighs the same. What tells them apart is WHERE the steel sits:
    // a picture-frame member is longer on one side, so its centre of mass
    // leans that way; a parallelogram is the same length everywhere and its
    // centre of mass stays on the axis.
    const Built frame = Build(kernel, Member("SHS 40x40x3", 300.0, 45.0, 45.0));
    ASSERT_TRUE(frame.shape.isValid()) << frame.why;
    const double skewX = ItemOf(MeasureSolid(kernel, built.shape, 0.0), "Centre of mass X");
    const double frameX = ItemOf(MeasureSolid(kernel, frame.shape, 0.0), "Centre of mass X");
    EXPECT_NEAR(skewX, 0.0, 1e-6) << "a parallelogram's steel is off to one side";
    // -2I/(L*A) for this section: about 1.6 mm towards the long point.
    EXPECT_LT(frameX, -1.0) << "the mitred member's steel did not lean towards its long point";
    EXPECT_GT(frameX, -2.5);
}

TEST(OcctFrameMemberTest, M56_KRN_004_ARoundTubeAndAnAngleAreBuiltFromTheirOwnOutlines) {
    OcctGeometryKernel kernel;

    // An angle is the one section here that is NOT symmetric about its own
    // axis, so its centre of mass says whether the outline came out as an L or
    // as something with the legs in the wrong place.
    const Built angle = Build(kernel, Member("L 40x40x4", 1000.0));
    ASSERT_TRUE(angle.shape.isValid()) << angle.why;
    const MeasureResult measured = MeasureSolid(kernel, angle.shape, kSteelKgPerM3);
    ASSERT_TRUE(measured.ok) << measured.message;
    double centreX = 0.0;
    double centreY = 0.0;
    for (const MeasureItem& item : measured.items) {
        if (item.label == "Centre of mass X") centreX = item.value;
        if (item.label == "Centre of mass Y") centreY = item.value;
    }
    // EN 10056 publishes the centroid of an L 40x40x4 at 11.2 mm from the back
    // of each leg, and the outline is drawn with its heel at the origin.
    EXPECT_NEAR(centreX, 11.2, 0.15) << "the angle's legs are not where the standard puts them";
    EXPECT_NEAR(centreY, 11.2, 0.15);
    EXPECT_NEAR(centreX, centreY, 1e-6) << "an EQUAL angle is symmetric about its own diagonal";

    // A round tube's bore has to be round: a bore built as a square would
    // weigh visibly less and nothing else would notice.
    const Built tube = Build(kernel, Member("CHS 42.4x2.6", 1000.0));
    ASSERT_TRUE(tube.shape.isValid()) << tube.why;
    const KernelBoundsResult bounds = kernel.boundsOfShape(tube.shape);
    ASSERT_TRUE(bounds.ok);
    EXPECT_NEAR(bounds.max.x - bounds.min.x, 42.4, 0.01);
    EXPECT_NEAR(bounds.max.y - bounds.min.y, 42.4, 0.01);
}

TEST(OcctFrameMemberTest, M56_KRN_005_AMemberIsPlacedAndCountedLikeAnyOtherPart) {
    // THE POINT OF THE `frm:` SCHEME. Nothing below mentions a frame: an
    // instance names a path, an assembly builds it, a parts list counts it --
    // the same three sentences a file and a catalogue screw answer, through
    // the same code.
    OcctGeometryKernel kernel;
    AssemblyDocument assembly{"Frame"};
    assembly.setGeometryKernel(&kernel);

    const std::string rail = "frm:SHS 40x40x3 L=600 A=45 B=45";
    const std::string post = "frm:SHS 40x40x3 L=400 A=45 B=45";
    assembly.addInstance("Rail1", rail, "");
    assembly.addInstance("Rail2", rail, "");
    Instance& first = assembly.addInstance("Post1", post, "");
    assembly.addInstance("Post2", post, "");
    ASSERT_TRUE(assembly.recompute().success);
    EXPECT_EQ(first.currentState(), ComputeState::Valid);
    ASSERT_TRUE(first.currentShape().isValid());

    const KernelBoundsResult bounds = kernel.boundsOfShape(first.currentShape());
    ASSERT_TRUE(bounds.ok);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, 440.0, 0.01);

    // TWO ROWS, NOT FOUR. Identical members are one line with a quantity, and
    // rails and posts are different lines -- because the path they are grouped
    // on already carries the length and both cuts.
    const BomContents counted = CountAssembly(assembly, BomDepth::TopLevel);
    ASSERT_TRUE(counted.ok) << counted.why;
    ASSERT_EQ(counted.rows.size(), 2u);
    EXPECT_EQ(counted.totalQuantity(), 4);

    // AND THE EXPLODED LIST COUNTS THEM TOO -- which it could not do for a
    // library part before M56, because it opened every source path as a file.
    const BomContents deep = CountAssembly(assembly, BomDepth::Exploded);
    ASSERT_TRUE(deep.ok) << deep.why;
    EXPECT_EQ(deep.totalQuantity(), 4);

    // A SIZE NOBODY STOCKS FAILS LOUDLY, rather than arriving as an empty
    // instance that looks like a member somebody forgot to place.
    Instance& ghost = assembly.addInstance("Ghost", "frm:SHS 45x45x3 L=250", "");
    assembly.recompute();
    EXPECT_EQ(ghost.currentState(), ComputeState::Failed);
}

TEST(OcctFrameMemberTest, M56_KRN_006_AGeneratedFrameACTUALLYCLOSES) {
    // THE ONE TEST THAT COULD NOT BE FAKED.
    //
    // Everything else about a mitre can be checked in arithmetic, and the
    // arithmetic was wrong once already: the first draft's cut planes leaned
    // the same way at both ends, which makes 45/45 a parallelogram and turns
    // every generated frame into four sticks that will not weld up. The
    // numbers all looked right -- 45 at each end, 340 long point, the mass of
    // 600 mm of tube.
    //
    // Two mitred members at a corner meet on a face. They must NOT share
    // volume, which is what a mitre leaning the wrong way produces, and they
    // must not be apart either. M55's interference measurement answers the
    // first directly; the second is what the fused solid's mass says, since
    // two solids that touch fuse into one of exactly their combined weight and
    // two that overlap fuse into something lighter.
    OcctGeometryKernel kernel;
    const FrameLayout layout = GenerateFrame(
        {SkeletonLine{Vec3{0.0, 0.0, 0.0}, Vec3{600.0, 0.0, 0.0}},
         SkeletonLine{Vec3{600.0, 0.0, 0.0}, Vec3{600.0, 400.0, 0.0}},
         SkeletonLine{Vec3{600.0, 400.0, 0.0}, Vec3{0.0, 400.0, 0.0}},
         SkeletonLine{Vec3{0.0, 400.0, 0.0}, Vec3{0.0, 0.0, 0.0}}},
        LookUpSection("SHS 40x40x3").value());
    ASSERT_TRUE(layout.ok) << layout.why;
    ASSERT_EQ(layout.members.size(), 4u);

    std::vector<KernelShape> placed;
    for (const FrameMemberPlacement& member : layout.members) {
        const Built built = Build(kernel, member.spec);
        ASSERT_TRUE(built.shape.isValid()) << member.name << ": " << built.why;
        const ShapeResult put = kernel.placeShape(built.shape, member.placement);
        ASSERT_EQ(put.error, KernelError::None) << put.message;
        placed.push_back(put.shape);
    }

    // NO TWO MEMBERS SHARE ANY STEEL. A mitre with the wrong hand overlaps its
    // neighbour by a wedge of roughly a section's worth -- thousands of cubic
    // millimetres -- so the tolerance here is generous and still decisive.
    for (std::size_t i = 0; i < placed.size(); ++i) {
        for (std::size_t j = i + 1; j < placed.size(); ++j) {
            const MeasureResult between = MeasureBetweenSolids(kernel, placed[i], placed[j]);
            ASSERT_TRUE(between.ok) << between.message;
            EXPECT_LT(ItemOf(between, "Overlap volume"), 1.0)
                << layout.members[i].name << " and " << layout.members[j].name
                << " are driven into each other, so the mitres lean the wrong way";
        }
    }

    // AND THE FRAME IS ONE PIECE, weighing what two metres of this tube
    // weighs. Four members that did not touch would fuse into a compound of
    // the same mass, so the mass alone does not prove contact -- but combined
    // with the corners being mitred at all, a frame this heavy with no overlap
    // anywhere is a frame that closes.
    KernelShape together = placed.front();
    for (std::size_t i = 1; i < placed.size(); ++i) {
        const ShapeResult fused = kernel.fuseShapes(together, placed[i]);
        ASSERT_EQ(fused.error, KernelError::None) << fused.message;
        together = fused.shape;
    }
    const MeasureResult whole = MeasureSolid(kernel, together, kSteelKgPerM3);
    ASSERT_TRUE(whole.ok) << whole.message;
    EXPECT_NEAR(ItemOf(whole, "Mass"), 2.0 * 3.30, 2.0 * 3.30 * 0.005)
        << "the welded frame does not weigh what its cut list says it does";

    // The outside of the frame is the skeleton plus half a section all round,
    // which is where a mitred corner's long point puts it.
    const KernelBoundsResult bounds = kernel.boundsOfShape(together);
    ASSERT_TRUE(bounds.ok);
    EXPECT_NEAR(bounds.max.x - bounds.min.x, 640.0, 0.01);
    EXPECT_NEAR(bounds.max.y - bounds.min.y, 440.0, 0.01);
    EXPECT_NEAR(bounds.max.z - bounds.min.z, 40.0, 0.01);
}

} // namespace
