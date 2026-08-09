#include "Core/Import/SketchImporter.h"

#include "Core/Document/PartDocument.h"
#include "Core/Sketch/Sketch.h"

#include <sstream>

namespace paramcad {

namespace {

std::string Describe(const ImportedSketchGeometry& geometry, std::size_t created) {
    std::ostringstream out;
    out << "imported " << created << " entit" << (created == 1 ? "y" : "ies")
        << " as " << ImportedLengthUnitName(geometry.unit);
    if (geometry.unitWasDefaulted)
        out << " (assumed: the file did not state a usable unit)";
    if (!geometry.skipped.empty()) out << "; skipped " << geometry.skipped.size();
    return out.str();
}

} // namespace

SketchImportResult ImportGeometryIntoNewSketch(PartDocument& document, std::string sketchName,
                                               const ImportedSketchGeometry& geometry) {
    SketchImportResult result;
    result.skippedCount = geometry.skipped.size();

    Sketch& sketch = document.addSketch(std::move(sketchName));
    const ObjectId sketchId = sketch.id();

    // Rolls the whole sketch back. Called on ANY failure, so a caller never has
    // to reason about which entities made it in (spec 10).
    const auto abandon = [&](std::string why) {
        document.removeObject(sketchId);
        SketchImportResult failed;
        failed.skippedCount = geometry.skipped.size();
        failed.message = std::move(why);
        return failed;
    };

    // Entities are added through the ORDINARY Sketch API, which validates and
    // assigns ids exactly as it does for geometry the user draws. An id of
    // kInvalidSketchEntityId means the geometry did not survive that validation
    // -- and since the reader has already filtered what it could, reaching this
    // point means the two disagree, which is a fault worth failing on rather
    // than quietly dropping an entity the user can see in their file.
    for (const ImportedLine2D& line : geometry.lines) {
        const SketchEntityId id = sketch.addLine(line.start, line.end);
        if (id == kInvalidSketchEntityId)
            return abandon("a line in the file is not valid sketch geometry");
        result.entityIds.push_back(id);
    }
    for (const ImportedCircle2D& circle : geometry.circles) {
        const SketchEntityId id = sketch.addCircle(circle.center, circle.radiusMm);
        if (id == kInvalidSketchEntityId)
            return abandon("a circle in the file is not valid sketch geometry");
        result.entityIds.push_back(id);
    }
    for (const ImportedArc2D& arc : geometry.arcs) {
        const SketchEntityId id =
            sketch.addArc(arc.center, arc.radiusMm, arc.startAngleRad, arc.endAngleRad);
        if (id == kInvalidSketchEntityId)
            return abandon("an arc in the file is not valid sketch geometry");
        result.entityIds.push_back(id);
    }

    // An import that produced nothing is a failure, not an empty success: the
    // user asked for a file's contents and got a sketch they would have to
    // discover was empty. The message says whether anything was in the file at
    // all, because "unsupported" and "malformed" call for different actions.
    if (result.entityIds.empty()) {
        return abandon(geometry.skipped.empty()
                           ? "the file contained no importable geometry"
                           : "every entity in the file was skipped; nothing to import");
    }

    // The sketch was only ever a container until now; dirty it so the ordinary
    // recompute machinery treats it like any other edited sketch.
    document.markSketchDirty(sketchId);

    result.sketchId = sketchId;
    result.importedCount = result.entityIds.size();
    result.message = Describe(geometry, result.importedCount);
    return result;
}

} // namespace paramcad
