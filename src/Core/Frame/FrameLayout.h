#pragma once

#include "Core/Frame/FrameProfile.h"
#include "Core/Geometry/MathTypes.h"

#include <string>
#include <vector>

namespace paramcad {

class AssemblyDocument;

// M56.3 -- THE GENERATOR: lines in, steel out.
//
// A skeleton is what a frame is designed as -- centrelines, drawn where the
// members are meant to run -- and the generator's whole job is to turn each
// line into a member and each meeting point into a cut.
//
// NOTHING HERE IS STORED. A layout is computed from the skeleton, the section
// and the up direction, and what it produces is a list of instances with
// placements. Re-run it after moving a line and every length and every mitre
// downstream of that line is different, because none of them was ever written
// down. This is the same call M53's flat pattern makes and for the same
// reason: a derived answer kept beside the thing it was derived from is a
// second thing that has to be right.

// A LINE OF THE SKELETON, in assembly millimetres. Where the member's own axis
// will run, which is why a mitred corner comes out exactly: two centrelines
// that meet at a point and two mitres that bisect the angle between them close
// with no gap and no arithmetic.
struct SkeletonLine {
    Vec3 fromMm{};
    Vec3 toMm{};
};

// ONE MEMBER, ready to be placed.
struct FrameMemberPlacement {
    std::string name;
    FrameMemberSpec spec;
    // The member is built running along its own +Z from the axis start. This
    // puts that local frame where the skeleton line is.
    Transform3D placement{};

    // "frm:SHS 40x40x3 L=600 A=45 B=45" -- and this is the ONLY thing that
    // reaches the assembly. The member's geometry is not copied in; it is
    // named, and rebuilt from the name (ADR-M22-003).
    std::string sourcePath() const;
};

// WHAT THE GENERATOR MADE, AND WHAT IT WOULD NOT GUESS.
//
// `notes` is not a warning list to be skimmed. Each line names a joint the
// generator LEFT SQUARE and says why -- three members meeting at a point, or
// two whose mitre plane will not lie along the section's own axis. Those are
// joints a fabricator has to decide about, and a generator that picked for
// them would produce a frame that looks finished and is wrong in a place
// nobody is looking any more.
struct FrameLayout {
    bool ok = false;
    std::string why;
    std::vector<FrameMemberPlacement> members;
    std::vector<std::string> notes;

    explicit operator bool() const noexcept { return ok; }
};

// `upMm` is WHICH WAY IS UP for the section -- the direction the profile's own
// +Y points, as near as each member's axis allows. A frame's members are not
// all parallel, so this is a hint and not an instruction; what a member gets is
// the part of it square to that member's axis.
//
// A member whose axis runs ALONG the up direction is refused rather than given
// an arbitrary roll: an upright post in a frame whose up is vertical has no
// "up" left, and rolling it to whatever the arithmetic produced would put its
// section at an angle nobody chose. The caller's answer is to give a different
// hint, and the message says so.
FrameLayout GenerateFrame(const std::vector<SkeletonLine>& skeleton,
                          const FrameProfile& profile, Vec3 upMm = Vec3{0.0, 0.0, 1.0});

// The layout, as instances in an assembly. Returns how many were added.
//
// Placed rather than merged: this adds to whatever is already there, because a
// frame is usually part of something bigger and a generator that cleared the
// assembly first would be a generator that deletes a user's work.
std::size_t PlaceFrame(AssemblyDocument& assembly, const FrameLayout& layout);

} // namespace paramcad
