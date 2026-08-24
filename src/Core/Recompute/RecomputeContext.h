#pragma once

namespace paramcad {

class DocumentBase;
class PartDocument;
class ObjectRegistry;
class IGeometryKernel;
class ISketchSolver;

// Execution context handed to every IRecomputable::recompute invocation.
// kernel is a non-owning pointer (ADR-M3-003): nullptr means "no geometry
// kernel configured," a normal, tested failure path for kernel-dependent
// nodes (BoxFeature/MassPropertiesNode). Forward-declared only -- this header
// adds zero OCCT dependency; IGeometryKernel itself is a Core header with
// zero OCCT (ADR-M3-001), so referencing it here does not violate CodingRule
// 2 ("Core headers may include only standard-library and Core headers").
struct RecomputeContext {
    DocumentBase& document;
    ObjectRegistry& registry;
    IGeometryKernel* kernel = nullptr;
    // Injected the same way and for the same reason (ADR-M3-003 extended to
    // M5): nullptr is a normal, tested failure path for a Sketch that carries
    // constraints, and Core never constructs a concrete solver. ISketchSolver
    // is a Core header free of Eigen (ADR-M5-003), so referencing it here adds
    // no backend dependency.
    ISketchSolver* sketchSolver = nullptr;

    // THE PART THIS NODE BELONGS TO (M23).
    //
    // `document` became a DocumentBase when the Assembly arrived, because one
    // recompute engine drives both. Every Feature and every Sketch still
    // belongs to a Part by construction -- a Body lives in a PartDocument and
    // an Assembly holds instances, not features -- so the sites that need a
    // Part's own answers get them through ONE checked cast here rather than a
    // cast each. Throws if the impossible happens, which is the only honest
    // thing to do with a claim that cannot be false.
    PartDocument& part() const;
};

} // namespace paramcad
