#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

class PartDocument;

// M56 -- THE FRAME GENERATOR'S VOCABULARY.
//
// A welded frame is the most common thing a small shop makes and the thing
// this program was worst at: a length of 40x40x3 tube was a box feature with
// three numbers typed into it, and its wall thickness was a pocket somebody
// remembered to add.
//
// THE WHOLE MILESTONE RIDES ON ONE DECISION: a frame member reaches the rest
// of the program as a SOURCE PATH, exactly as a catalogue fastener does (M45).
//
//   frm:SHS 40x40x3 L=250 A=45 B=45
//
// An instance names it, a drawing view projects it, a parts list counts it, a
// mass calculation weighs it -- and not one of those had to learn what a frame
// member is. The path is also the member's COMPLETE IDENTITY: profile, length,
// and both end cuts. Two members with the same path are the same stick of
// steel, and two with different cuts are not.
//
// WHICH IS WHY THE CUT LIST IS NOT A NEW TABLE. It is the parts list, counted
// by the same function, grouped on a path that already says how the saw is
// set. A frame generator that produced its own cut list would be the defect
// this codebase keeps paying for: two things that must agree, filled in by
// hand, each tested alone -- and the number that would disagree is a length of
// steel somebody cuts.

// --- The sections -----------------------------------------------------------
//
// FOUR KINDS, and every one of them has an outline of CONSTANT WALL THICKNESS.
// That is not a coincidence and it is not laziness: a tapered-flange channel
// or an I-beam has a published mass that includes root radii and a flange
// taper, and an outline that skipped them would model a section a few percent
// off the one the invoice is for. Those sections are absent rather than
// approximated, for M45's reason -- a size that is nearly right is worse than
// a size that is missing, because the nearly-right one gets used.
enum class SectionKind {
    SquareHollow,      // EN 10219, cold formed -- SHS
    RectangularHollow, // EN 10219 -- RHS
    CircularHollow,    // EN 10219 -- CHS
    EqualAngle,        // EN 10056-1 -- L
};
std::string_view toString(SectionKind kind) noexcept;
bool ParseSectionKind(std::string_view text, SectionKind& into) noexcept;
// The prefix a designation carries: "SHS", "RHS", "CHS", "L".
std::string_view SectionPrefixOf(SectionKind kind) noexcept;
// The standard this kind's numbers come from, which is what a drawing note and
// a purchase order say.
std::string_view StandardNumberOf(SectionKind kind) noexcept;

// ONE PUBLISHED SECTION.
//
// A zero in a field the kind does not use -- a round tube has no corner radius
// -- and nothing ever reads a field its kind does not fill.
struct FrameProfile {
    SectionKind kind = SectionKind::SquareHollow;
    double heightMm = 40.0;    // h; the outside DIAMETER for CHS; the leg for L
    double widthMm = 40.0;     // b; equal to h for SHS, CHS and L
    double thicknessMm = 3.0;  // t
    double outerRadiusMm = 0.0; // SHS/RHS corner outside; L toe radius r2
    double innerRadiusMm = 0.0; // SHS/RHS corner inside;  L root radius r1

    // THE PUBLISHED MASS, copied down and never computed.
    //
    // It is what the steel is invoiced by, and the modelled outline is checked
    // AGAINST it rather than the other way round: OcctFrameMemberTests builds
    // every row in the table and weighs it, so a digit mistyped here fails a
    // test instead of quietly costing a few percent on every frame that uses
    // that size. No earlier number table in this program could be checked that
    // way -- a thread pitch and a hole tolerance have nothing to be measured
    // against -- and this one can, so it is.
    double massPerMetreKgPerM = 0.0;

    // "SHS 40x40x3", "CHS 42.4x2.6", "L 40x40x4". What a drawing prints and
    // what the path is built from -- a RESOLVER PATH, not a label, so the
    // numbers go through ShortNumber and nothing is rounded for looks.
    std::string designation() const;
};

