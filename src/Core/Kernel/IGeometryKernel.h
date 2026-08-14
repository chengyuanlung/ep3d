#pragma once

#include "Core/Kernel/KernelShape.h"
#include "Core/Kernel/KernelTypes.h"
#include "Core/Kernel/ProfileDefinition.h"
#include <string>

namespace paramcad {

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
    virtual ShapeResult revolveProfile(const PlanarProfileDefinition& profile,
                                       const Vec3& axisOriginMm, const Vec3& axisDirection,
                                       double angleRad) = 0;
};

} // namespace paramcad
