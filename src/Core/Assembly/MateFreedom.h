#pragma once

#include "Core/Geometry/MathTypes.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace paramcad {

// WHAT EVERY MATE TYPE IS, AS ONE THING (M25, ADR-M25-001).
//
// M24 had three mate types and gave each its own middle transform. That worked
// and would not have kept working: cylindrical leaves TWO freedoms, planar
// leaves three, and a per-type formula means a per-type idea of what
// "connected" means -- which is the seam this project spends its milestones
// removing.
//
// So a mate type is now exactly one thing: WHICH OF THE SIX COMPONENTS of the
// relative transform between its two connectors it leaves free. Everything
// else falls out of that:
//
//   * the middle transform is the free components driven to their values;
//   * the RESIDUALS a closed-loop solve needs are the PINNED components, which
//     must be zero;
//   * the degrees of freedom the mate leaves are the free components counted.
//
// Roadmap §20.1's table is reproduced by FreedomOf below, entry for entry, and
// that is not a coincidence -- it is where the table came from.
enum class MateComponent {
    TX = 0, // translation along the connectors' shared X
    TY,
    TZ,
    RX, // rotation about the connectors' shared X
    RY,
    RZ,
};

inline constexpr std::size_t kMateComponentCount = 6;

std::string_view toString(MateComponent component) noexcept;

// A value per component. Only the FREE ones may be non-zero: a number on a
// pinned component would be an offset, which is a real feature (§20.2) and is
// not this one -- accepting it here would silently do nothing.
using MateValues = std::array<double, kMateComponentCount>;

struct MateFreedom {
    std::array<bool, kMateComponentCount> free{};

    bool isFree(MateComponent component) const noexcept {
        return free[static_cast<std::size_t>(component)];
    }
    int translational() const noexcept {
        return static_cast<int>(free[0]) + static_cast<int>(free[1]) + static_cast<int>(free[2]);
    }
    int rotational() const noexcept {
        return static_cast<int>(free[3]) + static_cast<int>(free[4]) + static_cast<int>(free[5]);
    }
    int total() const noexcept { return translational() + rotational(); }
};

// THE SIX NUMBERS THAT SAY WHERE A TRANSFORM IS, relative to identity.
//
// Translation, then the rotation as an AXIS-ANGLE VECTOR -- the axis scaled by
// the angle. Not Euler angles: those have a gimbal direction where two of the
// three stop being independent, and a solver whose residual quietly loses a
// dimension in some configurations is a solver that converges to the wrong
// answer there and nowhere else.
//
// All six are zero exactly when the transform is identity, which is what makes
// them usable as residuals.
std::array<double, kMateComponentCount> ComponentsOf(const Transform3D& transform) noexcept;

// The transform a set of values describes: rotate first, then translate.
//
// ONE ORDER, decided once -- the same order placeShape uses (ADR-M23-004) --
// because a caller that composes them the other way puts a part somewhere that
// looks like a modelling mistake.
Transform3D TransformOfComponents(const std::array<double, kMateComponentCount>& values) noexcept;

} // namespace paramcad
