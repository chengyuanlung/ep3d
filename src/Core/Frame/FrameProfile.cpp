#include "Core/Frame/FrameProfile.h"

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Text/NumberText.h"

#include <cmath>
#include <cstddef>

namespace paramcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- THE CATALOGUE ----------------------------------------------------------
//
// Copied down from EN 10219-2 (hollow sections, cold formed) and EN 10056-1
// (equal angles). The corner radii follow those standards' own rule -- outside
// 2t and inside t for a hollow section under 6 mm wall -- and they are written
// out per row rather than derived, because a rule that holds for the sizes in
// the table is still a rule that will be wrong for the first size added
// outside it.
//
// EVERY ROW IS CHECKED AGAINST GEOMETRY. Build the member, weigh the solid at
// 7850 kg/m3, and it must come out at the published kilogram per metre -- see
// OcctFrameMemberTests. That check is why the radii are here at all: the
// square-cornered outline of an SHS 40x40x3 weighs 3.49 kg/m against a
// published 3.30, and six percent is exactly the size of error that survives
// a review and reaches a quotation.
constexpr FrameProfile kSections[] = {
    // kind, h, b, t, r_out, r_in, kg/m
    {SectionKind::SquareHollow, 25.0, 25.0, 2.0, 4.0, 2.0, 1.36},
    {SectionKind::SquareHollow, 30.0, 30.0, 2.0, 4.0, 2.0, 1.68},
    {SectionKind::SquareHollow, 40.0, 40.0, 2.0, 4.0, 2.0, 2.31},
    {SectionKind::SquareHollow, 40.0, 40.0, 3.0, 6.0, 3.0, 3.30},
    {SectionKind::SquareHollow, 50.0, 50.0, 3.0, 6.0, 3.0, 4.25},
    {SectionKind::SquareHollow, 50.0, 50.0, 4.0, 8.0, 4.0, 5.45},
    {SectionKind::SquareHollow, 60.0, 60.0, 3.0, 6.0, 3.0, 5.19},
    {SectionKind::SquareHollow, 60.0, 60.0, 4.0, 8.0, 4.0, 6.71},
    {SectionKind::SquareHollow, 80.0, 80.0, 4.0, 8.0, 4.0, 9.22},
    {SectionKind::SquareHollow, 80.0, 80.0, 5.0, 10.0, 5.0, 11.30},
    {SectionKind::SquareHollow, 100.0, 100.0, 5.0, 10.0, 5.0, 14.40},
    {SectionKind::SquareHollow, 100.0, 100.0, 6.0, 12.0, 6.0, 17.00},

    {SectionKind::RectangularHollow, 50.0, 30.0, 3.0, 6.0, 3.0, 3.30},
    {SectionKind::RectangularHollow, 60.0, 40.0, 3.0, 6.0, 3.0, 4.25},
    {SectionKind::RectangularHollow, 80.0, 40.0, 4.0, 8.0, 4.0, 6.71},
    {SectionKind::RectangularHollow, 100.0, 50.0, 4.0, 8.0, 4.0, 8.59},
    {SectionKind::RectangularHollow, 120.0, 60.0, 5.0, 10.0, 5.0, 12.80},

    // h IS THE OUTSIDE DIAMETER for a round tube, and b repeats it. A round
    // section has one across-size, and giving it two fields that must agree
    // would be one field too many -- so the builder reads h and the width is
    // there only so that "how wide is this member" has an answer for every
    // kind (the mitre arithmetic asks it).
    {SectionKind::CircularHollow, 26.9, 26.9, 2.3, 0.0, 0.0, 1.40},
    {SectionKind::CircularHollow, 33.7, 33.7, 2.6, 0.0, 0.0, 1.99},
    {SectionKind::CircularHollow, 42.4, 42.4, 2.6, 0.0, 0.0, 2.55},
    {SectionKind::CircularHollow, 48.3, 48.3, 3.2, 0.0, 0.0, 3.56},
    {SectionKind::CircularHollow, 60.3, 60.3, 3.2, 0.0, 0.0, 4.51},

    // For an angle, r_out is the TOE radius r2 and r_in is the ROOT radius r1,
    // and r2 = r1/2 is the standard's own relation.
    {SectionKind::EqualAngle, 30.0, 30.0, 3.0, 2.5, 5.0, 1.36},
    {SectionKind::EqualAngle, 40.0, 40.0, 4.0, 3.0, 6.0, 2.42},
    {SectionKind::EqualAngle, 50.0, 50.0, 5.0, 3.5, 7.0, 3.77},
    {SectionKind::EqualAngle, 60.0, 60.0, 6.0, 4.0, 8.0, 5.42},
    {SectionKind::EqualAngle, 80.0, 80.0, 8.0, 5.0, 10.0, 9.63},
};

