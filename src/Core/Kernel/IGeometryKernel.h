#pragma once

#include "Core/Kernel/EdgeQuery.h"
#include "Core/Kernel/FaceQuery.h"

#include <cstdint>
#include "Core/Geometry/MathTypes.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Kernel/KernelTypes.h"
#include "Core/Kernel/ProfileDefinition.h"
#include <string>
#include <vector>

namespace paramcad {

// Whether a file was written, and why not (M22).
//
// Its own type rather than ShapeResult, because writing produces no shape and
// a caller that got one back would have to ignore it -- and a result nobody
// reads is a result nobody checks.
struct IoResult {
    bool ok{false};
    std::string message;
    explicit operator bool() const noexcept { return ok; }
};

// Where a shape reaches, and whether the question could be answered at all.
//
// `ok` is false for an empty or foreign handle: an empty shape has no extent,
// and returning a box at the origin would be indistinguishable from geometry
// that really is there.
struct KernelBoundsResult {
    bool ok{false};
    std::string message;
    Vec3 min{};
    Vec3 max{};
};

struct ShapeResult {
    KernelShape shape;
    KernelError error = KernelError::None;
    std::string message;
    explicit operator bool() const noexcept { return error == KernelError::None; }
};

// Kernel-neutral geometry service boundary (ADR-M3-001/003). This interface
// lives inside src/Core (zero OCCT anywhere in this header or its includes),
// which is what makes it legal to reference from RecomputeContext without
// violating CodingRule 2 ("Core headers may include only standard-library and
// Core headers"). Only translation units under src/Kernel/Occt ever implement
// this with real OCCT calls; BoxFeature and every other Core consumer sees
// only this interface, injected through RecomputeContext::kernel (ADR-M3-003)
// -- Core never names a concrete kernel implementation.
class IGeometryKernel {
public:
    virtual ~IGeometryKernel() = default;

    // Expected invalid input (spec 13: zero/negative/NaN/infinite dimension)
    // returns a controlled ShapeResult with KernelError::InvalidDimension and
    // a diagnostic message -- never throws, never UB.
    virtual ShapeResult createBox(const BoxDefinition& definition) = 0;

    // Builds a planar face from an already-validated, ordered, oriented profile
    // and extrudes it distanceMm along the profile plane's normal (M4,
    // ADR-M4-003).
    //
    // One call rather than createPlanarFace + extrude(face): a KernelFace would
    // be a second runtime handle type with its own validity and staleness
    // story, and M4 has no consumer for a bare face. M3's costliest defects
    // were all a second copy of runtime state disagreeing with the first
    // (ADR-M3-006/007). The face stays internal to the implementation; if a
    // later milestone needs one (UpToFace, shells), the split can be added when
    // there is a caller to define its semantics.
    //
    // Invalid input -- empty segment list, non-finite or non-positive distance,
    // a degenerate plane -- returns a controlled ShapeResult with
    // KernelError::InvalidDimension and a diagnostic. Never throws.
    virtual ShapeResult extrudeProfile(const PlanarProfileDefinition& profile,
                                       double distanceMm) = 0;

    // shape must be a valid KernelShape previously returned by this same
    // kernel; an invalid/foreign shape returns a controlled
    // KernelMassPropertiesResult with KernelError::GeometryConstructionFailed
    // rather than dereferencing anything unsafely.
    virtual KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) = 0;

    // base minus tool, as a NEW shape (M8, spec 5). Neither input is modified
    // or invalidated: the feature chain reads its base through ISolidFeature
    // and the base feature goes on owning its own result, so a boolean that
    // mutated its operands would corrupt the very state selective recompute
    // relies on being stable.
    //
    // A tool that misses the base entirely, or swallows it entirely, is a
    // LEGAL cut (M8 spec 6): the result is the base unchanged, or an empty
    // shape, respectively. Refusing either would make ordinary modelling fail
    // -- a pocket dragged off the part is a modelling state, not an error.
    //
    // Invalid or foreign handles return a controlled ShapeResult with
    // KernelError::GeometryConstructionFailed. Never throws.
    virtual ShapeResult subtractShape(const KernelShape& base, const KernelShape& tool) = 0;