// EVERY SECTION THIS PROGRAM HOLDS, in the order a catalogue lists them.
const std::vector<FrameProfile>& StandardSections();

// The section a designation names, or nothing. A size the catalogue does not
// hold is refused rather than interpolated: an SHS 45x45x3 is not a number, it
// is a length of steel that has to exist.
std::optional<FrameProfile> LookUpSection(std::string_view designation);

// --- A member ---------------------------------------------------------------

// AN ANGLE IS MEASURED FROM THE MEMBER'S AXIS, in degrees, and 90 is a square
// cut. This is how a saw is set and how a shop drawing calls it out.
//
// The full range 0 < angle < 180 is meaningful and both halves are used: 45 and
// 135 are the same saw setting LEANING OPPOSITE WAYS, which is the difference
// between a picture-frame corner and a parallelogram brace. One signed number
// carries it; a magnitude plus a hand would be two fields that have to agree.
constexpr double kSquareCutDeg = 90.0;

struct FrameMemberSpec {
    FrameProfile profile;
    // THE AXIS LENGTH: the distance between the two joint points on the
    // skeleton. Not the long point and not the short point -- those are
    // DERIVED below, and storing either would be storing an answer that the
    // end angles already determine.
    double lengthMm = 0.0;
    double angleADeg = kSquareCutDeg; // the cut at the start of the axis
    double angleBDeg = kSquareCutDeg; // the cut at the end

    // "SHS 40x40x3 L=250" or "SHS 40x40x3 L=250 A=45 B=45". The angles are
    // omitted when both are square, so an unmitred member's path is the short
    // one a user would have typed.
    std::string designation() const;

    // WHAT THE SAW IS SET TO, and what the tape reads.
    //
    // The classic fabrication ambiguity -- "is that the long point?" -- has
    // exactly one answer here because neither number is stored: both come out
    // of the axis length and the two angles, and they cannot drift apart.
    //
    // Each end's cut moves the material by (b/2)*cot(angle) at the extreme
    // corner, so the two corner lengths are L +/- (overA + overB) -- and it is
    // the SUM, not the sum of magnitudes, which is why a 45/135 member is a
    // parallelogram whose long point and short point are both L.
    double longPointMm() const noexcept;
    double shortPointMm() const noexcept;
    // How much longer the pad has to be than the axis, at each end, so that
    // both cuts have material to take away.
    double overhangAMm() const noexcept;
    double overhangBMm() const noexcept;

    double massKg() const noexcept;
};

// WHY THIS MEMBER CANNOT BE MADE, or empty when it can.
//
// A cut that leaves no short point is the one that matters: the arithmetic
// still produces a number, the solid still builds as something, and what comes
// out is a wedge where a member was meant to be.
std::string WhyMemberRefused(const FrameMemberSpec& spec);

// --- The source-path scheme -------------------------------------------------
//
//   frm:SHS 40x40x3 L=250 A=45 B=45
//
// The prefix tells the resolver this is generated and not a file to open,
// exactly as `std:` does. See LibraryPart.h, which is the one place either
// scheme is turned into a part.
constexpr std::string_view kFrameMemberScheme = "frm:";
bool IsFrameMemberPath(std::string_view path) noexcept;
std::string FrameMemberPath(const FrameMemberSpec& spec);
std::optional<FrameMemberSpec> FrameMemberOfPath(std::string_view path);

// THE MEMBER, BUILT FROM THE NUMBERS: a real PartDocument with a real feature
// tree, so the recompute engine, the projector, the parts list and the measure
// tool all treat it as the part it is.
//
// The section is padded to the long-point length and the two ends are then cut
// away, which is what a saw does and what makes the wall thickness follow the
// mitre round the corner -- a tube cut at 45 shows a rectangle of wall, and
// that rectangle is what a weld preparation is drawn on.
std::unique_ptr<PartDocument> BuildFrameMember(const FrameMemberSpec& spec);

} // namespace paramcad
