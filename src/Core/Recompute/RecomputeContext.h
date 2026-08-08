#pragma once

namespace paramcad {

class PartDocument;
class ObjectRegistry;
class IGeometryKernel;

// Execution context handed to every IRecomputable::recompute invocation.
// kernel is a non-owning pointer (ADR-M3-003): nullptr means "no geometry
// kernel configured," a normal, tested failure path for kernel-dependent
// nodes (BoxFeature/MassPropertiesNode). Forward-declared only -- this header
// adds zero OCCT dependency; IGeometryKernel itself is a Core header with
// zero OCCT (ADR-M3-001), so referencing it here does not violate CodingRule
// 2 ("Core headers may include only standard-library and Core headers").
struct RecomputeContext {
    PartDocument& document;
    ObjectRegistry& registry;
    IGeometryKernel* kernel = nullptr;
};

} // namespace paramcad
