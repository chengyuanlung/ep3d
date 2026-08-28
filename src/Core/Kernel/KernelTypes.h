#pragma once

#include <cmath>
#include <string>
#include "Core/Geometry/MathTypes.h"

namespace paramcad {

// Kernel-neutral box definition (ADR-M3-001/002). Lengths in millimeters.
struct BoxDefinition {
    double widthMm{0.0};
    double heightMm{0.0};
    double depthMm{0.0};
};

// A ROUND WIRE WOUND INTO A HELIX (M60).
//
// A definition struct rather than six loose arguments, for BoxDefinition's
// reason: the validity rule then has one place to live and every kernel, real
// or fake, asks it first (ADR-M3-001).
//
// WHY THE KERNEL NEEDS THIS AT ALL. sweepProfile takes a PLANAR path, and a
// helix is the one shape in mechanical engineering that is nothing but a
// non-planar curve. M39 refused to model a thread as a helix and was right to
// -- a thread has a blank to draw instead, and the drawing says M8x1.25 either
// way. A SPRING HAS NOTHING ELSE IT IS.
struct HelixDefinition {
    double wireRadiusMm{0.0};  // half the wire's thickness
    double helixRadiusMm{0.0}; // axis to the WIRE'S CENTRE: the mean radius
    double pitchMm{0.0};       // rise per full turn
    double turns{0.0};         // may be fractional
    double startAngleRad{0.0}; // where on the circle the wire starts
    double startHeightMm{0.0}; // how far up the axis it starts
};

enum class KernelError {
    None,
    InvalidDimension,
    NonFinite,
    GeometryConstructionFailed,
    MassPropertiesFailed
};

// Smallest box dimension this kernel boundary accepts, in mm. Strictly
// positive is not a sufficient test in practice: OCCT rejects degenerate
// primitives below its own confusion tolerance and signals that by throwing,
// which would reach callers as a bare "OCCT raised ..." string instead of the
// structured InvalidDimension the spec-13 contract promises for a bad
// dimension. 1e-6 mm (one nanometre) sits well above OCCT's threshold and far
// below any real CAD feature, so it rejects only inputs that were already
// unusable.
inline constexpr double kMinBoxDimensionMm = 1e-6;

// The ONE place numeric dimension validation lives (ADR-M3-001): every
// IGeometryKernel implementation, real or fake, calls this first, so the
// check exists exactly once. A valid box requires every dimension to be
// finite and at least kMinBoxDimensionMm (zero, negative, NaN, infinity, and
// degenerate-but-positive values are all invalid -- spec 13).
// THE COIL HAS TO CLEAR ITSELF. A pitch smaller than the wire is thickness
// wound onto thickness: OCCT builds a self-intersecting pipe, and what comes
// back is a solid-looking thing with the wrong volume rather than a failure.
// Refused here so that no kernel has to notice.
inline bool IsValidHelixDefinition(const HelixDefinition& definition) noexcept {
    if (!(definition.wireRadiusMm > 0.0) || !(definition.helixRadiusMm > 0.0)) return false;
    if (!(definition.turns > 0.0)) return false;
    if (!(definition.pitchMm >= 2.0 * definition.wireRadiusMm)) return false;
    // ...and it has to clear the AXIS as well, or the inside of the coil folds
    // through the middle.
    if (!(definition.helixRadiusMm > definition.wireRadiusMm)) return false;
    return std::isfinite(definition.wireRadiusMm) && std::isfinite(definition.helixRadiusMm) &&
           std::isfinite(definition.pitchMm) && std::isfinite(definition.turns) &&
           std::isfinite(definition.startAngleRad) && std::isfinite(definition.startHeightMm);
}

inline bool IsValidBoxDefinition(const BoxDefinition& definition) noexcept {
    for (double dimension : {definition.widthMm, definition.heightMm, definition.depthMm}) {
        if (!std::isfinite(dimension)) return false;
        if (dimension < kMinBoxDimensionMm) return false;
    }
    return true;
}

// Geometric (density-independent) mass properties of a shape, as computed by
// the kernel. Units (ADR-M3-002):
//   volumeMm3        -- mm^3
//   centerOfMassMm   -- mm, exposed to CAD unconverted
//   secondMomentMm5  -- second moment of volume about the COM, integral(r^2 dV)
//                        per axis pair, in mm^5. Density-independent; the
//                        single conversion to a physical inertia tensor
//                        (kg*m^2) happens only in MassPropertiesNode.
struct KernelMassProperties {
    double volumeMm3{0.0};
    Vec3 centerOfMassMm{};
    Matrix3 secondMomentMm5{};
};

struct KernelMassPropertiesResult {
    KernelMassProperties properties;
    KernelError error = KernelError::None;
    std::string message;
    explicit operator bool() const noexcept { return error == KernelError::None; }
};

} // namespace paramcad
