#include "Kernel/Occt/OcctFaceQuery.h"
#include "Kernel/Occt/OcctFaceQueryTopology.h"

#include "Kernel/Occt/OcctShape.h"

#include <cmath>
#include <vector>
#include <string>
#include <cstdint>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Circ.hxx>
#include <gp_Lin.hxx>
#include <TopExp_Explorer.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

namespace paramcad {

namespace {

double Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

Vec3 ToVec3(const gp_Pnt& p) noexcept { return Vec3{p.X(), p.Y(), p.Z()}; }

Vec3 ToVec3(const gp_Dir& d) noexcept { return Vec3{d.X(), d.Y(), d.Z()}; }

// One edge, read as far as EP3D's sketch entities can hold it.
//
// The three supported kinds map one-to-one onto SketchLine, SketchCircle and
// SketchArc. Everything else -- splines, ellipses, the trimmed remains of a
// surface intersection -- comes back Unsupported ON PURPOSE: the projection
// then reports how many edges it could not bring across, instead of handing
// back a boundary with silent gaps in it.
FaceCurve CurveOfEdge(const TopoDS_Edge& edge) {
    FaceCurve curve;
    BRepAdaptor_Curve adaptor(edge);

    if (adaptor.GetType() == GeomAbs_Line) {
        curve.kind = FaceCurve::Kind::Line;
        curve.start = ToVec3(adaptor.Value(adaptor.FirstParameter()));
        curve.end = ToVec3(adaptor.Value(adaptor.LastParameter()));
        return curve;
    }

    if (adaptor.GetType() == GeomAbs_Circle) {
        const gp_Circ circle = adaptor.Circle();
        curve.center = ToVec3(circle.Location());
        curve.radiusMm = circle.Radius();
        curve.axis = ToVec3(circle.Axis().Direction());
        curve.start = ToVec3(adaptor.Value(adaptor.FirstParameter()));
        curve.end = ToVec3(adaptor.Value(adaptor.LastParameter()));
        // A FULL circle and an arc are the same curve type in OCCT and
        // different entities here, so the parameter range is what tells them
        // apart. A hole in a face is a full circle; the fillet on a corner is
        // an arc, and drawing one as the other would either close a boundary
        // that is open or open one that is closed.
        const double sweep = adaptor.LastParameter() - adaptor.FirstParameter();
        constexpr double kTwoPi = 6.28318530717958647692;
        curve.kind = std::fabs(std::fabs(sweep) - kTwoPi) < 1e-9 ? FaceCurve::Kind::Circle
                                                                 : FaceCurve::Kind::Arc;
        return curve;
    }

    return curve; // Unsupported, and honest about it
}

FaceBoundary BoundaryOf(const TopoDS_Face& face) {
    FaceBoundary boundary;
    for (TopExp_Explorer it(face, TopAbs_EDGE); it.More(); it.Next())
        boundary.push_back(CurveOfEdge(TopoDS::Edge(it.Current())));
    return boundary;
}

} // namespace

FacePlane PlaneOfFace(const TopoDS_Shape& shape) {
    FacePlane result;
    if (shape.IsNull() || shape.ShapeType() != TopAbs_FACE) return result;
    result.isFace = true;

    const TopoDS_Face face = TopoDS::Face(shape);
    // Restricted to the surface, not the face's parametric bounds: the plane
    // is what is wanted, and a trimmed adaptor would refuse nothing extra.
    BRepAdaptor_Surface surface(face, Standard_False);
    if (surface.GetType() != GeomAbs_Plane) return result; // a face, but curved
    result.planar = true;

    const gp_Pln plane = surface.Plane();
    const gp_Pnt location = plane.Location();
    gp_Dir direction = plane.Axis().Direction();
    if (face.Orientation() == TopAbs_REVERSED) direction.Reverse();

    result.point = Vec3{location.X(), location.Y(), location.Z()};
    result.normal = Vec3{direction.X(), direction.Y(), direction.Z()};
    result.boundary = BoundaryOf(face);
    return result;
}

std::vector<FacePlane> FacesOf(const KernelShape& shape) {
    std::vector<FacePlane> faces;
    const auto* occt = dynamic_cast<const OcctShape*>(shape.handle());
    if (occt == nullptr || occt->shape().IsNull()) return faces; // foreign or empty
    for (TopExp_Explorer it(occt->shape(), TopAbs_FACE); it.More(); it.Next()) {
        FacePlane plane = PlaneOfFace(it.Current());
        // The owning shape is in hand here, so the provenance can be read --
        // which is the difference between this door and PlaneOfFace on its own,
        // where a bare face has no owner to ask.
        for (const auto& entry : occt->provenance())
            if (entry.second.Contains(it.Current())) plane.createdBy = entry.first;
        faces.push_back(std::move(plane));
    }
    return faces;
}

// --- Resolving a face query against real topology (M17.14, ADR-M17-036) -----

// WHICH ONE, as a position in the face list -- or -1 with the reason.
//
// The narrowing lives here and nowhere else. `ResolveFaceQuery` wants the
// face's GEOMETRY and `FaceForQuery` wants its TOPOLOGY, and those are two
// questions about the same answer: a second copy of "furthest along, and
// facing that way, and ties are ambiguous" would be two places to disagree
// about which face a sentence names.
//
// The index is into the order TopExp_Explorer visits faces in, which is also
// the order FacesOf builds them in. That is an index used as identity, and it
// is safe for exactly one reason: it never leaves this call. Nothing stores
// it, nothing writes it to a file, and the shape cannot change while it is in
// hand.
int ChooseFace(const std::vector<FacePlane>& faces, const FaceQuery& query,
               std::string& message) {
    if (query.empty()) {
        // Never "the first face we found". A query that narrows to nothing has
        // no answer, and inventing one puts a sketch on a face nobody chose.
        message = "no face was named";
        return -1;
    }

    constexpr double kFacing = 0.9;   // the cone EdgesOfExtremeFace uses
    constexpr double kSameMm = 1e-6;

    // Narrow by every condition that is set. The order does not matter -- this
    // is a conjunction, and each pass only removes.
    std::vector<int> candidates;
    for (int i = 0; i < static_cast<int>(faces.size()); ++i) {
        const FacePlane& face = faces[static_cast<std::size_t>(i)];
        if (!face.planar) continue; // a sketch and an edge set both need a plane
        if (query.createdBy.has_value() &&
            face.createdBy != static_cast<std::uint64_t>(*query.createdBy))
            continue;
        if (query.facing.has_value() && Dot(face.normal, *query.facing) < kFacing) continue;
        candidates.push_back(i);
    }

    if (candidates.empty()) {
        message = "no face on the solid matches " + DescribeFaceQuery(query);
        return -1;
    }

    if (query.extremeTowards.has_value()) {
        // Furthest along the direction AND facing it -- both, for the reason
        // ExtremeFace already documents: "highest point" alone picks a side
        // wall whose top corner is the highest thing around.
        const Vec3 towards = *query.extremeTowards;
        int best = -1;
        double furthest = 0.0;
        for (const int index : candidates) {
            const FacePlane& face = faces[static_cast<std::size_t>(index)];
            if (Dot(face.normal, towards) < kFacing) continue;
            const double offset = Dot(face.point, towards);
            if (best >= 0 && offset <= furthest) continue;
            best = index;
            furthest = offset;
        }
        if (best < 0) {
            message = "no face faces " + DescribeFaceQuery(query);
            return -1;
        }
        // Ties are AMBIGUOUS, not resolved by picking one. Two faces at the
        // same offset facing the same way is a symmetric part, and choosing
        // between them arbitrarily would put the answer somewhere the user
        // could not predict and could not correct.
        int atFurthest = 0;
        for (const int index : candidates) {
            const FacePlane& face = faces[static_cast<std::size_t>(index)];
            if (Dot(face.normal, towards) >= kFacing &&
                Dot(face.point, towards) > furthest - kSameMm)
                ++atFurthest;
        }
        if (atFurthest > 1) {
            message = DescribeFaceQuery(query) + " matches " + std::to_string(atFurthest) +
                      " faces at the same distance";
            return -1;
        }
        message = DescribeFaceQuery(query);
        return best;
    }

    if (candidates.size() > 1) {
        // Narrowed, but not to one. Said with the COUNT, because the fix is to
        // add a condition and the user needs to know how far off they are.
        message = DescribeFaceQuery(query) + " matches " + std::to_string(candidates.size()) +
                  " faces; it needs narrowing";
        return -1;
    }

    message = DescribeFaceQuery(query);
    return candidates.front();
}

FaceQueryResult ResolveFaceQuery(const KernelShape& shape, const FaceQuery& query) {
    FaceQueryResult result;
    const auto* occt = dynamic_cast<const OcctShape*>(shape.handle());
    if (occt == nullptr || occt->shape().IsNull()) {
        result.message = "there is no solid to find that face on";
        return result;
    }

    const std::vector<FacePlane> faces = FacesOf(shape);
    const int chosen = ChooseFace(faces, query, result.message);
    if (chosen < 0) return result;
    result.ok = true;
    result.face = faces[static_cast<std::size_t>(chosen)];
    return result;
}

TopoDS_Face FaceForQuery(const KernelShape& shape, const FaceQuery& query, std::string& why) {
    const auto* occt = dynamic_cast<const OcctShape*>(shape.handle());
    if (occt == nullptr || occt->shape().IsNull()) {
        why = "there is no solid to find that face on";
        return TopoDS_Face();
    }

    const int chosen = ChooseFace(FacesOf(shape), query, why);
    if (chosen < 0) return TopoDS_Face();

    // The SAME walk FacesOf made, stopped at the index it chose.
    int at = 0;
    for (TopExp_Explorer it(occt->shape(), TopAbs_FACE); it.More(); it.Next(), ++at)
        if (at == chosen) return TopoDS::Face(it.Current());
    why = "that face went missing between being chosen and being fetched";
    return TopoDS_Face();
}

} // namespace paramcad