bool Same(double a, double b) noexcept { return std::fabs(a - b) < 1e-9; }

// A number, a separator, a number... shared by both parsers below so that
// "40x40x3" and "L=250" cannot come to disagree about what a number looks like.
bool ReadNumber(std::string_view text, std::size_t& at, double& out) {
    const std::size_t start = at;
    bool seenPoint = false;
    while (at < text.size()) {
        const char c = text[at];
        if (c >= '0' && c <= '9') {
            ++at;
        } else if (c == '.' && !seenPoint) {
            seenPoint = true;
            ++at;
        } else {
            break;
        }
    }
    if (at == start) return false;
    out = std::stod(std::string(text.substr(start, at - start)));
    return true;
}

bool Expect(std::string_view text, std::size_t& at, char c) {
    if (at >= text.size() || text[at] != c) return false;
    ++at;
    return true;
}

void SkipSpaces(std::string_view text, std::size_t& at) {
    while (at < text.size() && text[at] == ' ') ++at;
}

// The rounded rectangle a hollow section's wall follows, as a closed loop of
// four lines and four arcs, centred on the sketch origin.
//
// ONE FUNCTION FOR THE OUTSIDE AND THE INSIDE. They differ by the wall
// thickness and by the radius, and writing them twice is how a tube ends up
// with an outside cut from EN 10219 and an inside somebody derived.
void AddRoundedRectangle(PartDocument& part, ObjectId sketchId, double width, double height,
                         double radius) {
    const double x = width / 2.0;
    const double y = height / 2.0;
    if (radius <= 1e-9) {
        const Vec2 corners[4] = {Vec2{x, y}, Vec2{-x, y}, Vec2{-x, -y}, Vec2{x, -y}};
        for (int i = 0; i < 4; ++i)
            part.addSketchEntity(sketchId, SketchLine{corners[i], corners[(i + 1) % 4]});
        return;
    }
    const double sx = x - radius;
    const double sy = y - radius;
    part.addSketchEntity(sketchId, SketchLine{Vec2{sx, y}, Vec2{-sx, y}});
    part.addSketchEntity(sketchId,
                         SketchArc{Vec2{-sx, sy}, radius, kPi / 2.0, kPi, true});
    part.addSketchEntity(sketchId, SketchLine{Vec2{-x, sy}, Vec2{-x, -sy}});
    part.addSketchEntity(sketchId,
                         SketchArc{Vec2{-sx, -sy}, radius, kPi, 1.5 * kPi, true});
    part.addSketchEntity(sketchId, SketchLine{Vec2{-sx, -y}, Vec2{sx, -y}});
    part.addSketchEntity(sketchId,
                         SketchArc{Vec2{sx, -sy}, radius, 1.5 * kPi, 2.0 * kPi, true});
    part.addSketchEntity(sketchId, SketchLine{Vec2{x, -sy}, Vec2{x, sy}});
    part.addSketchEntity(sketchId, SketchArc{Vec2{sx, sy}, radius, 0.0, kPi / 2.0, true});
}

