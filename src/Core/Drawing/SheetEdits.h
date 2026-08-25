#pragma once

#include "Core/Drawing/DrawingEntity.h"
#include "Core/Drawing/Geometry2D.h"

#include <string>
#include <vector>

namespace paramcad {

// M40 -- THE EDITING TOOLS A 2D DRAUGHTSMAN ACTUALLY USES.
//
// Trim, extend, fillet, chamfer, offset, array. Everything in this header is a
// PURE FUNCTION over shapes: shapes in, shapes out, and not one of them
// touches a document, a selection or a mouse. That is deliberate and it is the
// whole reason the file exists separately from the commands that call it.
//
// WHY THAT MATTERS HERE MORE THAN USUAL. Every one of these tools fails the
// same silent way: it does something plausible to the wrong piece. Trim keeps
// the half the user meant to remove; offset goes the wrong side; fillet joins
// the far ends of two lines instead of the near ones. All of those produce a
// perfectly valid drawing that is not the one anybody asked for, and none of
// them can be caught by asking "did it work". They can only be caught by
// asking "which piece came back", and that is a question a pure function can
// be asked in a test and a mouse handler cannot.
//
// SO EVERY TOOL TAKES THE PICK POINT. Not a flag, not a side, not a
// convention: the point on the paper where the user clicked. That is what
// AutoCAD does and it is not an accident of history -- the pick point is the
// only piece of information that says which of two equally valid answers the
// user meant, and it is unambiguous in a way "keep the left half" is not.

struct SheetEditResult {
    bool ok = false;
    std::string why; // set on refusal, and only then

    // What the edited shapes become. Empty with ok means the shapes were
    // consumed entirely -- trimming a line between two cutters that covers all
    // of it -- which is a real outcome and not a failure.
    std::vector<DrawShape> shapes;

    explicit operator bool() const noexcept { return ok; }
};

// TRIM: cut `victim` where it crosses any of `cutters`, and throw away the
// piece the user picked.
//
// The picked piece goes and the rest stays, which is the opposite of what
// somebody writing this from memory usually does. Trimming the middle of a
// line leaves TWO lines, and that is why the result is a list.
SheetEditResult TrimShape(const DrawShape& victim, const std::vector<DrawShape>& cutters,
                          Vec2 pickedAt);

// EXTEND: lengthen the end of `victim` NEAREST the pick until it reaches a
// boundary.
//
// Which end is not a question the user should have to answer twice: they
// picked near one, and that is the one that moves. Refused when nothing lies
// ahead of that end, rather than stretching to some default length -- a line
// that grew by an arbitrary amount looks exactly like one that met something.
SheetEditResult ExtendShape(const DrawShape& victim, const std::vector<DrawShape>& boundaries,
                            Vec2 pickedAt);

// FILLET: an arc of `radiusMm` tangent to both lines, with each line trimmed
// back to where the arc meets it.
//
// The picks say WHICH ENDS are being joined. Two lines that cross have four
// corners and all four are valid fillets; without the picks a tool has to
// guess, and the guess is right about a quarter of the time.
//
// A radius of zero is a corner: the two lines are simply trimmed or extended
// to their intersection, which is what every CAD system does with it and what
// users expect.
SheetEditResult FilletLines(const DrawShape& first, const DrawShape& second, Vec2 pickFirst,
                            Vec2 pickSecond, double radiusMm);

// CHAMFER: the same, with a straight line across the corner instead of an arc.
// `setbackMm` is measured along each line from the corner.
SheetEditResult ChamferLines(const DrawShape& first, const DrawShape& second, Vec2 pickFirst,
                             Vec2 pickSecond, double setbackMm);

// OFFSET: a parallel copy at `distanceMm`, on the side `towards` lies on.
//
// The distance is a LENGTH and is refused if it is not positive; the side
// comes from the point, for the same reason trim takes a pick. A signed
// distance would work for a line and mean nothing for a circle.
SheetEditResult OffsetShape(const DrawShape& shape, double distanceMm, Vec2 towards);

// ARRAY, rectangular: `columns` x `rows` copies at `pitchMm` apart.
//
// The original is INCLUDED, as the first copy. A tool that returned only the
// new ones would leave the caller adding the original back, and the caller
// that forgot would delete it.
SheetEditResult RectangularArray(const std::vector<DrawShape>& shapes, int columns, int rows,
                                 Vec2 pitchMm);

// ARRAY, polar: `count` copies spread over `totalAngleRad` about `centre`.
//
// `rotateItems` is the difference between a ring of bolts (each turned to face
// out) and a ring of labels (each still readable). Both are wanted often
// enough that neither can be the silent default.
SheetEditResult PolarArray(const std::vector<DrawShape>& shapes, Vec2 centre, int count,
                           double totalAngleRad, bool rotateItems);

} // namespace paramcad
