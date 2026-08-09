#pragma once

#include "Core/Geometry/MathTypes.h"

namespace paramcad {

// Derived, never-persisted document-level physical properties (ADR-009 D6,
// ADR-M3-002). Units: volumeMm3 in mm^3, massKg in kg, centerOfMassMm in mm,
// inertiaTensorKgM2 in kg*m^2 (about the center of mass, local XYZ axes).
struct MassProperties {
    double volumeMm3{0.0};
    double massKg{0.0};
    Vec3 centerOfMassMm{};
    Matrix3 inertiaTensorKgM2{};
    bool valid{false};
};

} // namespace paramcad
