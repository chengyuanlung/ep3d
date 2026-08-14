#pragma once

// Test-only IGeometryKernel implementation (spec 16 pattern, applied to the
// M3/M4 kernel boundary). Computes geometry analytically with the SAME raw
// formulas the oracle tests use independently -- zero OCCT dependency, so every
// Core-side test (BoxFeature, PadFeature, MassPropertiesNode, serialization
// round-trip/recompute-after-load) can run and pass without linking
// Kernel/Occt. Real geometric correctness against OCCT is exercised separately
// in tests/Kernel/.
//
// SCOPE OF extrudeProfile, stated rather than implied: this fake models exactly
// two cross-sections -- an axis-aligned rectangle of four line segments, and a
// full circle. Anything else (arcs, general polygons) returns a structured
// failure saying so. That is deliberate: a half-correct analytical model would
// make Core-only tests agree with a formula rather than with geometry, and the
// shapes a fake cannot model are precisely the ones that belong in the real
// OCCT suite. The two supported cross-sections are the ones mandatory release
// gates A and B are built on.

#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Kernel/KernelTypes.h"
#include "Core/Kernel/ProfileDefinition.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace paramcad {

// Opaque handle carrying the mass properties the fake computed when the shape
// was created; only FakeGeometryKernel ever dynamic_casts to this type (mirrors
// how OcctShape is only ever inspected by OcctGeometryKernel).
class FakeShapeHandle final : public IShapeHandle {
public:
    explicit FakeShapeHandle(KernelMassProperties properties) noexcept
        : properties_(properties) {}
    const KernelMassProperties& properties() const noexcept { return properties_; }

private:
    KernelMassProperties properties_;
};

class FakeGeometryKernel final : public IGeometryKernel {
public:
    ShapeResult createBox(const BoxDefinition& definition) override {
        if (!IsValidBoxDefinition(definition))
            return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                               "invalid box definition: every dimension must be finite and at "
                               "least " +
                                   std::to_string(kMinBoxDimensionMm) + " mm"};
        ++createBoxCallCount;

        const double w = definition.widthMm;
        const double h = definition.heightMm;
        const double d = definition.depthMm;

        KernelMassProperties properties;
        properties.volumeMm3 = w * h * d;
        properties.centerOfMassMm = Vec3{w / 2.0, h / 2.0, d / 2.0};
        // Second moment of volume about COM for an axis-aligned rectangular
        // solid, mm^5 (same shape as the physical inertia formula, without
        // density -- ADR-M3-002). Off-diagonal terms are exactly zero.
        const double v = properties.volumeMm3;
        properties.secondMomentMm5.m[0] = v / 12.0 * (h * h + d * d);
        properties.secondMomentMm5.m[4] = v / 12.0 * (w * w + d * d);
        properties.secondMomentMm5.m[8] = v / 12.0 * (w * w + h * h);
        properties.secondMomentMm5.m[1] = properties.secondMomentMm5.m[3] = 0.0;
        properties.secondMomentMm5.m[2] = properties.secondMomentMm5.m[6] = 0.0;
        properties.secondMomentMm5.m[5] = properties.secondMomentMm5.m[7] = 0.0;

