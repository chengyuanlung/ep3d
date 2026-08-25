// M32.2 -- hidden-line removal, which is what makes a drawing view a drawing
// rather than a wireframe.
//
// OCCT ships two projectors and they are not interchangeable:
//
//   HLRBRep_Algo      exact. Works on the real curves, so a cylinder's
//                     silhouette comes back as a real curve and a hole comes
//                     back as a real circle. Slow on big assemblies.
//   HLRBRep_PolyAlgo  approximate. Works on the triangulation, so everything
//                     comes back as short segments -- including holes.
//
// THE EXACT ONE, deliberately. A drawing is measured: a diameter dimension
// attaches to a circle, and a circle that arrived as a 40-segment polygon has
// no centre and no radius to attach to. The speed is the price of the drawing
// being a drawing. If an assembly is ever too slow for it, the answer is to
// project less often -- not to project something that cannot be dimensioned.

#include "Core/Kernel/DrawingProjection.h"
#include "Kernel/Occt/OcctShape.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRep_Tool.hxx>
#include <GeomAbs_CurveType.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakeHalfSpace.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <HLRAlgo_Projector.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pln.hxx>
#include <HLRBRep_Algo.hxx>
#include <HLRBRep_HLRToShape.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Lin.hxx>
#include <gp_Pnt.hxx>

#include <cmath>
#include <string>
#include <vector>

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// HLR hands back edges lying in the projection plane -- z is the depth it
// already used and is not part of the drawing. Dropping it here, in one place,
// is what makes everything downstream two-dimensional.
Vec2 Flatten(const gp_Pnt& point) { return Vec2{point.X(), point.Y()}; }

// One HLR edge, named as precisely as OCCT will let us.
//
// ANALYTIC WHERE THE MODEL WAS ANALYTIC (see ProjectedGeometry.h). The
// fallback is a tessellation, not a refusal: a drawing that dropped every
// curve it could not classify would be missing geometry the model has.
ProjectedShape ShapeOfEdge(const TopoDS_Edge& edge) {
    BRepAdaptor_Curve curve(edge);
    const double first = curve.FirstParameter();
    const double last = curve.LastParameter();

    switch (curve.GetType()) {
        case GeomAbs_Line: {
            ProjectedLine line;
            line.a = Flatten(curve.Value(first));
            line.b = Flatten(curve.Value(last));
            return line;
        }
        case GeomAbs_Circle: {
            const gp_Circ circle = curve.Circle();
            // ONLY A CIRCLE WHOSE PLANE IS THE PAPER stays a circle. One seen
            // edge-on projects to a straight line and one seen obliquely to an
            // ellipse, and calling either of those a circle would put a
            // diameter dimension on a curve that has no diameter. The check is
            // on the axis: parallel to the line of sight means face-on, and
            // after HLR the line of sight is +Z.
            const gp_Dir axis = circle.Axis().Direction();
            if (std::fabs(std::fabs(axis.Z()) - 1.0) < 1.0e-9) {
                ProjectedArc arc;
                arc.centre = Flatten(circle.Location());
                arc.radius = circle.Radius();
                arc.startAngle = first;
                arc.endAngle = last;
                arc.isFullCircle = std::fabs((last - first) - kTwoPi) < 1.0e-9;
                // OCCT parametrises about the circle's own axis. A circle
                // whose axis points at the viewer rather than away runs the
                // other way round the page, so its angles are mirrored --
                // without this a half-arc comes back as the half that is not
                // there.
                if (axis.Z() < 0.0 && !arc.isFullCircle) {
                    const double start = -last;
                    const double end = -first;
                    arc.startAngle = start;
                    arc.endAngle = end;
                }
                // ...and the parametrisation is about the circle's location,
                // which after HLR is already in page coordinates, so the
                // angles are page angles. Normalised so a consumer can compare
                // them without knowing that.
                while (arc.startAngle < 0.0) {
                    arc.startAngle += kTwoPi;
                    arc.endAngle += kTwoPi;
                }
                return arc;
            }
            break;
        }
        default: break;
    }

    // THE FALLBACK. Sampled along the parameter, which is not the same as
    // sampled evenly along the curve -- good enough to draw, and honestly
    // labelled as a polyline so nothing downstream mistakes it for a measured
    // curve.
    ProjectedPolyline polyline;
    constexpr int kSamples = 24;
    for (int i = 0; i <= kSamples; ++i) {
        const double t = first + (last - first) * (static_cast<double>(i) / kSamples);
        polyline.points.push_back(Flatten(curve.Value(t)));
    }
    return polyline;
}