// An equal angle's single closed loop: two legs of thickness t meeting at the
// origin, a root fillet r1 that ADDS material at the re-entrant corner and two
// toe radii r2 that take it away at the tips.
void AddAngleOutline(PartDocument& part, ObjectId sketchId, const FrameProfile& profile) {
    const double a = profile.heightMm;
    const double t = profile.thicknessMm;
    const double r1 = profile.innerRadiusMm; // root
    const double r2 = profile.outerRadiusMm; // toe

    part.addSketchEntity(sketchId, SketchLine{Vec2{0.0, 0.0}, Vec2{a, 0.0}});
    part.addSketchEntity(sketchId, SketchLine{Vec2{a, 0.0}, Vec2{a, t - r2}});
    part.addSketchEntity(sketchId,
                         SketchArc{Vec2{a - r2, t - r2}, r2, 0.0, kPi / 2.0, true});
    part.addSketchEntity(sketchId, SketchLine{Vec2{a - r2, t}, Vec2{t + r1, t}});
    // CLOCKWISE, because this arc bulges TOWARDS the corner: the fillet in the
    // armpit of an angle is material the section has, not material it lacks.
    part.addSketchEntity(sketchId,
                         SketchArc{Vec2{t + r1, t + r1}, r1, 1.5 * kPi, kPi, false});
    part.addSketchEntity(sketchId, SketchLine{Vec2{t, t + r1}, Vec2{t, a - r2}});
    part.addSketchEntity(sketchId,
                         SketchArc{Vec2{t - r2, a - r2}, r2, 0.0, kPi / 2.0, true});
    part.addSketchEntity(sketchId, SketchLine{Vec2{t - r2, a}, Vec2{0.0, a}});
    part.addSketchEntity(sketchId, SketchLine{Vec2{0.0, a}, Vec2{0.0, 0.0}});
}

double CotDegrees(double angleDeg) noexcept {
    const double radians = angleDeg * kPi / 180.0;
    const double sine = std::sin(radians);
    if (std::fabs(sine) < 1e-12) return 0.0;
    return std::cos(radians) / sine;
}

} // namespace

std::string_view toString(SectionKind kind) noexcept {
    switch (kind) {
    case SectionKind::SquareHollow: return "SquareHollow";
    case SectionKind::RectangularHollow: return "RectangularHollow";
    case SectionKind::CircularHollow: return "CircularHollow";
    case SectionKind::EqualAngle: return "EqualAngle";
    }
    return "SquareHollow";
}

bool ParseSectionKind(std::string_view text, SectionKind& into) noexcept {
    for (const SectionKind kind : {SectionKind::SquareHollow, SectionKind::RectangularHollow,
                                   SectionKind::CircularHollow, SectionKind::EqualAngle}) {
        if (toString(kind) != text) continue;
        into = kind;
        return true;
    }
    return false;
}

std::string_view SectionPrefixOf(SectionKind kind) noexcept {
    switch (kind) {
    case SectionKind::SquareHollow: return "SHS";
    case SectionKind::RectangularHollow: return "RHS";
    case SectionKind::CircularHollow: return "CHS";
    case SectionKind::EqualAngle: return "L";
    }
    return "SHS";
}

std::string_view StandardNumberOf(SectionKind kind) noexcept {
    switch (kind) {
    case SectionKind::SquareHollow:
    case SectionKind::RectangularHollow:
    case SectionKind::CircularHollow: return "EN 10219";
    case SectionKind::EqualAngle: return "EN 10056-1";
    }
    return "EN 10219";
}

std::string FrameProfile::designation() const {
    std::string out(SectionPrefixOf(kind));
    out += ' ';
    // A ROUND TUBE IS NAMED BY DIAMETER AND WALL, and a two-number name is not
    // a shorter way of writing a three-number one -- "CHS 42.4x42.4x2.6" names
    // nothing anybody sells.
    if (kind == SectionKind::CircularHollow)
        return out + ShortNumber(heightMm) + "x" + ShortNumber(thicknessMm);
    return out + ShortNumber(heightMm) + "x" + ShortNumber(widthMm) + "x" +
           ShortNumber(thicknessMm);
}

const std::vector<FrameProfile>& StandardSections() {
    static const std::vector<FrameProfile> all(std::begin(kSections), std::end(kSections));
    return all;
}