    // Revolves the profile about an axis lying in (or parallel to) its plane,
    // counter-clockwise about axisDirection by angleRad (M8.2, ADR-M8-005).
    // The axis arrives as WORLD origin+direction because Core has already done
    // the (u,v)->world conversion through SketchFrame -- the kernel never
    // re-derives a frame (ADR-M4-002's rule, unchanged).
    //
    // angleRad in (0, 2*pi]; exactly 2*pi is a full solid of revolution.
    // Invalid input -- bad profile, degenerate axis direction, out-of-range
    // angle -- returns a controlled InvalidDimension. Never throws.
    // SWEEPS the profile along a path (M19).
    //
    // The profile and the path arrive on their OWN planes, each already
    // converted to world by Core through its sketch's frame -- the kernel never
    // re-derives a frame (ADR-M4-002). They are usually different planes, and
    // for a sweep to be anything other than an extrusion they had better be.
    //
    // The profile is NOT required to sit on the path, or to touch it. What is
    // swept is the profile's shape; where it starts is where the profile is.
    // Requiring contact would refuse the ordinary case of drawing the section
    // on one datum and the spine on another.
    //
    // Invalid input -- an empty profile, an empty path, a degenerate plane --
    // returns a controlled InvalidDimension. A path OCCT cannot sweep along
    // (one that doubles back inside the profile's own width, say) returns
    // GeometryConstructionFailed with OCCT's own complaint. Never throws.
    virtual ShapeResult sweepProfile(const PlanarProfileDefinition& profile,
                                     const PlanarPathDefinition& path) = 0;

    // LOFTS through two or more profiles, in the order given (M19).
    //
    // Order is the caller's, and it is load-bearing: lofting A-B-C and A-C-B
    // are different solids, and sorting them by anything the kernel could see
    // -- distance from the origin, plane height -- would be the kernel deciding
    // what the user meant.
    //
    // Each profile must be closed and valid on its own; a loft through fewer
    // than two is an InvalidDimension, not an extrusion by another name.
    virtual ShapeResult loftProfiles(const std::vector<PlanarProfileDefinition>& profiles) = 0;

    virtual ShapeResult revolveProfile(const PlanarProfileDefinition& profile,
                                       const Vec3& axisOriginMm, const Vec3& axisDirection,
                                       double angleRad) = 0;

    // Fillets (rounds) or chamfers (bevels) EVERY edge of the shape by one
    // radius/distance (M8.3, ADR-M8-006).
    //
    // ALL edges, deliberately. Selective edge treatment needs an edge the
    // document can NAME, and today the only available name is an OCCT explorer
    // position -- transient topology, the exact thing ADR-M4-004 forbids as
    // identity. Rather than smuggle that in through a parameter, per-edge
    // selection waits for the selection architecture (roadmap section 13) and
    // a semantic edge-naming scheme; ADR-M8-006 records the deferral.
    //
    // A radius/distance the geometry cannot accommodate (larger than half the
    // smallest adjacent dimension, degenerate results) surfaces as a
    // controlled GeometryConstructionFailed from OCCT's own algorithms --
    // there is no cheap a-priori bound worth half-checking. Neither call
    // modifies its input shape. Never throws.
    // `selection` is a QUERY, answered against this shape on every call
    // (M17.12, ADR-M17-034). It replaced filletAllEdges/chamferAllEdges rather
    // than joining them: `AllEdgesSelection()` says the same thing, and two
    // ways to ask for one behaviour is how two behaviours eventually appear.
    //
    // A selection that matches NO edge is refused with a diagnostic, never
    // treated as "nothing to do". A fillet that dresses no edge returns the
    // solid unchanged and reports success -- a command that changed nothing
    // while saying it worked, which this project has already had to fix twice
    // in other clothes.
    // --- Provenance (M17.13, ADR-M17-035) -----------------------------------
    //
    // Records WHO made which face, so a `CreatedBy` query can be answered on
    // any later shape in the chain. Called by a feature straight after it
    // builds, with what it consumed, what it produced, and an opaque TAG --
    // Core puts a feature id in it; a kernel only has to hand the same number
    // back.
    //
    // DEFAULTED to "no provenance", not pure, because a kernel that does not
    // track history is a perfectly good kernel: the fake models volumes and has
    // no topology to attribute. Making this pure would have forced an empty
    // override into every fake for a fact none of them can produce, and a
    // pile of empty overrides is where a real one eventually gets missed.
    // The ONE face a query names on this shape (M17.14, ADR-M17-036).
    //
    // DEFAULTED to a refusal, not to a guess: a kernel that cannot answer must
    // say so, because the caller's next move is to place a sketch on the
    // answer. Returning an identity plane would put it at the world origin,
    // which is a perfectly plausible-looking wrong place.
    virtual FaceQueryResult resolveFace(const KernelShape& shape, const FaceQuery& query) {
        (void)shape;
        (void)query;
        return FaceQueryResult{false, "this kernel cannot find faces", {}};
    }

