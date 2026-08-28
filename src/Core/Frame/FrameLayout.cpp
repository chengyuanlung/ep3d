#include "Core/Frame/FrameLayout.h"

#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Assembly/Instance.h"
#include "Core/Sketch/SketchFrame.h"
#include "Core/Text/NumberText.h"

#include <cmath>
#include <cstddef>
#include <optional>

namespace paramcad {

namespace {

constexpr double kPi = 3.14159265358979323846;
// A JOINT IS A POINT TWO LINES SHARE, and "share" has to have a tolerance
// because a skeleton comes from a sketch solver. A hundredth of a millimetre
// is far below anything a frame is built to and far above the noise a solve
// leaves behind.
constexpr double kJointTolMm = 0.01;

double Length(Vec3 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 Minus(Vec3 a, Vec3 b) noexcept { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Plus(Vec3 a, Vec3 b) noexcept { return Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Scale(Vec3 v, double by) noexcept { return Vec3{v.x * by, v.y * by, v.z * by}; }
double Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
std::optional<Vec3> Unit(Vec3 v) noexcept {
    const double length = Length(v);
    if (!std::isfinite(length) || length < 1e-12) return std::nullopt;
    return Scale(v, 1.0 / length);
}

// One end of one line, and what it is attached to.
struct End {
    std::size_t line = 0;
    bool atStart = true;
    Vec3 whereMm{};
    // The direction leading AWAY from this end into the member. The mitre at a
    // joint is a question about the two directions leaving it, and taking them
    // both the same way is what keeps the arithmetic from having a handedness.
    Vec3 awayMm{};
};

FrameLayout refuse(std::string why) {
    FrameLayout out;
    out.why = std::move(why);
    return out;
}

} // namespace

std::string FrameMemberPlacement::sourcePath() const { return FrameMemberPath(spec); }

FrameLayout GenerateFrame(const std::vector<SkeletonLine>& skeleton,
                          const FrameProfile& profile, Vec3 upMm) {
    if (skeleton.empty()) return refuse("there are no lines to make members from");
    if (profile.massPerMetreKgPerM <= 0.0)
        return refuse("'" + profile.designation() + "' is not a section the catalogue holds");
    const std::optional<Vec3> up = Unit(upMm);
    if (!up) return refuse("the up direction has no length, so there is no way up for a section");

    // Every end, with the direction leading away from it.
    std::vector<End> ends;
    std::vector<Vec3> directions(skeleton.size());
    for (std::size_t i = 0; i < skeleton.size(); ++i) {
        const std::optional<Vec3> along = Unit(Minus(skeleton[i].toMm, skeleton[i].fromMm));
        if (!along)
            return refuse("line " + std::to_string(i + 1) +
                          " begins and ends in the same place, so it is not a member");
        directions[i] = *along;
        ends.push_back(End{i, true, skeleton[i].fromMm, *along});
        ends.push_back(End{i, false, skeleton[i].toMm, Scale(*along, -1.0)});
    }

    FrameLayout out;
    for (std::size_t i = 0; i < skeleton.size(); ++i) {
        const Vec3 along = directions[i];

        // THE SECTION'S OWN UP, square to this member. A frame's members are
        // not parallel, so the hint is projected rather than used: what a
        // member gets is the part of the hint that is perpendicular to it.
        const double leaning = Dot(*up, along);
        const std::optional<Vec3> sectionUp =
            Unit(Minus(*up, Scale(along, leaning)));
        if (!sectionUp)
            return refuse("member " + std::to_string(i + 1) +
                          " runs along the up direction, so there is no way up left for its "
                          "section -- give the generator a different up direction");
        // Columns (x, y, z) with z along the member: exactly what
        // SketchFrame::FromBasis builds, which is the ONE place in this
        // program that turns a basis into a rotation (ADR-M4-002). Writing a
        // second Shepperd conversion here to avoid borrowing a sketch type
        // would be the trade this codebase has refused every time it came up.
        const Vec3 sectionX = Cross(*sectionUp, along);
        const std::optional<SketchFrame> basis =
            SketchFrame::FromBasis(skeleton[i].fromMm, sectionX, along);
        if (!basis)
            return refuse("member " + std::to_string(i + 1) + " has no square section frame");

        FrameMemberPlacement placed;
        placed.name = "Member " + std::to_string(i + 1);
        placed.spec.profile = profile;
        placed.spec.lengthMm = Length(Minus(skeleton[i].toMm, skeleton[i].fromMm));
        placed.placement = basis->transform();

        // --- THE CUTS -------------------------------------------------------
        for (const End& end : ends) {
            if (end.line != i) continue;
            // Everything else that reaches this same point.
            const End* neighbour = nullptr;
            int met = 0;
            for (const End& other : ends) {
                if (other.line == i && other.atStart == end.atStart) continue;
                if (Length(Minus(other.whereMm, end.whereMm)) > kJointTolMm) continue;
                ++met;
                neighbour = &other;
            }
            const std::string where = placed.name + (end.atStart ? " (start)" : " (end)");
            if (met == 0) {
                // A FREE END IS CUT SQUARE, and that is right -- unless the end
                // is not free at all but LANDS ON ANOTHER MEMBER'S SPAN.
                //
                // Found by M56's own tests, which assumed a mullion running to
                // the middle of a rail was a joint. It is not one by the rule
                // this generator uses -- a joint is a point two ENDS share --
                // so it was passing through silently. On a centreline skeleton
                // that means a member driven half a section deep into the one
                // it lands on: a real interference, produced quietly, in a
                // frame that otherwise looks generated.
                //
                // Trimming it needs the neighbour's SOLID, not its line, and
                // which face to trim back to is a fabrication decision of the
                // same kind a three-way joint is. So it is named, not guessed.
                for (std::size_t j = 0; j < skeleton.size(); ++j) {
                    if (j == i) continue;
                    const Vec3 span = Minus(skeleton[j].toMm, skeleton[j].fromMm);
                    const Vec3 offset = Minus(end.whereMm, skeleton[j].fromMm);
                    const double squared = Dot(span, span);
                    if (squared < 1e-18) continue;
                    const double at = Dot(offset, span) / squared;
                    if (at <= kJointTolMm || at >= 1.0 - kJointTolMm) continue;
                    if (Length(Minus(offset, Scale(span, at))) > kJointTolMm) continue;
                    out.notes.push_back(
                        where + ": this end lands part way along Member " +
                        std::to_string(j + 1) +
                        " rather than on one of its ends, so it needs trimming back to that "
                        "member's face -- this program cuts at joints only, and left square "
                        "it will run into the member it meets");
                    break;
                }
                continue;
            }
            if (met > 1) {
                // THREE MEMBERS AT A POINT IS A DECISION, NOT A CALCULATION.
                // One of them runs through and the others are trimmed to it,
                // and which one runs through changes the cut list. A generator
                // that picked would produce a frame that looks finished.
                out.notes.push_back(where + ": " + std::to_string(met + 1) +
                                    " members meet here, so which one runs through is a "
                                    "choice -- left cut square");
                continue;
            }

            // The mitre plane bisects the angle between the two directions
            // LEAVING the joint, so its normal is the difference of them. The
            // sign is chosen to face the waste: away from the member at the
            // start end, past it at the far one.
            // ONE EXPRESSION FOR BOTH ENDS. The first draft had a branch here
            // -- a difference at the start, a sum at the far end -- and the
            // two turned out to be the same vector written twice: at the far
            // end the member's own away direction IS the negated axis, so the
            // sum had already been taken. Two spellings of one formula is how
            // one end gets a fix the other does not.
            const std::optional<Vec3> normal = Unit(Minus(neighbour->awayMm, end.awayMm));
            if (!normal) {
                out.notes.push_back(where +
                                    ": the two members here run along the same line, so there "
                                    "is no angle to bisect -- left cut square");
                continue;
            }
            // AND IT HAS TO BE A CUT THIS SAW CAN MAKE. A member is cut about
            // the section's own up axis; a joint whose bisecting plane leans
            // out of that is a compound cut, which this program does not carry
            // -- the path has one angle per end and inventing a second would
            // be a number nothing downstream could read.
            if (std::fabs(Dot(*normal, *sectionUp)) > 1e-6) {
                out.notes.push_back(
                    where + ": the members here are not in one plane with the section's up "
                            "direction, so the mitre would be a compound cut -- left cut square");
                continue;
            }
            const double alongCut = Dot(*normal, along);
            const double acrossCut = Dot(*normal, sectionX);
            const double angleDeg =
                std::atan2(end.atStart ? -alongCut : alongCut, acrossCut) * 180.0 / kPi;
            (end.atStart ? placed.spec.angleADeg : placed.spec.angleBDeg) = angleDeg;
        }

        const std::string refused = WhyMemberRefused(placed.spec);
        if (!refused.empty())
            return refuse(placed.name + " cannot be made: " + refused);
        out.members.push_back(std::move(placed));
    }

    out.ok = true;
    return out;
}

std::size_t PlaceFrame(AssemblyDocument& assembly, const FrameLayout& layout) {
    if (!layout.ok) return 0;
    std::size_t added = 0;
    for (const FrameMemberPlacement& member : layout.members) {
        Instance& one = assembly.addInstance(member.name, member.sourcePath(), "");
        assembly.setInstanceTransform(one.id(), member.placement);
        ++added;
    }
    return added;
}

} // namespace paramcad