        return ShapeResult{KernelShape(std::make_shared<FakeShapeHandle>(properties)),
                           KernelError::None, {}};
    }

    ShapeResult extrudeProfile(const PlanarProfileDefinition& profile,
                               double distanceMm) override {
        if (!IsValidProfileDefinition(profile))
            return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                               "invalid profile definition"};
        if (!IsValidExtrusionDistance(distanceMm))
            return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                               "invalid extrusion distance"};
        ++extrudeProfileCallCount;

        double area = 0.0;
        Vec2 centroid{};
        // Local second moments of AREA about the cross-section centroid, in the
        // sketch's own (u,v) axes.
        double areaMomentUU = 0.0;
        double areaMomentVV = 0.0;

        if (profile.segments.size() == 1 &&
            std::holds_alternative<ProfileCircleSegment>(profile.segments.front())) {
            const auto& circle = std::get<ProfileCircleSegment>(profile.segments.front());
            const double r = circle.radiusMm;
            area = kPi * r * r;
            centroid = circle.center;
            areaMomentUU = areaMomentVV = kPi * r * r * r * r / 4.0;
        } else {
            std::vector<Vec2> corners;
            corners.reserve(profile.segments.size());
            for (const ProfileSegment& segment : profile.segments) {
                const auto* line = std::get_if<ProfileLineSegment>(&segment);
                if (line == nullptr)
                    return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                                       "FakeGeometryKernel models only rectangles and full "
                                       "circles; use the OCCT kernel for this profile"};
                corners.push_back(line->start);
            }
            if (corners.size() != 4)
                return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                                   "FakeGeometryKernel models only rectangles and full circles; "
                                   "use the OCCT kernel for this profile"};

            const auto minMaxU = std::minmax_element(
                corners.begin(), corners.end(),
                [](Vec2 a, Vec2 b) { return a.x < b.x; });
            const auto minMaxV = std::minmax_element(
                corners.begin(), corners.end(),
                [](Vec2 a, Vec2 b) { return a.y < b.y; });
            const double width = minMaxU.second->x - minMaxU.first->x;
            const double height = minMaxV.second->y - minMaxV.first->y;
            if (width <= 0.0 || height <= 0.0)
                return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                                   "FakeGeometryKernel: degenerate rectangle"};

            // Confirm it really is an axis-aligned rectangle rather than any
            // four-sided polygon, so the closed-form values below are exact.
            for (Vec2 corner : corners) {
                const bool onU = std::fabs(corner.x - minMaxU.first->x) < 1e-9 ||
                                 std::fabs(corner.x - minMaxU.second->x) < 1e-9;
                const bool onV = std::fabs(corner.y - minMaxV.first->y) < 1e-9 ||
                                 std::fabs(corner.y - minMaxV.second->y) < 1e-9;
                if (!onU || !onV)
                    return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                                       "FakeGeometryKernel models only axis-aligned rectangles "
                                       "and full circles; use the OCCT kernel"};
            }

            area = width * height;
            centroid = Vec2{(minMaxU.first->x + minMaxU.second->x) / 2.0,
                            (minMaxV.first->y + minMaxV.second->y) / 2.0};
            areaMomentUU = width * height * height * height / 12.0;
            areaMomentVV = height * width * width * width / 12.0;
        }

        KernelMassProperties properties;
        properties.volumeMm3 = area * distanceMm;

        // COM: cross-section centroid in the plane, lifted half the extrusion
        // distance along the normal.
        const Vec3& o = profile.plane.origin;
        const Vec3& u = profile.plane.uAxis;
        const Vec3& v = profile.plane.vAxis;
        const Vec3& n = profile.plane.normal;
        const double half = distanceMm / 2.0;
        properties.centerOfMassMm =
            Vec3{o.x + centroid.x * u.x + centroid.y * v.x + half * n.x,
                 o.y + centroid.x * u.y + centroid.y * v.y + half * n.y,
                 o.z + centroid.x * u.z + centroid.y * v.z + half * n.z};

        // Second moment of VOLUME about the COM, first in the sketch's own
        // axes, then rotated into part-local XYZ by R * diag * R^T where R's
        // columns are (u, v, n). Without the rotation a Pad on a tilted frame
        // would report a tensor for the wrong axes.
        const double localUU = areaMomentUU * distanceMm +
                               area * distanceMm * distanceMm * distanceMm / 12.0;
        const double localVV = areaMomentVV * distanceMm +
                               area * distanceMm * distanceMm * distanceMm / 12.0;
        const double localNN = (areaMomentUU + areaMomentVV) * distanceMm;
        const double diag[3] = {localUU, localVV, localNN};
        const Vec3 basis[3] = {u, v, n};
        const auto component = [](const Vec3& vec, int axis) {
            return axis == 0 ? vec.x : (axis == 1 ? vec.y : vec.z);
        };
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                double sum = 0.0;
                for (int k = 0; k < 3; ++k)
                    sum += component(basis[k], row) * diag[k] * component(basis[k], col);
                properties.secondMomentMm5.m[static_cast<std::size_t>(row * 3 + col)] = sum;
            }
        }

        return ShapeResult{KernelShape(std::make_shared<FakeShapeHandle>(properties)),
                           KernelError::None, {}};
    }

    KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) override {
        ++calculateMassPropertiesCallCount;
        const auto* fake = dynamic_cast<const FakeShapeHandle*>(shape.handle());
        if (fake == nullptr) {
            return KernelMassPropertiesResult{{}, KernelError::GeometryConstructionFailed,
                                              "shape handle is not a FakeShapeHandle (null or "
                                              "foreign kernel)"};
        }
        return KernelMassPropertiesResult{fake->properties(), KernelError::None, {}};
    }

    ShapeResult subtractShape(const KernelShape& base, const KernelShape& tool) override {
        ++subtractShapeCallCount;
        const auto* fakeBase = dynamic_cast<const FakeShapeHandle*>(base.handle());
        if (fakeBase == nullptr)
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "subtract base is not a FakeShapeHandle (null or foreign kernel)"};
        const auto* fakeTool = dynamic_cast<const FakeShapeHandle*>(tool.handle());
        if (fakeTool == nullptr)
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "subtract tool is not a FakeShapeHandle (null or foreign kernel)"};

        // ANALYTICAL MODEL, and a deliberately narrow one, exactly like the
        // fake's extrudeProfile: volume subtracts, centre of mass recombines by
        // volume-weighted difference, and the second moment is NOT modelled
        // (zeroed). The fake exists so Core-side tests can run without OCCT;
        // any test whose oracle needs a real boolean's inertia belongs in
        // tests/Kernel with the real kernel, not here agreeing with a formula.
        //
        // The volume floors at zero: the fake cannot know overlap, so it
        // models the FULL-CONTAINMENT case (pocket tool entirely inside the
        // base), which is what every Core-level chain test constructs. A
        // disjoint or partial cut is real geometry and belongs to OCCT tests.
        const KernelMassProperties& b = fakeBase->properties();
        const KernelMassProperties& t = fakeTool->properties();
        KernelMassProperties result;
        result.volumeMm3 = b.volumeMm3 - t.volumeMm3;
        if (result.volumeMm3 < 0.0) result.volumeMm3 = 0.0;
        if (result.volumeMm3 > 0.0) {
            result.centerOfMassMm =
                Vec3{(b.centerOfMassMm.x * b.volumeMm3 - t.centerOfMassMm.x * t.volumeMm3) /
                         result.volumeMm3,
                     (b.centerOfMassMm.y * b.volumeMm3 - t.centerOfMassMm.y * t.volumeMm3) /
                         result.volumeMm3,
                     (b.centerOfMassMm.z * b.volumeMm3 - t.centerOfMassMm.z * t.volumeMm3) /
                         result.volumeMm3};
        }
        return ShapeResult{KernelShape(std::make_shared<FakeShapeHandle>(result)),
                           KernelError::None, {}};
    }

    // Axis-aligned rectangle detection shared by the revolve model: four line
    // segments whose union of endpoints spans exactly two u and two v values.
    static bool RectangleBounds(const PlanarProfileDefinition& profile, double& minU,
                                double& maxU, double& minV, double& maxV) {
        if (profile.segments.size() != 4) return false;
        bool first = true;
        for (const ProfileSegment& segment : profile.segments) {
            const auto* line = std::get_if<ProfileLineSegment>(&segment);
            if (line == nullptr) return false;
            for (const Vec2& point : {line->start, line->end}) {
                if (first) {
                    minU = maxU = point.x;
                    minV = maxV = point.y;
                    first = false;
                } else {
                    minU = std::min(minU, point.x);
                    maxU = std::max(maxU, point.x);
                    minV = std::min(minV, point.y);
                    maxV = std::max(maxV, point.y);
                }
            }
            const bool horizontal = line->start.y == line->end.y;
            const bool vertical = line->start.x == line->end.x;
            if (!horizontal && !vertical) return false;
        }
        return maxU > minU && maxV > minV;
    }

    ShapeResult revolveProfile(const PlanarProfileDefinition& profile, const Vec3& axisOriginMm,
                               const Vec3& axisDirection, double angleRad) override {
        ++revolveProfileCallCount;
        if (!IsValidProfileDefinition(profile))
            return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                               "invalid profile definition"};
        if (!IsValidRevolveAngle(angleRad))
            return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                               "invalid revolve angle: must be finite and in (0, 2*pi]"};

        // NARROW ANALYTICAL MODEL, like the fake's other operations: an
        // axis-aligned RECTANGLE of four line segments, revolved a FULL turn
        // about a VERTICAL in-plane axis (direction == the plane's v axis)
        // that does not cross the rectangle. Volume is Pappus, exact under
        // those conditions: V = 2*pi * Rbar * A. Anything else -- partial
        // sweeps, arcs, axis through the profile -- is real geometry and
        // belongs in tests/Kernel with OCCT, so it returns a structured
        // failure rather than a half-right number.
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        if (std::abs(angleRad - kTwoPi) > 1e-12)
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "FakeGeometryKernel models only full revolutions"};
        const auto vAxis = profile.plane.vAxis;
        const double dot = axisDirection.x * vAxis.x + axisDirection.y * vAxis.y +
                           axisDirection.z * vAxis.z;
        const double axisLen = std::sqrt(axisDirection.x * axisDirection.x +
                                         axisDirection.y * axisDirection.y +
                                         axisDirection.z * axisDirection.z);
        if (axisLen <= 0.0 || std::abs(std::abs(dot) / axisLen - 1.0) > 1e-9)
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "FakeGeometryKernel models only axes parallel to the plane's "
                               "v axis"};

        double minU = 0, maxU = 0, minV = 0, maxV = 0;
        if (!RectangleBounds(profile, minU, maxU, minV, maxV))
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "FakeGeometryKernel revolves only axis-aligned rectangles"};

        // The axis's u coordinate in the sketch plane: project (axisOrigin -
        // planeOrigin) onto the u axis.
        const double du = axisOriginMm.x - profile.plane.origin.x;
        const double dv = axisOriginMm.y - profile.plane.origin.y;
        const double dw = axisOriginMm.z - profile.plane.origin.z;
        const double axisU = du * profile.plane.uAxis.x + dv * profile.plane.uAxis.y +
                             dw * profile.plane.uAxis.z;
        if (axisU > minU + 1e-12 && axisU < maxU - 1e-12)
            return ShapeResult{KernelShape{}, KernelError::GeometryConstructionFailed,
                               "FakeGeometryKernel cannot revolve a profile crossing its axis"};

        const double area = (maxU - minU) * (maxV - minV);
        const double rbar = std::abs((minU + maxU) * 0.5 - axisU);
        KernelMassProperties result;
        result.volumeMm3 = kTwoPi * rbar * area;
        // COM of a full solid of revolution lies ON the axis at mid-height.
        const double midV = (minV + maxV) * 0.5;
        result.centerOfMassMm =
            Vec3{profile.plane.origin.x + axisU * profile.plane.uAxis.x + midV * vAxis.x,
                 profile.plane.origin.y + axisU * profile.plane.uAxis.y + midV * vAxis.y,
                 profile.plane.origin.z + axisU * profile.plane.uAxis.z + midV * vAxis.z};
        return ShapeResult{KernelShape(std::make_shared<FakeShapeHandle>(result)),
                           KernelError::None, {}};
    }

    int createBoxCallCount = 0;
    int extrudeProfileCallCount = 0;
    int calculateMassPropertiesCallCount = 0;
    int subtractShapeCallCount = 0;
    int revolveProfileCallCount = 0;

private:
    static constexpr double kPi = 3.14159265358979323846;
};

} // namespace paramcad