    virtual KernelShape tagCreatedFaces(const KernelShape& result, const KernelShape& base,
                                        std::uint64_t tag) {
        (void)base;
        (void)tag;
        return result;
    }

    // HOLLOWS the solid, leaving `thicknessMm` of wall and opening the faces
    // the selection names (M20).
    //
    // The faces are QUERIES, re-answered against this shape, exactly as a
    // fillet's edges are: a shell that remembered "face 3" would open whatever
    // is now third after the part changed.
    //
    // Thickness is INWARD and positive. An outward shell is a different
    // operation -- it grows the part -- and giving it to the same call under a
    // sign would make "shell 2 mm" mean two different solids depending on a
    // character.
    //
    // An EMPTY selection is refused: a shell with no opening is a hollow with
    // no way in, and OCCT will happily build one that looks solid from every
    // side and weighs less than it should.
    virtual ShapeResult shellSolid(const KernelShape& base, const FaceSelection& openFaces,
                                   double thicknessMm) = 0;

    // TAPERS the faces the selection names, by `angleRad`, about `neutral` (M20).
    //
    // The neutral face is what the taper pivots on: it keeps its size, and
    // everything above or below it moves in or out. It is a query too, because
    // it is a face of this same solid -- usually the one the part sits on.
    //
    // The angle's SIGN chooses which way the taper leans, and there is no
    // defensible default for it: a mould that has to release upwards and one
    // that has to release downwards are the same magnitude and opposite
    // intents.
    virtual ShapeResult draftFaces(const KernelShape& base, const FaceSelection& faces,
                                   const FaceQuery& neutral, double angleRad) = 0;

    // --- The outside world (M22) ------------------------------------------
    //
    // Reading and writing files is a KERNEL capability, not a document one: the
    // kernel owns the representation, and a different kernel would write STEP
    // from a different one. What crosses this boundary is still only a
    // KernelShape and a path -- no topology, no OCCT type (ADR-M4-004).
    //
    // PATHS, not streams. STEP writing is a multi-pass operation in OCCT and
    // the reader is happier with a file than with anything else; wrapping that
    // in a stream would be an abstraction over one implementation's convenience
    // rather than over a real choice.

    // Writes `shape` as AP214 STEP. The one exchange format every mechanical
    // CAD system reads, which is the whole reason it comes before STL.
    virtual IoResult exportStep(const KernelShape& shape, const std::string& path) = 0;

    // Reads a STEP file back as ONE solid.
    //
    // A file holding several solids is REFUSED rather than silently reduced to
    // the first or fused into one: "which of these did you mean" has no
    // defensible default, and a fuse would invent material between parts that
    // were deliberately apart. Multi-solid import is a real feature and it is
    // not this one.
    virtual ShapeResult importStep(const std::string& path) = 0;

    // Writes `shape` as binary STL, tessellated to `deflectionMm`.
    //
    // LOSSY BY CONSTRUCTION, and the deflection says how lossy: STL is
    // triangles, so a cylinder becomes a fan of flats and there is no setting
    // that makes it not so. It is here because it is what a 3D printer eats.
    virtual IoResult exportStl(const KernelShape& shape, const std::string& path,
                               double deflectionMm) = 0;

