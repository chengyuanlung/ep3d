#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Import/ImportedGeometry.h"
#include "Core/Sketch/SketchTypes.h"
#include <string>
#include <vector>

namespace paramcad {

class PartDocument;

// Turns format-neutral imported geometry into ORDINARY Sketch entities
// (ADR-M6-003/004).
//
// This is the whole point of M6: after import there is nothing about an entity
// that says it came from a file. It carries an ordinary SketchEntityId from the
// shared generator, it persists through the v5 schema, it can be constrained,
// solved, extruded and deleted exactly like an entity the user drew -- which is
// what Gate G proves by driving a Pad from one.
//
// Free of any DXF type, so it is testable with hand-built ImportedSketchGeometry
// and needs no file, no parser and no library present.

struct SketchImportResult {
    ObjectId sketchId{kInvalidObjectId};
    std::vector<SketchEntityId> entityIds; // in creation order; NOT identity
    std::size_t importedCount{0};
    std::size_t skippedCount{0};
    std::string message;

    explicit operator bool() const noexcept { return sketchId != kInvalidObjectId; }
};

// Creates ONE Sketch containing every valid entity in `geometry`.
//
// TRANSACTIONAL at the sketch level (spec 10): if any entity cannot be created,
// the sketch is removed again and the document is left exactly as it was -- no
// half-built sketch, no orphan registry entry, no stray graph node, no dangling
// id. Returning a partially-populated sketch would be worse than failing,
// because the user would have to discover which half arrived.
//
// Entities the READER already rejected arrive in `geometry.skipped` and are
// reported, not re-litigated here; they do not fail the import.
SketchImportResult ImportGeometryIntoNewSketch(PartDocument& document, std::string sketchName,
                                               const ImportedSketchGeometry& geometry);

} // namespace paramcad
