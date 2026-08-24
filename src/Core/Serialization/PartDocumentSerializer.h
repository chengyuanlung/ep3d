#pragma once

#include "Core/Document/PartDocument.h"
#include "Core/Serialization/SerializationError.h"
#include <iosfwd>
#include <memory>
#include <string>

namespace paramcad {

// Native JSON serialization of PartDocument, schema v3 (loader also accepts
// v1 and v2 files; save always writes v3).
// Covered: document header, scalar parameters (expression round-trips as an
// uninterpreted string), bodies, feature metadata -- PlaceholderFeature and,
// new in v3, BoxFeature (width/height/depth/material references) -- an
// optional Material record, and dependency edges. Feature type dispatch on
// both save and load is keyed by Feature::typeName() (ADR-M3-005), not
// dynamic_cast probing.
// Edge persistence is a hybrid (ADR-012/ADR-M3-005):
//   Option A (persisted, generic "dependencies" array) -- edges whose BOTH
//     endpoints are persisted document objects that do not fall under Option
//     B below; edges touching runtime-only IRecomputable stubs are never
//     saved.
//   Option B (re-derived, NEVER written to "dependencies") -- every edge
//     whose dependent is a Feature (e.g. BoxFeature's Width/Height/Depth
//     prerequisites) or is the document's singleton MassPropertiesNode
//     (never persisted, never even has a "dependencies" entry since its id
//     is never a parameter/body/feature id). These are reconstructed on load
//     purely from semantic id fields (widthParameterId etc.) via the same
//     facade methods used for fresh creation.
// Graph node ComputeStates are NOT persisted (derived data) -- after load
// every node starts Dirty. Not serialized: frames, connectors, mass
// properties (still purely derived), sketches, KernelShape/IShapeHandle/any
// Kernel-Occt runtime state.
// ObjectIds are stored as decimal strings (uint64 exceeds double precision);
// schema v1 accepts ids in [1, kMaxObjectId] only (larger values are rejected
// with InvalidFieldType so the id generator can never wrap), and every id in a
// document must be unique across the document, parameters, bodies, and
// features (violations are rejected with DuplicateId). Enums are stored as
// their exact enumerator-name strings.
// Stateless free functions; no exceptions cross this API.

// The schema version SaveDocument stamps, and the newest one LoadDocument will
// accept.
//
// Exported so a test can say "relabel this as one version older" without
// spelling out the current number -- a literal that lived in five suites and
// turned all of them red at every bump, for reasons that had nothing to do with
// what any of them tests. ONE test still pins the literal on purpose: a bump
// nobody noticed is worth an alarm, and a constant compared against itself is
// not one.
int CurrentSchemaVersion() noexcept;



struct LoadResult {
    std::unique_ptr<PartDocument> document;
    SerializationError error = SerializationError::None;
    std::string message;
    explicit operator bool() const noexcept {
        return error == SerializationError::None && document != nullptr;
    }
};

// Canonical stream API. On load, any error returns a null document with a
// structured error (never a partially restored document). Unknown object keys
// are ignored; missing required fields and wrong JSON types are rejected.
SaveResult savePartDocument(const PartDocument& document, std::ostream& out);
LoadResult loadPartDocument(std::istream& in);

// File convenience wrappers over the stream API.
SaveResult savePartDocumentToFile(const PartDocument& document, const std::string& path);
LoadResult loadPartDocumentFromFile(const std::string& path);

} // namespace paramcad