std::optional<FrameProfile> LookUpSection(std::string_view designation) {
    std::size_t at = 0;
    SkipSpaces(designation, at);
    const std::size_t prefixStart = at;
    while (at < designation.size() && designation[at] != ' ') ++at;
    const std::string_view prefix = designation.substr(prefixStart, at - prefixStart);
    SkipSpaces(designation, at);

    double first = 0.0;
    double second = 0.0;
    double third = 0.0;
    if (!ReadNumber(designation, at, first)) return std::nullopt;
    if (!Expect(designation, at, 'x')) return std::nullopt;
    if (!ReadNumber(designation, at, second)) return std::nullopt;
    const bool hasThird = Expect(designation, at, 'x');
    if (hasThird && !ReadNumber(designation, at, third)) return std::nullopt;
    SkipSpaces(designation, at);
    if (at != designation.size()) return std::nullopt;

    for (const FrameProfile& row : kSections) {
        if (SectionPrefixOf(row.kind) != prefix) continue;
        if (row.kind == SectionKind::CircularHollow) {
            if (hasThird) continue;
            if (Same(row.heightMm, first) && Same(row.thicknessMm, second)) return row;
            continue;
        }
        if (!hasThird) continue;
        if (Same(row.heightMm, first) && Same(row.widthMm, second) &&
            Same(row.thicknessMm, third))
            return row;
    }
    return std::nullopt;
}

// --- The member -------------------------------------------------------------

double FrameMemberSpec::overhangAMm() const noexcept {
    return profile.widthMm / 2.0 * CotDegrees(angleADeg);
}

double FrameMemberSpec::overhangBMm() const noexcept {
    return profile.widthMm / 2.0 * CotDegrees(angleBDeg);
}

double FrameMemberSpec::longPointMm() const noexcept {
    // THE SUM, NOT THE SUM OF MAGNITUDES. Two cuts leaning the same way put
    // their long points on the same corner and add up; two leaning opposite
    // ways make a parallelogram, whose two long edges are both exactly the
    // axis length. Taking magnitudes here would report a parallelogram brace
    // as longer than it is, at both ends, which is scrap.
    return lengthMm + std::fabs(overhangAMm() + overhangBMm());
}

double FrameMemberSpec::shortPointMm() const noexcept {
    return lengthMm - std::fabs(overhangAMm() + overhangBMm());
}

double FrameMemberSpec::massKg() const noexcept {
    return lengthMm / 1000.0 * profile.massPerMetreKgPerM;
}

std::string FrameMemberSpec::designation() const {
    std::string out = profile.designation() + " L=" + ShortNumber(lengthMm);
    // OMITTED WHEN SQUARE, so an unmitred member's path is the short one -- and
    // so that a member cut square is the SAME PATH however it was made, which
    // is what lets a parts list count it as one line.
    if (!Same(angleADeg, kSquareCutDeg) || !Same(angleBDeg, kSquareCutDeg))
        out += " A=" + ShortNumber(angleADeg) + " B=" + ShortNumber(angleBDeg);
    return out;
}

std::string WhyMemberRefused(const FrameMemberSpec& spec) {
    if (spec.profile.massPerMetreKgPerM <= 0.0)
        return "this is not a section the catalogue holds";
    if (!(spec.lengthMm > 0.0))
        return "a member has to have a length, and " + ShortNumber(spec.lengthMm) + " is not one";
    for (const auto& end : {std::pair<double, const char*>{spec.angleADeg, "first"},
                            std::pair<double, const char*>{spec.angleBDeg, "second"}}) {
        if (!(end.first > 0.0) || !(end.first < 180.0))
            return std::string("the ") + end.second + " end is cut at " +
                   ShortNumber(end.first) +
                   " degrees, and a cut has to be somewhere between 0 and 180";
    }
    // AND THIS IS THE ONE THAT MATTERS. A mitre longer than the member leaves
    // no short point at all: the arithmetic still gives a number, the solid
    // still builds as SOMETHING, and what comes out is a wedge where a length
    // of steel was meant to be. Nothing downstream would notice -- it has a
    // path, a mass and a row on the cut list.
    if (spec.shortPointMm() <= 0.0)
        return "the two cuts meet before the member ends: " + ShortNumber(spec.lengthMm) +
               " mm of axis with these angles leaves a short point of " +
               ShortNumber(spec.shortPointMm()) + " mm, so there is no member left";
    return {};
}

