#include "Core/Feature/SheetContour.h"

#include "Core/Text/NumberText.h"

#include <cmath>

namespace paramcad {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTiny = 1e-9;

double Radians(double degrees) noexcept { return degrees * kPi / 180.0; }

// THE MATERIAL IS ON THE LEFT OF TRAVEL, everywhere along the chain.
//
// One convention, chosen once, and the reason it has to be chosen once: the
// inside of a bend is on the left when it turns left and on the right when it
// turns right, so "the inner face" is not a single surface along a chain that
// turns both ways. What IS a single surface is "the face the walk traces", and
// everything below is written in terms of that.
Vec2 LeftOf(Vec2 direction) noexcept { return Vec2{-direction.y, direction.x}; }

Vec2 Turned(Vec2 direction, double radians) noexcept {
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return Vec2{direction.x * c - direction.y * s, direction.x * s + direction.y * c};
}

Vec2 Plus(Vec2 a, Vec2 b) noexcept { return Vec2{a.x + b.x, a.y + b.y}; }
Vec2 Times(Vec2 a, double k) noexcept { return Vec2{a.x * k, a.y * k}; }

// Where a bend's centre sits, and what radius each face runs at.
//
// A LEFT TURN FOLDS TOWARDS THE MATERIAL, so the traced face is on the OUTSIDE
// of the bend and runs at R + T; the far face is the inside, at R. A right
// turn is the other way about. Getting this backwards builds a part whose
// bends are a thickness out -- which on a drawing of the section looks
// entirely reasonable.
struct BendGeometry {
    Vec2 centre{};
    double tracedRadius = 0.0;
    double farRadius = 0.0;
};

BendGeometry GeometryOf(Vec2 at, Vec2 direction, const ContourStep& step,
                        double thicknessMm) {
    const Vec2 left = LeftOf(direction);
    BendGeometry out;
    if (step.turnsLeft) {
        out.tracedRadius = step.bend.innerRadiusMm + thicknessMm;
        out.farRadius = step.bend.innerRadiusMm;
        out.centre = Plus(at, Times(left, out.tracedRadius));
    } else {
        out.tracedRadius = step.bend.innerRadiusMm;
        out.farRadius = step.bend.innerRadiusMm + thicknessMm;
        out.centre = Plus(at, Times(left, -out.tracedRadius));
    }
    return out;
}

} // namespace

std::string WhyContourRefused(const SheetContour& contour, SheetMaterial material,
                              double thicknessMm) {
    if (thicknessMm <= kTiny) return "a contour has no section without a thickness";
    if (contour.lastFlangeMm <= kTiny)
        return "the run after the last bend is not a flange -- it has no length";
    for (const ContourStep& step : contour.steps) {
        if (step.flangeMm <= kTiny)
            return "a flange of no length is not a flange";
        // THE SAME BEND RULES M51 STATES, asked of the same function rather
        // than restated here: a radius the material cracks at, a bend of
        // nothing, a hem.
        const std::string why = WhyBendRefused(step.bend, material, thicknessMm);
        if (!why.empty()) return why;
    }
    return {};
}

FlatPatternResult ContourFlatLength(const SheetContour& contour, SheetMaterial material,
                                    double thicknessMm) {
    FlatPatternResult out;
    out.why = WhyContourRefused(contour, material, thicknessMm);
    if (!out.why.empty()) return out;
    // UNPACKED INTO WHAT M51 ALREADY READS. Not a second implementation of the
    // bend allowance: there is one in this program, and one place it is wrong
    // if it is wrong.
    std::vector<double> tangents;
    std::vector<SheetBend> bends;
    tangents.reserve(contour.steps.size() + 1);
    bends.reserve(contour.steps.size());
    for (const ContourStep& step : contour.steps) {
        tangents.push_back(step.flangeMm);
        bends.push_back(step.bend);
    }
    tangents.push_back(contour.lastFlangeMm);
    return FlatLengthFromTangents(tangents, bends, material, thicknessMm);
}

double FoldedSectionAreaMm2(const SheetContour& contour, double thicknessMm) {
    if (thicknessMm <= kTiny) return 0.0;
    double area = 0.0;
    for (const ContourStep& step : contour.steps) {
        area += step.flangeMm * thicknessMm;
        // THE ANNULUS SEGMENT, which is what the metal in a bend actually
        // occupies -- and see the header for why this is NOT the flat blank's
        // share of the same bend.
        //
        // AND IT IS EXACTLY THE MID-SURFACE ARC LENGTH TIMES THE THICKNESS:
        // angle/2 * ((r+T)^2 - r^2) is angle * T * (r + T/2), the same number
        // written twice. The mutation gate proved that by swapping one for the
        // other and nothing moving. It is worth knowing rather than tidying
        // away, because it is the reason K = 1/2 is the break-even: the flat
        // blank spends angle * T * (r + K*T) on the same bend, so the two
        // agree when, and only when, the neutral line sits at mid-thickness.
        const double r = step.bend.innerRadiusMm;
        area += 0.5 * Radians(step.bend.angleDeg) *
                ((r + thicknessMm) * (r + thicknessMm) - r * r);
    }
    area += contour.lastFlangeMm * thicknessMm;
    return area;
}

