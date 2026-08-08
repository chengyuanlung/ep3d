#pragma once

#include "Core/Kernel/KernelShape.h"
#include "Core/Kernel/KernelTypes.h"
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

    // shape must be a valid KernelShape previously returned by this same
    // kernel; an invalid/foreign shape returns a controlled
    // KernelMassPropertiesResult with KernelError::GeometryConstructionFailed
    // rather than dereferencing anything unsafely.
    virtual KernelMassPropertiesResult calculateMassProperties(const KernelShape& shape) = 0;
};

} // namespace paramcad
