#pragma once

#include "Core/Drawing/ProjectedGeometry.h"
#include "Core/Geometry/MathTypes.h"

#include <string>
#include <vector>

namespace paramcad {

// WHAT A DRAWING VIEW ASKS THE KERNEL FOR (M32.2).
//
// A camera and two switches. The camera comes from ViewDirection's one table
// (see DrawingView.h) so the projector never decides which way "up" is; the
// switches are the drawing's conventions, and they are ASKED FOR rather than
// applied afterwards because hidden-line removal is expensive and computing
// edges nobody will draw is work thrown away.
// WHERE THE KNIFE WENT (M38).
//
// A section view is not a new kind of view: it is the ordinary projection of a
// solid that has had a half-space taken out of it. So the cut travels with the
// projection request, and everything downstream -- hidden lines, silhouettes,
// the extent, the scale -- keeps working without knowing about it.
//
// THE NORMAL POINTS AT THE MATERIAL THAT IS REMOVED. Stated because it is a
// coin toss otherwise, and getting it backwards produces a perfectly plausible
// drawing of the wrong half.
struct SectionPlane {
    bool active = false;
    Vec3 origin{0.0, 0.0, 0.0};
    Vec3 normal{0.0, 1.0, 0.0};
};

struct DrawingProjectionRequest {
    // Nothing is cut when this is inactive, which is every ordinary view.
    SectionPlane section;

    Vec3 towards{0.0, 1.0, 0.0}; // direction of sight, in model space
    Vec3 up{0.0, 0.0, 1.0};      // which way is up the page

    // Hidden edges are conventionally dashed on a mechanical drawing, and
    // conventionally absent on a presentation view. Both are ordinary, so
    // both are asked for.
    bool includeHidden = true;
    // Tangent edges -- where a fillet meets a face without a crease. Standard
    // practice is NOT to draw them, so this defaults off; Inventor calls the
    // same switch "tangent edges".
    bool includeSmooth = false;
};

struct DrawingProjectionResult {
    bool ok = false;
    std::string message; // always set, on success and refusal alike
    ProjectedDrawing drawing;
    // THE CUT FACES, as closed loops in the same MODEL millimetres the curves
    // are in -- ready for the hatch engine and for nothing else.
    //
    // Empty for an ordinary view. Kept apart from `drawing` because they are
    // not edges to be drawn: they are an AREA, and drawing their outline again
    // would double the weight of every line the section already has.
    std::vector<std::vector<Vec2>> cutLoops;

    explicit operator bool() const noexcept { return ok; }
};

} // namespace paramcad
