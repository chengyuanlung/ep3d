#pragma once

#include "Core/Drawing/DrawingEntity.h"

#include <optional>
#include <string_view>
#include <vector>

namespace paramcad {

// OBJECT SNAP (M33), ported from EasyCad's `EasyCad.Core.Snap`.
//
// The nine modes AutoCAD ships and the PRIORITY between them, which is the
// part that is not obvious and the part EasyCad had already got right:
//
//   END > MID > CEN > NOD > QUA > INT > PER > TAN > NEA
//
// The order matters because several modes usually match at once near a corner,
// and a package that took the closest would give a different answer every time
// the cursor moved a pixel. Taking the highest-priority match within the
// aperture makes it PREDICTABLE, which is the whole reason a drafter trusts
// snapping enough to stop zooming in.
//
// NEAREST IS LAST AND ALWAYS MATCHES. It is the fallback that makes the
// aperture feel continuous -- without it the cursor jumps free between snap
// points and the user cannot tell whether snapping is on.
enum class SnapMode {
    Endpoint,
    Midpoint,
    Centre,
    Node,
    Quadrant,
    Intersection,
    Perpendicular,
    Tangent,
    Nearest,
};

std::string_view toString(SnapMode mode) noexcept;
// Every mode, in PRIORITY ORDER. Derived from the enum's own order rather than
// written down twice -- the second list is what drifts.
const std::vector<SnapMode>& SnapModesByPriority();

struct SnapHit {
    bool found = false;
    Vec2 at{};
    SnapMode mode = SnapMode::Nearest;
    ObjectId entityId = kInvalidObjectId;

    explicit operator bool() const noexcept { return found; }
};

// WHICH MODES ARE ON, as a set of flags. A drafter turns individual ones off
// -- CENTRE is a nuisance while tracing a bolt circle -- so this is not "all
// or nothing".
struct SnapSettings {
    bool endpoint = true;
    bool midpoint = true;
    bool centre = true;
    bool node = true;
    bool quadrant = true;
    bool intersection = true;
    bool perpendicular = false; // needs an anchor, so off until one is asked for
    bool tangent = false;
    bool nearest = false; // last resort, and noisy when always on
    // The catch radius, in SHEET millimetres. The caller converts from screen
    // pixels, because how big a pixel is belongs to the view and not here.
    double apertureMm = 2.0;

    bool isOn(SnapMode mode) const noexcept;
    // Everything on, which is what OSNAP ALL means.
    static SnapSettings all() noexcept;
    static SnapSettings none() noexcept;
};

// THE SNAP, over a set of entities.
//
// `anchor` is where the current operation started -- the previous click of a
// LINE, say. PERPENDICULAR and TANGENT are meaningless without one, which is
// why they default off: a perpendicular from nowhere is not a point.
SnapHit SnapTo(const std::vector<const DrawingEntity*>& entities, Vec2 cursor,
               const SnapSettings& settings,
               const std::optional<Vec2>& anchor = std::nullopt);

// Every candidate a single entity offers for the static modes (END, MID, CEN,
// NOD, QUA). Split out because it is what a drawing's canvas paints as the
// little markers, and because it is the half that needs no anchor.
struct SnapCandidate {
    Vec2 at{};
    SnapMode mode = SnapMode::Endpoint;
};
std::vector<SnapCandidate> StaticSnapPointsOf(const DrawShape& shape);

} // namespace paramcad
