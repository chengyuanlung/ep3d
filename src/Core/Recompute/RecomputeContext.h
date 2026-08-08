#pragma once

namespace paramcad {

class PartDocument;
class ObjectRegistry;

// Execution context handed to every IRecomputable::recompute invocation.
// Future extension point (units, diagnostics, kernel adapter, cancellation);
// M2 deliberately adds no kernel dependency here.
struct RecomputeContext {
    PartDocument& document;
    ObjectRegistry& registry;
};

} // namespace paramcad