void CollectCompound(const TopoDS_Shape& compound, ProjectedEdgeKind kind,
                     ProjectedVisibility visibility, ProjectedDrawing& into) {
    if (compound.IsNull()) return;
    for (TopExp_Explorer it(compound, TopAbs_EDGE); it.More(); it.Next()) {
        ProjectedCurve curve;
        curve.shape = ShapeOfEdge(TopoDS::Edge(it.Current()));
        curve.kind = kind;
        curve.visibility = visibility;
        GrowExtent(into.extent, curve);
        into.curves.push_back(std::move(curve));
    }
}

} // namespace

DrawingProjectionResult ProjectShapeForDrawing(const KernelShape& shape,
                                               const DrawingProjectionRequest& request) {
    const auto* occtShape = dynamic_cast<const OcctShape*>(shape.handle());
    if (occtShape == nullptr || occtShape->shape().IsNull())
        return DrawingProjectionResult{
            false, "projection input is not an OcctShape (null or foreign kernel)", {}, {}};

    const Vec3& towards = request.towards;
    const Vec3& up = request.up;
    const double towardsLength =
        std::sqrt(towards.x * towards.x + towards.y * towards.y + towards.z * towards.z);
    const double upLength = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
    if (!(towardsLength > 1.0e-12) || !(upLength > 1.0e-12))
        return DrawingProjectionResult{false, "a view needs a direction and an up", {}, {}};

    // THE CAMERA. OCCT's projector wants the direction it looks ALONG and a
    // frame to flatten into; gp_Ax2's main direction is the one that ends up
    // pointing at the viewer, so the line of sight is REVERSED going in.
    //
    // A `up` that leans into the line of sight is refused rather than
    // straightened: silently fixing it would rotate the view by an amount
    // nobody asked for, and the six standard directions each carry an up that
    // already agrees with them (see CameraFor).
    const gp_Dir sight(-towards.x, -towards.y, -towards.z);
    const gp_Dir upward(up.x, up.y, up.z);
    if (std::fabs(sight.Dot(upward)) > 1.0e-9)
        return DrawingProjectionResult{
            false, "this view's up vector leans into its own line of sight", {}, {}};

    // gp_Ax2's THIRD ARGUMENT IS THE PAGE'S X DIRECTION, not its up.
    //
    // Handing it the up vector puts up along the page's X and turns every view
    // ninety degrees: a 100 x 10 block came back 10 x 100, and a cylinder
    // 30 tall came back 30 wide. It looks like a drawing, which is why the
    // tests measure the extent against the model's own dimensions rather than
    // checking that some curves arrived.
    //
    // Page Y is N x Vx, so for page Y to BE the up vector, Vx = up x N.
    const gp_Dir pageX = upward.Crossed(sight);
    const gp_Ax2 page(gp_Pnt(0.0, 0.0, 0.0), sight, pageX);
    HLRAlgo_Projector projector(page);

    // A 3D POINT ONTO THE PAGE, by the SAME frame the projector uses.
    //
    // The cut faces have to land in the same millimetres as the curves or the
    // hatch will not sit inside the outline it belongs to -- and it would be
    // off by a rotation, which looks like the section being drawn twice.
    const auto projectPoint = [&page](const gp_Pnt& point) {
        const gp_Vec fromOrigin(page.Location(), point);
        return Vec2{fromOrigin.Dot(gp_Vec(page.XDirection())),
                    fromOrigin.Dot(gp_Vec(page.YDirection()))};
    };

    // --- THE CUT (M38) -------------------------------------------------------
    //
    // A half-space is subtracted before anything is projected, so every step
    // after this -- hidden lines, silhouettes, the extent -- works on the cut
    // solid without knowing a cut happened.
    TopoDS_Shape subject = occtShape->shape();
    std::vector<std::vector<Vec2>> cutLoops;
    if (request.section.active) {
        const Vec3& n = request.section.normal;
        const double normalLength = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
        if (!(normalLength > 1.0e-12))
            return DrawingProjectionResult{false, "a section plane needs a normal", {}, {}};
        const gp_Pnt origin(request.section.origin.x, request.section.origin.y,
                            request.section.origin.z);
        const gp_Dir normal(n.x, n.y, n.z);

        // THE HALF-SPACE IS BUILT FROM THE PLANE PLUS A POINT ON THE SIDE TO
        // REMOVE. OCCT's MakeHalfSpace keeps the side the reference point is
        // on, and what we want to CUT AWAY is the side the normal points at --
        // so the reference point goes along the normal.
        //
        // The step is sized from the shape itself rather than a constant: a
        // fixed offset that happened to fall inside a large part would build a
        // half-space through the middle of it.
        Bnd_Box box;
        BRepBndLib::Add(subject, box);
        if (box.IsVoid())
            return DrawingProjectionResult{false, "this shape has no extent to section", {}, {}};
        double xMin = 0.0, yMin = 0.0, zMin = 0.0, xMax = 0.0, yMax = 0.0, zMax = 0.0;
        box.Get(xMin, yMin, zMin, xMax, yMax, zMax);
        const double reach =
            std::max({xMax - xMin, yMax - yMin, zMax - zMin, 1.0}) * 2.0 + 10.0;
        const gp_Pnt awayFromMaterial(origin.X() + normal.X() * reach,
                                      origin.Y() + normal.Y() * reach,
                                      origin.Z() + normal.Z() * reach);

        const gp_Pln plane(origin, normal);
        BRepBuilderAPI_MakeFace faceMaker(plane, -reach, reach, -reach, reach);
        if (!faceMaker.IsDone())
            return DrawingProjectionResult{false, "the section plane could not be built",
                                           {}, {}};
        BRepPrimAPI_MakeHalfSpace halfSpace(faceMaker.Face(), awayFromMaterial);
        if (!halfSpace.IsDone())
            return DrawingProjectionResult{false, "the section half-space could not be built",
                                           {}, {}};

        BRepAlgoAPI_Cut cut(subject, halfSpace.Solid());
        cut.Build();
        if (!cut.IsDone())
            return DrawingProjectionResult{false, "the section cut failed", {}, {}};
        subject = cut.Shape();
        // NOTHING LEFT IS NOT A DRAWING.
        //
        // A boolean that removes everything hands back an empty COMPOUND, not
        // a null shape -- so asking IsNull alone is asking a question that is
        // never true, and a cut whose plane misses the part came back VALID
        // with no curves in it. On the sheet that is a view with nothing in
        // it, which reads as a part still computing rather than as a knife in
        // the wrong place.
        // The face count is BELT AND BRACES and is recorded as such: on this
        // OCCT a cut that removes everything hands back a null shape, so a
        // mutation deleting the second half of this test survives. It stays
        // because the other outcome -- an empty compound reported as a good
        // section -- is a view with nothing in it on a sheet somebody signs,
        // and because which of the two OCCT returns is not this file's to
        // promise.
        if (subject.IsNull() || !TopExp_Explorer(subject, TopAbs_FACE).More())
            return DrawingProjectionResult{
                false, "the section cut removed the whole part -- the plane misses it", {}, {}};

        // --- WHICH FACES ARE THE CUT ------------------------------------------
        //
        // The ones LYING ON THE PLANE. Not "the new ones": a boolean renames
        // everything, and asking which faces are new is asking the question
        // FaceQuery exists because nobody can answer stably. Asking which are
        // ON the plane is a geometric question with a geometric answer.
        for (TopExp_Explorer faces(subject, TopAbs_FACE); faces.More(); faces.Next()) {
            const TopoDS_Face face = TopoDS::Face(faces.Current());
            const Handle(Geom_Surface) surface = BRep_Tool::Surface(face);
            const Handle(Geom_Plane) asPlane = Handle(Geom_Plane)::DownCast(surface);
            if (asPlane.IsNull()) continue;
            const gp_Pln facePlane = asPlane->Pln();
            if (!facePlane.Axis().Direction().IsParallel(normal, 1.0e-7)) continue;
            if (std::fabs(facePlane.Distance(origin)) > 1.0e-7) continue;

            for (TopExp_Explorer wires(face, TopAbs_WIRE); wires.More(); wires.Next()) {
                const TopoDS_Wire wire = TopoDS::Wire(wires.Current());
                std::vector<Vec2> loop;
                // FLATTENED WITH THE SAME DEFLECTION the curves use, so a
                // hatch boundary and the outline drawn over it agree about
                // where the edge is.
                for (BRepTools_WireExplorer along(wire); along.More(); along.Next()) {
                    const TopoDS_Edge edge = along.Current();
                    BRepAdaptor_Curve curve(edge);
                    GCPnts_QuasiUniformDeflection points(curve, 0.05);
                    if (!points.IsDone()) continue;
                    // The LAST point of each edge is the first of the next, so
                    // it is dropped -- a loop that repeated every corner would
                    // give the scanline two crossings at one place and invert
                    // the fill.
                    for (int i = 1; i < points.NbPoints(); ++i)
                        loop.push_back(projectPoint(points.Value(i)));
                }
                if (loop.size() >= 3) cutLoops.push_back(std::move(loop));
            }
        }
    }

    Handle(HLRBRep_Algo) algo = new HLRBRep_Algo();
    algo->Add(subject);
    algo->Projector(projector);
    algo->Update();
    // WHICH EDGES ARE HIDDEN is a separate pass from projecting them, and
    // skipping it is how a "hidden line" drawing comes back with everything
    // visible. Always run: the request's `includeHidden` decides what is
    // COLLECTED, not what is computed, because the visible set is wrong
    // without it too.
    algo->Hide();

    HLRBRep_HLRToShape toShape(algo);
    ProjectedDrawing drawing;

    // SHARP EDGES AND SILHOUETTES ARE DIFFERENT THINGS and a drawing needs
    // both: `VCompound` is the real edges, `OutLineVCompound` is the outline
    // of a curved face, which is not an edge of the solid at all -- it exists
    // only for this direction of sight. A projector that returned only the
    // first would draw a cylinder as two circles and no sides.
    CollectCompound(toShape.VCompound(), ProjectedEdgeKind::Sharp,
                    ProjectedVisibility::Visible, drawing);
    CollectCompound(toShape.OutLineVCompound(), ProjectedEdgeKind::Outline,
                    ProjectedVisibility::Visible, drawing);
    if (request.includeSmooth)
        CollectCompound(toShape.Rg1LineVCompound(), ProjectedEdgeKind::Smooth,
                        ProjectedVisibility::Visible, drawing);
    if (request.includeHidden) {
        CollectCompound(toShape.HCompound(), ProjectedEdgeKind::Sharp,
                        ProjectedVisibility::Hidden, drawing);
        CollectCompound(toShape.OutLineHCompound(), ProjectedEdgeKind::Outline,
                        ProjectedVisibility::Hidden, drawing);
        if (request.includeSmooth)
            CollectCompound(toShape.Rg1LineHCompound(), ProjectedEdgeKind::Smooth,
                            ProjectedVisibility::Hidden, drawing);
    }

    if (drawing.curves.empty())
        return DrawingProjectionResult{false, "this view projected to nothing at all", {}, {}};

    return DrawingProjectionResult{true,
                                   "projected " + std::to_string(drawing.curves.size()) +
                                       " curves",
                                   std::move(drawing), std::move(cutLoops)};
}

} // namespace paramcad