    // The axis-aligned extent of a shape, in part-local XYZ (M20).
    //
    // Kernel-neutral by construction -- three numbers each way, no topology --
    // and it exists because "through all" is a question about how far the
    // material reaches. A hole that guessed a very deep cylinder instead would
    // work until somebody built a part deeper than the guess, and then it
    // would stop part-way with nothing to say.
    virtual KernelBoundsResult boundsOfShape(const KernelShape& shape) = 0;

    virtual ShapeResult filletEdges(const KernelShape& shape, const EdgeSelection& selection,
                                    double radiusMm) = 0;
    virtual ShapeResult chamferEdges(const KernelShape& shape, const EdgeSelection& selection,
                                     double distanceMm) = 0;

    // --- M10.6: the two verbs Mirror and Pattern are made of -----------------
    //
    // ADR-M9-006 deferred Mirror and Pattern to M10 because both are a
    // TRANSFORM plus a BOOLEAN, and a transform needs a stable plane or axis to
    // be defined against -- which is what a ReferenceFrame now is. These are
    // that transform and that boolean.
    //
    // `mirrorShape` reflects across the plane through `planeOriginMm` with
    // normal `planeNormal`. A reflection is NOT expressible as a Transform3D:
    // a unit quaternion is a rotation, and rotations preserve handedness while
    // a mirror reverses it. So the plane is passed as origin + normal rather
    // than as a transform, which also matches how a frame describes a plane
    // (its origin, and its local +Z as the normal -- the same convention
    // SketchFrame uses).
    //
    // `translateShape` is the pattern verb: a pure offset, which IS a
    // Transform3D case, kept narrow because that is all a linear pattern needs.
    //
    // `fuseShapes` unions two solids. Disjoint inputs give a compound whose
    // volume is the sum -- a legal result, not an error, exactly as a disjoint
    // cut is in `subtractShape`.
    //
    // A degenerate normal (zero length, non-finite) is refused. None of these
    // modifies its inputs. None throws.
    virtual ShapeResult mirrorShape(const KernelShape& shape, const Vec3& planeOriginMm,
                                    const Vec3& planeNormal) = 0;
    virtual ShapeResult translateShape(const KernelShape& shape, const Vec3& offsetMm) = 0;

    // `rotateShape` is the CIRCULAR pattern's verb (M21): a pure rotation about
    // a world axis, which is a rigid motion exactly as a translation is -- the
    // copy is the same solid somewhere else, not a new one built from scratch.
    //
    // The axis arrives as world origin+direction because Core has already done
    // any frame conversion; the kernel never re-derives a frame (ADR-M4-002).
    virtual ShapeResult rotateShape(const KernelShape& shape, const Vec3& axisOriginMm,
                                    const Vec3& axisDirection, double angleRad) = 0;

    // `placeShape` is the ASSEMBLY's verb (M23): a whole rigid placement in
    // one call -- rotate about the origin, then translate.
    //
    // It exists rather than letting the caller compose rotateShape and
    // translateShape because that composition has an ORDER, and a caller that
    // gets it backwards produces a part in the wrong place by an amount that
    // looks like a modelling mistake. One verb, one order, stated once.
    //
    // The transform arrives as a Transform3D because that is what a frame
    // already is; the kernel does not re-derive a frame (ADR-M4-002), it just
    // applies the rigid motion it is handed. A non-finite or non-unit rotation
    // is refused.
    virtual ShapeResult placeShape(const KernelShape& shape, const Transform3D& placement) = 0;

    // `intersectShapes` keeps ONLY what both solids occupy (M21).
    //
    // The third boolean, and the one whose empty answer is meaningful: two
    // solids that do not overlap intersect to nothing, and that is a real
    // result rather than a failure. It is still REFUSED here, because a feature
    // whose output is an empty solid is one the user did not mean to make --
    // and a chain carrying an empty shape forward looks exactly like a chain
    // that worked.
    virtual ShapeResult intersectShapes(const KernelShape& a, const KernelShape& b) = 0;
    virtual ShapeResult fuseShapes(const KernelShape& a, const KernelShape& b) = 0;
};

} // namespace paramcad