bool IsFrameMemberPath(std::string_view path) noexcept {
    return path.size() > kFrameMemberScheme.size() &&
           path.substr(0, kFrameMemberScheme.size()) == kFrameMemberScheme;
}

std::string FrameMemberPath(const FrameMemberSpec& spec) {
    return std::string(kFrameMemberScheme) + spec.designation();
}

std::optional<FrameMemberSpec> FrameMemberOfPath(std::string_view path) {
    if (!IsFrameMemberPath(path)) return std::nullopt;
    std::string_view rest = path.substr(kFrameMemberScheme.size());

    const std::size_t lengthAt = rest.find(" L=");
    if (lengthAt == std::string_view::npos) return std::nullopt;
    const std::optional<FrameProfile> profile = LookUpSection(rest.substr(0, lengthAt));
    if (!profile) return std::nullopt;

    FrameMemberSpec spec;
    spec.profile = *profile;
    std::size_t at = lengthAt + 3;
    if (!ReadNumber(rest, at, spec.lengthMm)) return std::nullopt;
    if (at != rest.size()) {
        // BOTH ANGLES OR NEITHER. One of them alone would be a path whose
        // other end is a default nobody wrote down, and the default is the
        // square cut -- so it would read as deliberate.
        SkipSpaces(rest, at);
        if (!Expect(rest, at, 'A') || !Expect(rest, at, '=')) return std::nullopt;
        if (!ReadNumber(rest, at, spec.angleADeg)) return std::nullopt;
        SkipSpaces(rest, at);
        if (!Expect(rest, at, 'B') || !Expect(rest, at, '=')) return std::nullopt;
        if (!ReadNumber(rest, at, spec.angleBDeg)) return std::nullopt;
        if (at != rest.size()) return std::nullopt;
    }
    if (!WhyMemberRefused(spec).empty()) return std::nullopt;
    return spec;
}

