#pragma once

#include "Core/Drawing/ProjectedGeometry.h"
#include "Core/Geometry/MathTypes.h"

#include <string>

namespace paramcad {

// WHAT A DRAWING VIEW ASKS THE KERNEL FOR (M32.2).
//
// A camera and two switches. The camera comes from ViewDirection's one table
// (see DrawingView.h) so the projector never decides which way "up" is; the
// switches are the drawing's conventions, and they are ASKED FOR rather than
// applied afterwards because hidden-line removal is expensive and computing
// edges nobody will draw is work thrown away.
struct DrawingProjectionRequest {
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

    explicit operator bool() const noexcept { return ok; }
};

} // namespace paramcad
