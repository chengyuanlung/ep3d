#pragma once

#include <QIcon>
#include <QPalette>

namespace paramcad::ui {

// M15 -- the sketch command icons, PAINTED rather than loaded.
//
// Three reasons this is code and not a folder of PNGs:
//
//  1. **Resolution.** These are line drawings. A raster asset picked for 24 px
//     is soft at 150% scaling and mushy at 200%, and this project already has
//     a rule that every size is a LOGICAL pixel Qt scales (UI spec 3/15). A
//     painted icon is exact at every ratio because it is re-rendered per size.
//  2. **Theme.** The shell has a dark mode. A baked-in dark stroke disappears
//     on a dark background, and DesignTokens.h already forbids encoding
//     presentation as fixed RGB for exactly that reason -- the first run of
//     this project shipped a Model Tree that was unreadable on a light theme.
//     These take the palette and derive their ink from it.
//  3. **No binary assets.** Nothing to lose, re-export, or license.
//
// The visual language is deliberate and consistent across all of them:
//   * geometry is drawn in the palette's text colour, thin and even;
//   * the RELATIONSHIP an icon is about is drawn in the accent colour, so a
//     constraint icon reads as "these two, like this";
//   * defined points are small filled dots, the same mark the canvas uses.
enum class SketchIcon {
    // Drawing tools
    Select,
    Line,
    Rectangle,
    Circle,
    Arc,
    Point,
    // Geometric constraints
    Coincident,
    Horizontal,
    Vertical,
    Fix,
    Parallel,
    Perpendicular,
    Equal,
    Concentric,
    Midpoint,
    PointOnObject,
    Tangent,
    // Dimensions
    Dimension,
    Radius,
    Diameter,
    HorizontalDistance,
    VerticalDistance,
    HVDistance,
    PointLineDistance,
    Offset,
    Trim,
    Extend,
    Chamfer,
    Fillet,
    Symmetric,
    Mirror,
    AutoPlaceDimensions,
    // Sketch-mode commands
    OriginPoint,
    Construction,
    DeleteGeometry,
    FitSketch,
    NewSketch,
    EditSketch,
    FinishSketch,
    // Solid modelling, on the Model toolbar.
    Pad,
    Pocket,
    Revolve,
    SketchOnFace,
    UseReference,
    CenterRectangle,
    ThreePointCircle,
    ThreePointArc,
    TangentArc,
    Split,
    Transform,
    Ellipse,
    EllipticalArc,
    MajorAxisDimension,
    MinorAxisDimension,
    Spline,
    Polygon,
    DimensionTool,
    ReferenceDimension,
    Slot,
    // M19-M22 solid modelling, on the Model toolbar. Every one of these
    // shipped with "UI: script and API only" against it; this is that debt
    // being paid.
    Sweep,
    Loft,
    Shell,
    Hole,
    Union,
    Subtract,
    Intersect,
    CircularPattern,
    CurvePattern,
    ExportModel,
    ImportModel,
    // Document-level commands on the main toolbar.
    Undo,
    Redo,
    Recompute,
    Visibility,

    // NOT AN ICON. The sweep below is 0..Count, which is what lets
    // AllSketchIcons be derived rather than written down -- and a list written
    // down is what this enum already outgrew once, silently, because nothing
    // used it.
    //
    // Keep it LAST. Anything after it would be invisible to every sweep.
    // --- Assembly (M30.2) ---------------------------------------------------
    //
    // Their own group, drawn to read as being ABOUT PARTS rather than about
    // geometry: a part is a filled block here, where every sketch icon is a
    // line. That is the one visual rule this set follows, and it is what makes
    // an assembly toolbar tell you at a glance that you are not in a part.
    InsertInstance,
    GroundInstance,
    AddMate,
    DriveMate,
    LimitMate,
    AssemblyPattern,
    NamedPosition,
    ExplodeView,
    Interference,
    // M31. Two blocks with a LINK between them, because a relation is not a
    // part and not a mate -- it is the thing that makes one freedom follow
    // another, and the link is the only part of it a user can point at.
    AddRelation,

    // --- Drawing (M32.4) ----------------------------------------------------
    //
    // Their own group again, drawn to read as being ABOUT PAPER: every one of
    // them has a sheet outline in it, where an assembly icon has a filled
    // block and a sketch icon is a bare line. That is the one visual rule this
    // set follows, and it is what tells a user at a glance which document they
    // are in.
    NewDrawing,
    BaseView,
    ProjectedView,
    UpdateViews,
    SheetSetup,
    DrawingLayer,

    // --- Dimensions (M34) ---------------------------------------------------
    //
    // These four break the sheet-outline rule the group above follows, and on
    // purpose: a dimension is not ABOUT the paper, it is about the PART. So
    // each one draws the annotation itself -- the arrows, the arc, the leader
    // -- which is also what makes them tell each other apart at 24 px, where
    // four sheets with small marks inside would be one grey smudge four times.
    LinearDimension,
    RadiusDimension,
    DiameterDimension,
    AngularDimension,
    DimensionStyleIcon,

    // M35. Back to the sheet-outline rule the M32 group follows, because a
    // title block IS about the paper -- it is the one thing on a drawing that
    // describes the drawing rather than the part.
    TitleBlock,

    Count
};

// A stable, human-readable name. Used by the smoke test to say WHICH icon
// failed, so a fingerprint clash names the pair rather than an index.
const char* SketchIconName(SketchIcon icon) noexcept;

// Every icon, in declaration order. DERIVED from the enum's own extent rather
// than written down: the hand-kept list this replaced had drifted by seventeen
// entries without anybody noticing, because nothing called it.
const SketchIcon* AllSketchIcons(int* count) noexcept;

// Paints `icon` for the given palette, at every size the toolbar and menus ask
// for. The result carries several pixmaps, so Qt picks a crisp one instead of
// scaling a single bitmap.
QIcon MakeSketchIcon(SketchIcon icon, const QPalette& palette);

} // namespace paramcad::ui
