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
