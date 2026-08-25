#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Drawing/DrawingTables.h"
#include "Core/Drawing/Geometry2D.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace paramcad {

// WHAT A USER DRAWS ON THE SHEET (M33).
//
// The AUTHORED half of a drawing, as against the DERIVED half a view projects.
// This is the distinction M32.1 built the document around, and it is why these
// are a different type from ProjectedCurve rather than the same one with a
// flag: an authored entity has an ObjectId, a layer, undo and selection; a
// projected curve has none of those and is thrown away on the next rebuild.
//
// PORTED FROM EASYCAD (`EasyCad.Core.Entities`), whose entity model had
// already made these choices and proved them against DXF. Two things changed
// on the way across:
//
//   * ITS `Handle` IS GONE. EP3D has ObjectId; a second identity per object is
//     the seam this project spends its milestones removing.
//   * A VARIANT, NOT A CLASS HIERARCHY. The same decision SketchConstraint
//     records: a shape that cannot hold the wrong members cannot be
//     constructed wrong. A LineEnt with a radius field is a line somebody will
//     eventually give a radius to.

// --- The shapes --------------------------------------------------------------

struct DrawPoint {
    Vec2 at{};
};

struct DrawLine {
    Vec2 a{};
    Vec2 b{};
};

struct DrawCircle {
    Vec2 centre{};
    double radius = 0.0;
};

// ALWAYS COUNTER-CLOCKWISE from `startAngle` to `endAngle`. One convention, so
// nothing downstream has to ask which way round -- a mirrored arc has its
// angles rewritten rather than a direction flag added, because a flag would be
// a second thing every consumer had to read.
struct DrawArc {
    Vec2 centre{};
    double radius = 0.0;
    double startAngle = 0.0;
    double endAngle = 0.0;
};

struct DrawEllipse {
    Vec2 centre{};
    double majorRadius = 0.0;
    double minorRadius = 0.0;
    double rotation = 0.0; // of the MAJOR axis, from +X
};

// A VERTEX CARRIES A BULGE, which is AutoCAD's way of putting an arc in a
// polyline: the tangent of a quarter of the included angle, signed for
// direction. Kept because it is what DXF stores, and converting on the way in
// and out would make a round trip lossy at every vertex.
struct DrawVertex {
    Vec2 at{};
    double bulge = 0.0;
};

struct DrawPolyline {
    std::vector<DrawVertex> vertices;
    bool closed = false;
};

struct DrawText {
    Vec2 at{};
    std::string text;
    double heightMm = 3.5; // ISO 3098's smallest common size
    double rotation = 0.0;
};

using DrawShape =
    std::variant<DrawPoint, DrawLine, DrawCircle, DrawArc, DrawEllipse, DrawPolyline, DrawText>;

std::string_view ShapeName(const DrawShape& shape) noexcept;

// --- The entity --------------------------------------------------------------

// WHICH LAYER, and the three overrides DXF allows an entity to make over it.
//
// ByLayer is the DEFAULT and it matters: a drawing where every entity carried
// its own colour is a drawing where changing a layer's colour changes nothing,
// which is the whole reason layers exist.
class DrawingEntity {
public:
    DrawingEntity(DrawShape shape, ObjectId layerId);
    DrawingEntity(ObjectId id, DrawShape shape, ObjectId layerId, int color,
                  std::string linetype, int lineweight);

    ObjectId id() const noexcept { return id_; }
    static std::string_view typeName() noexcept { return "DrawingEntity"; }

    const DrawShape& shape() const noexcept { return shape_; }
    void setShape(DrawShape shape) { shape_ = std::move(shape); }

    ObjectId layerId() const noexcept { return layerId_; }

    // WHICH PAGE THIS SITS ON (M44). kInvalidObjectId means the drawing's
    // first page, which is what every object made before there was more than
    // one page belongs to.
    ObjectId sheetId() const noexcept { return sheetId_; }
    void setSheetId(ObjectId sheetId) noexcept { sheetId_ = sheetId; }

    void setLayerId(ObjectId layerId) noexcept { layerId_ = layerId; }

    int color() const noexcept { return color_; }
    void setColor(int color) noexcept { color_ = color; }
    const std::string& linetype() const noexcept { return linetype_; }
    void setLinetype(std::string linetype) { linetype_ = std::move(linetype); }
    int lineweight() const noexcept { return lineweight_; }
    void setLineweight(int lineweight) noexcept { lineweight_ = lineweight; }

    // --- What every entity can answer -----------------------------------------
    //
    // EasyCad's virtuals, as free functions over the variant. Each is ONE
    // place, so a new shape is a compiler error in every one of them rather
    // than a silent gap in the odd one somebody forgot.
    Box2D bounds() const;
    // Pick distance in sheet millimetres, or a very large number for no hit.
    double distanceTo(Vec2 point) const;
    Vec2 closestPointTo(Vec2 point) const;
    // As a polyline, for hatching, plotting and anything that cannot draw
    // curves. `chordToleranceMm` decides how closely.
    std::vector<Vec2> flatten(double chordToleranceMm) const;
    void applyTransform(const Matrix2D& transform);

private:
    ObjectId id_;
    DrawShape shape_;
    // M44. Set when the object is added, from whichever page was current.
    ObjectId sheetId_ = kInvalidObjectId;
    ObjectId layerId_{kInvalidObjectId};
    int color_ = kColorByLayer;
    std::string linetype_{"BYLAYER"};
    int lineweight_ = kLineweightByLayer;
};

// The same answers for a bare shape, which is what a command needs while it is
// still deciding whether to make an entity at all.
Box2D BoundsOf(const DrawShape& shape);
double DistanceFrom(const DrawShape& shape, Vec2 point);
Vec2 ClosestPointOn(const DrawShape& shape, Vec2 point);
std::vector<Vec2> FlattenShape(const DrawShape& shape, double chordToleranceMm);
DrawShape TransformShape(const DrawShape& shape, const Matrix2D& transform);

} // namespace paramcad
