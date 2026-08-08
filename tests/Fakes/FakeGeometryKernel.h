#pragma once

// Test-only IGeometryKernel implementation (spec 16 pattern, applied to the
// M3 kernel boundary). Computes box geometry analytically with the SAME raw
// formulas the oracle tests use independently (volume = w*h*d, COM = box
// center, second moment of volume about COM for a rectangular solid) -- zero
// OCCT dependency, so every Core-side test (BoxFeature, MassPropertiesNode,
// serialization round-trip/recompute-after-load) can run and pass without
// linking Kernel/Occt. Real geometric correctness against OCCT is exercised
// separately in tests/Kernel/OcctGeometryKernelTests.cpp.

#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Kernel/KernelShape.h"
#include "Core/Kernel/KernelTypes.h"
#include <memory>
#include <utility>

namespace paramcad {

// Opaque handle carrying the box definition the fake shape was built from;
// only FakeGeometryKernel ever dynamic_casts to this type (mirrors how
// OcctShape is only ever inspected by OcctGeometryKernel).
class FakeShapeHandle final : public IShapeHandle {
public:
    explicit FakeShapeHandle(BoxDefinition definition) noexcept : definition_(definition) {}
    const BoxDefinition& definition() const noexcept { return definition_; }

private:
    BoxDefinition definition_;
};

class FakeGeometryKernel final : public IGeometryKernel {
public:
    ShapeResult createBox(const BoxDefinition& definition) override {
        if (!IsValidBoxDefinition(definition))
            return ShapeResult{KernelShape{}, KernelError::InvalidDimension,
                               "invalid box definition: dimensions must be finite and positive"};
        ++createBoxCallCount;
        auto handle = std::make_shared<FakeShapeHandle>(definition);
        return ShapeResult{KernelShape(std::move(handle)), KernelError::None, {}};
    }

    KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) override {
        ++calculateMassPropertiesCallCount;
        const auto* fake = dynamic_cast<const FakeShapeHandle*>(shape.handle());
        if (fake == nullptr) {
            return KernelMassPropertiesResult{{}, KernelError::GeometryConstructionFailed,
                                              "shape handle is not a FakeShapeHandle (null or "
                                              "foreign kernel)"};
        }
        const BoxDefinition& d = fake->definition();
        const double w = d.widthMm;
        const double h = d.heightMm;
        const double dp = d.depthMm;

        KernelMassProperties properties;
        properties.volumeMm3 = w * h * dp;
        properties.centerOfMassMm = Vec3{w / 2.0, h / 2.0, dp / 2.0};
        // Second moment of volume about COM for an axis-aligned rectangular
        // solid, mm^5 (same shape as the physical inertia formula, without
        // density -- see ADR-M3-002). Off-diagonal terms are exactly zero.
        const double v = properties.volumeMm3;
        properties.secondMomentMm5.m[0] = v / 12.0 * (h * h + dp * dp); // xx
        properties.secondMomentMm5.m[4] = v / 12.0 * (w * w + dp * dp); // yy
        properties.secondMomentMm5.m[8] = v / 12.0 * (w * w + h * h);  // zz
        properties.secondMomentMm5.m[1] = properties.secondMomentMm5.m[3] = 0.0;
        properties.secondMomentMm5.m[2] = properties.secondMomentMm5.m[6] = 0.0;
        properties.secondMomentMm5.m[5] = properties.secondMomentMm5.m[7] = 0.0;
        return KernelMassPropertiesResult{properties, KernelError::None, {}};
    }

    int createBoxCallCount = 0;
    int calculateMassPropertiesCallCount = 0;
};

} // namespace paramcad