ContourProfileResult ContourProfile(const SheetContour& contour, SheetMaterial material,
                                    double thicknessMm) {
    ContourProfileResult out;
    out.why = WhyContourRefused(contour, material, thicknessMm);
    if (!out.why.empty()) return out;

    // ONE WALK, TWO FACES. Both are produced from the same positions and the
    // same centres, so the solid cannot come out with one side bent to a
    // different radius from the other -- which is what two walks would risk,
    // and what nothing downstream could see.
    struct Piece {
        bool isArc = false;
        Vec2 from{};
        Vec2 to{};
        Vec2 centre{};
        double tracedRadius = 0.0;
        double farRadius = 0.0;
        double startAngle = 0.0;
        double endAngle = 0.0;
        bool counterClockwise = true;
        Vec2 farFrom{};
        Vec2 farTo{};
    };
    std::vector<Piece> pieces;

    Vec2 at{0.0, 0.0};
    Vec2 direction{1.0, 0.0};
    const double t = thicknessMm;

    const auto straight = [&](double length) {
        Piece piece;
        piece.isArc = false;
        piece.from = at;
        piece.to = Plus(at, Times(direction, length));
        const Vec2 left = LeftOf(direction);
        piece.farFrom = Plus(piece.from, Times(left, t));
        piece.farTo = Plus(piece.to, Times(left, t));
        pieces.push_back(piece);
        at = piece.to;
    };

    for (const ContourStep& step : contour.steps) {
        straight(step.flangeMm);

        const BendGeometry geometry = GeometryOf(at, direction, step, t);
        const double sweep = Radians(step.bend.angleDeg);
        Piece piece;
        piece.isArc = true;
        piece.centre = geometry.centre;
        piece.tracedRadius = geometry.tracedRadius;
        piece.farRadius = geometry.farRadius;
        piece.counterClockwise = step.turnsLeft;
        // The angle from the centre to where the walk currently is. Taken from
        // the geometry rather than accumulated, so a chain of bends cannot
        // drift a fraction of a degree per corner.
        const Vec2 spoke{at.x - geometry.centre.x, at.y - geometry.centre.y};
        piece.startAngle = std::atan2(spoke.y, spoke.x);
        piece.endAngle = piece.startAngle + (step.turnsLeft ? sweep : -sweep);
        piece.from = at;

        const Vec2 endSpoke{std::cos(piece.endAngle), std::sin(piece.endAngle)};
        piece.to = Plus(geometry.centre, Times(endSpoke, geometry.tracedRadius));
        // THE FAR FACE SHARES THE CENTRE AND THE ANGLES. Only the radius
        // differs, which is what makes the two faces concentric by
        // construction rather than by arithmetic that has to agree -- and it
        // is why an arc needs no far ENDPOINTS of its own. Its own centre,
        // radius and angles say where it runs.
        //
        // The first draft computed them anyway, and the mutation gate found
        // they were never read: the caps at each end of the loop are drawn at
        // pieces.front() and pieces.back(), and the walk always begins and
        // ends with a STRAIGHT. Dead code that looked like a safety net.
        pieces.push_back(piece);

        at = piece.to;
        direction = Turned(direction, step.turnsLeft ? sweep : -sweep);
    }
    straight(contour.lastFlangeMm);

    if (pieces.empty()) {
        out.why = "this contour has nothing in it";
        return out;
    }

    // THE TRACED FACE, forward.
    for (const Piece& piece : pieces) {
        if (piece.isArc) {
            ProfileArcSegment arc;
            arc.center = piece.centre;
            arc.radiusMm = piece.tracedRadius;
            arc.startAngleRad = piece.startAngle;
            arc.endAngleRad = piece.endAngle;
            arc.counterClockwise = piece.counterClockwise;
            out.segments.push_back(arc);
        } else {
            out.segments.push_back(ProfileLineSegment{piece.from, piece.to});
        }
    }
    // The cap at the far end. ALWAYS A STRAIGHT: the walk lays a flange down
    // before every bend and one after the last, so a chain can neither begin
    // nor end on an arc. Said here because the cap reads a straight's far
    // endpoints, and an arc does not have any.
    out.segments.push_back(ProfileLineSegment{pieces.back().to, pieces.back().farTo});
    // THE FAR FACE, backward -- and every arc reversed with it. An arc left
    // running the other way closes the loop geometrically and folds the face
    // back on itself, which OCCT refuses with a message about the wire rather
    // than about the part.
    for (std::size_t i = pieces.size(); i-- > 0;) {
        const Piece& piece = pieces[i];
        if (piece.isArc) {
            ProfileArcSegment arc;
            arc.center = piece.centre;
            arc.radiusMm = piece.farRadius;
            arc.startAngleRad = piece.endAngle;
            arc.endAngleRad = piece.startAngle;
            arc.counterClockwise = !piece.counterClockwise;
            out.segments.push_back(arc);
        } else {
            out.segments.push_back(ProfileLineSegment{piece.farTo, piece.farFrom});
        }
    }
    // ...and the cap at the near end, back to where the walk began.
    out.segments.push_back(ProfileLineSegment{pieces.front().farFrom, pieces.front().from});
    out.ok = true;
    return out;
}

} // namespace paramcad