std::unique_ptr<PartDocument> BuildFrameMember(const FrameMemberSpec& spec) {
    if (!WhyMemberRefused(spec).empty()) return nullptr;

    const FrameProfile& profile = spec.profile;
    auto part = std::make_unique<PartDocument>(spec.designation());
    Body& body = part->addBody(spec.designation());

    // THE SECTION LIES ON THE XY PLANE AND THE MEMBER RUNS ALONG +Z, which is
    // the convention the layout generator places against and the one the mitre
    // arithmetic above is written in. The pad starts at the lowest point ANY
    // corner reaches, so both cuts have material to take away.
    const double overA = std::fabs(spec.overhangAMm());
    const double overB = std::fabs(spec.overhangBMm());
    Sketch& section = part->addSketch("Section", SketchFrame::Translated(Vec3{0.0, 0.0, -overA}));
    switch (profile.kind) {
    case SectionKind::SquareHollow:
    case SectionKind::RectangularHollow:
        AddRoundedRectangle(*part, section.id(), profile.widthMm, profile.heightMm,
                            profile.outerRadiusMm);
        break;
    case SectionKind::CircularHollow:
        part->addSketchEntity(section.id(),
                              SketchCircle{Vec2{0.0, 0.0}, profile.heightMm / 2.0});
        break;
    case SectionKind::EqualAngle:
        AddAngleOutline(*part, section.id(), profile);
        break;
    }

    Parameter& padLength = part->addParameter("Blank length", spec.lengthMm + overA + overB,
                                              UnitType::Millimeter);
    PadFeature& blank = part->addPadFeature(body, "Blank", section.id(), padLength.id());
    ObjectId tip = blank.id();

    // THE BORE, for the two kinds that are hollow. A pocket rather than an
    // inner loop, for the reason M45's nut bore is one: it goes a millimetre
    // past the far face, because a cut ending exactly ON a face leaves OCCT
    // deciding whether the two are coincident and that is what it is worst at.
    if (profile.kind != SectionKind::EqualAngle) {
        Sketch& bore =
            part->addSketch("Bore", SketchFrame::Translated(Vec3{0.0, 0.0, -overA - 1.0}));
        if (profile.kind == SectionKind::CircularHollow) {
            part->addSketchEntity(
                bore.id(),
                SketchCircle{Vec2{0.0, 0.0}, profile.heightMm / 2.0 - profile.thicknessMm});
        } else {
            AddRoundedRectangle(*part, bore.id(), profile.widthMm - 2.0 * profile.thicknessMm,
                                profile.heightMm - 2.0 * profile.thicknessMm,
                                profile.innerRadiusMm);
        }
        Parameter& deep = part->addParameter("Bore depth", spec.lengthMm + overA + overB + 2.0,
                                             UnitType::Millimeter);
        tip = part->addPocketFeature(body, "Bore", tip, bore.id(), deep.id()).id();
    }

    // THE TWO CUTS. Each is a pocket from a sketch lying ON the cut plane and
    // facing AWAY from the member, so what it removes is everything past that
    // plane -- which is what the saw leaves on the floor.
    struct Cut {
        const char* name;
        double angleDeg;
        double alongAxisMm;
        double towards;  // +1 cuts up the member, -1 cuts down it
        double overhang; // how much there is to take off; zero for a square cut
    };
    const Cut cuts[2] = {{"Cut A", spec.angleADeg, 0.0, -1.0, overA},
                         {"Cut B", spec.angleBDeg, spec.lengthMm, +1.0, overB}};
    // Big enough to swallow the member whatever the angle, and squared off --
    // a cut that only just reaches is a cut that stops reaching the first time
    // somebody uses a bigger section.
    const double reach = 4.0 * (profile.heightMm + profile.widthMm + spec.lengthMm);
    for (const Cut& cut : cuts) {
        // A SQUARE CUT IS NO CUT. The pad already ends on that plane, and a
        // pocket that removes nothing is a failed feature (PocketFeature
        // checks for exactly that) -- so the commonest member in any frame
        // would arrive with two broken features in its tree.
        if (cut.overhang <= 1e-9) continue;
        // The cut plane contains the section's Y axis and makes `angle` with
        // the member's axis. Its normal, pointing at the waste, is the axis
        // turned by that angle about Y and then flipped to face outward.
        //
        // THE X COMPONENT DOES NOT FLIP WITH THE END, and that is the whole
        // sign convention. Both cut planes tilt the SAME way for the same
        // angle, so a member cut at 45 at both ends is a picture-frame corner
        // -- long on one side, short on the other -- which is what a shop
        // means by "45 both ends". Flipping x with the end instead makes 45/45
        // a parallelogram, and every mitred frame in the program comes out as
        // a set of parallel-sided sticks that will not close.
        const double radians = cut.angleDeg * kPi / 180.0;
        const Vec3 normal{std::cos(radians), 0.0, cut.towards * std::sin(radians)};
        const std::optional<SketchFrame> frame =
            SketchFrame::OnFace(Vec3{0.0, 0.0, cut.alongAxisMm}, normal);
        if (!frame) continue;
        Sketch& blade = part->addSketch(cut.name, *frame);
        const Vec2 corners[4] = {Vec2{-reach, -reach}, Vec2{reach, -reach}, Vec2{reach, reach},
                                 Vec2{-reach, reach}};
        for (int i = 0; i < 4; ++i)
            part->addSketchEntity(blade.id(), SketchLine{corners[i], corners[(i + 1) % 4]});
        Parameter& deep = part->addParameter(std::string(cut.name) + " depth", reach,
                                             UnitType::Millimeter);
        tip = part->addPocketFeature(body, cut.name, tip, blade.id(), deep.id()).id();
    }

    return part;
}

} // namespace paramcad
