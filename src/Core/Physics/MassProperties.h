#pragma once

#include "Core/Geometry/MathTypes.h"

namespace paramcad {

struct MassProperties {
    double volumeMm3{0.0};
    double massKg{0.0};
    Vec3 centerOfMassMm{};
    Matrix3 inertiaTensorKgMm2{};
    bool valid{false};
};

} // namespace paramcad
