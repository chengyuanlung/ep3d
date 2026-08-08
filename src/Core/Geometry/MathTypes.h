#pragma once

#include <array>
#include <cmath>

namespace paramcad {

struct Vec2 {
    double x{0.0};
    double y{0.0};
};

struct Vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

struct Matrix3 {
    std::array<double, 9> m{
        1.0, 0.0, 0.0,
        0.0, 1.0, 0.0,
        0.0, 0.0, 1.0
    };
};

struct Quaternion {
    double w{1.0};
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

struct Transform3D {
    Vec3 translation{};
    Quaternion rotation{};

    static Transform3D Identity() { return {}; }
};

} // namespace paramcad
