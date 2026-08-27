#include "Core/Serialization/DrawingDocumentSerializer.h"

#include "Core/Serialization/DocumentJson.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace paramcad {

namespace {

// M47. Makes the "not handled" branch of a visit a COMPILE error rather than a
// value.
template <class>
inline constexpr bool kNoSaverFor = false;

using docjson::FieldError;
using docjson::fieldError;
using docjson::idFromString;
using docjson::idToString;
using docjson::requireField;

// AN ANCHOR IS A REFERENCE, not a coordinate -- except for Free, which is a
// coordinate on purpose. What is written is exactly what the resolver asks
// again on the next rebuild.
//
// LIFTED OUT OF THE DIMENSION WRITER (M41). The symbols point at things the
// same way a dimension does, and a second codec for the same struct is two
// readings that drift: the day one of them learns a new anchor kind, files
// written by one half stop opening in the other.
JsonValue WriteDimensionAnchor(const DimensionAnchor& anchor) {
    JsonValue out = JsonValue::makeObject();
    out.set("kind", JsonValue::makeString(std::string(toString(anchor.kind))));
    out.set("x", JsonValue::makeNumber(anchor.at.x));
    out.set("y", JsonValue::makeNumber(anchor.at.y));
    out.set("entityId", JsonValue::makeString(idToString(anchor.entityId)));
    out.set("snapIndex", JsonValue::makeNumber(static_cast<double>(anchor.snapIndex)));
    out.set("viewId", JsonValue::makeString(idToString(anchor.viewId)));
    out.set("toleranceMm", JsonValue::makeNumber(anchor.toleranceMm));
    // v45 (M43). WHAT KIND of point an in-view anchor was put on. Written for
    // every anchor and read for every anchor, because there is one codec --
    // which is the whole reason lifting it out of two lambdas was worth doing.
    out.set("role", JsonValue::makeString(std::string(toString(anchor.role))));
    return out;
}

// --- A WELD BEAD, BOTH WAYS (v47, M47) --------------------------------------
//
// ONE CODEC, for the reason the anchor has one: the arrow side and the other
// side are the same struct written twice, and two readings of it would be two
// chances for one side to learn a field the other does not. A weld whose far
// side quietly lost its intermittent run is a drawing that reads as a
// continuous weld -- and it is the side nobody looks at.
JsonValue WriteWeldBead(const WeldBead& bead) {
    JsonValue out = JsonValue::makeObject();
    out.set("type", JsonValue::makeString(std::string(toString(bead.type))));
    out.set("sizeMm", JsonValue::makeNumber(bead.sizeMm));
    out.set("sizeKind", JsonValue::makeString(std::string(toString(bead.sizeKind))));
    out.set("contour", JsonValue::makeString(std::string(toString(bead.contour))));
    if (bead.run.has_value()) {
        JsonValue run = JsonValue::makeObject();
        run.set("count", JsonValue::makeNumber(static_cast<double>(bead.run->count)));
        run.set("lengthMm", JsonValue::makeNumber(bead.run->lengthMm));
        // THE GAP, under the name it has. Called "pitch" it would be read as
        // the AWS number by the next person to open this file.
        run.set("gapMm", JsonValue::makeNumber(bead.run->gapMm));
        out.set("run", std::move(run));
    }
    return out;
}

bool ReadWeldBead(const JsonValue& entry, const std::string& context, FieldError& err,
                  WeldBead& into) {
    const JsonValue* type = requireField(entry, "type", JsonType::String, context, err);
    if (type == nullptr) return false;
    // REFUSED, NOT DEFAULTED. A type this build does not know would become a
    // fillet, and a butt weld read as a fillet is a joint with no penetration.
    if (!ParseWeldType(type->asString(), into.type)) {
        err = fieldError(SerializationError::InvalidEnumValue,
                         context + ": unknown weld type '" + type->asString() + "'");
        return false;
    }
    const JsonValue* size = requireField(entry, "sizeMm", JsonType::Number, context, err);
    if (size == nullptr) return false;
    into.sizeMm = size->asNumber();
    if (const JsonValue* kind = entry.find("sizeKind")) {
        if (kind->type() != JsonType::String) {
            err = fieldError(SerializationError::InvalidFieldType,
                             context + ": field 'sizeKind' is not a string");
            return false;
        }
        // THROAT AND LEG ARE DIFFERENT WELDS. Falling back to either one turns
        // a5 into z5 or the other way about, and both print as one number.
        if (!ParseFilletSizeKind(kind->asString(), into.sizeKind)) {
            err = fieldError(SerializationError::InvalidEnumValue,
                             context + ": unknown fillet size kind '" + kind->asString() + "'");
            return false;
        }
    }
    if (const JsonValue* contour = entry.find("contour")) {
        if (contour->type() != JsonType::String) {
            err = fieldError(SerializationError::InvalidFieldType,
                             context + ": field 'contour' is not a string");
            return false;
        }
        if (!ParseWeldContour(contour->asString(), into.contour)) {
            err = fieldError(SerializationError::InvalidEnumValue,
                             context + ": unknown weld contour '" + contour->asString() + "'");
            return false;
        }
    }
    if (const JsonValue* run = entry.find("run")) {
        if (run->type() != JsonType::Object) {
            err = fieldError(SerializationError::InvalidFieldType,
                             context + ": field 'run' is not an object");
            return false;
        }
        WeldRun into_run;
        const JsonValue* count = requireField(*run, "count", JsonType::Number, context, err);
        if (count == nullptr) return false;
        into_run.count = static_cast<int>(count->asNumber());
        if (const JsonValue* length = run->find("lengthMm"))
            if (length->type() == JsonType::Number) into_run.lengthMm = length->asNumber();
        if (const JsonValue* gap = run->find("gapMm"))
            if (gap->type() == JsonType::Number) into_run.gapMm = gap->asNumber();
        into.run = into_run;
    }
    return true;
}

// The other half of the same codec. Returns false with `err` filled for a
// missing or mistyped field, and false with `err` untouched for an anchor kind
// this build does not know -- which the caller turns into its own message.
// THE PAPER, ITS FRAME AND ITS TITLE BLOCK (M44).
//
// Lifted out of the three places they were written inline, for the reason the
// anchor codec was: a drawing has SEVERAL pages now, and three copies of "how
// a page is written" is three chances for one to learn a field the others do
// not.
JsonValue WriteSheet(const Sheet& sheet) {
    JsonValue paper = JsonValue::makeObject();
    paper.set("size", JsonValue::makeString(std::string(toString(sheet.size()))));
    paper.set("orientation",
              JsonValue::makeString(std::string(toString(sheet.orientation()))));
    paper.set("scale", JsonValue::makeString(sheet.scale().toString()));
    // WHICH SIDE THE PROJECTED VIEWS GO. On the sheet, because a drawing is in
    // one convention or the other and never both -- which is what the
    // projection symbol in a title block promises.
    paper.set("projectionAngle",
              JsonValue::makeString(std::string(toString(sheet.projectionAngle()))));
    if (sheet.size() == SheetSize::Custom) {
        // AS TYPED. A custom sheet ignores orientation (see Sheet::widthMm),
        // so these two numbers ARE its width and height in both directions.
        paper.set("widthMm", JsonValue::makeNumber(sheet.customWidthMm()));
        paper.set("heightMm", JsonValue::makeNumber(sheet.customHeightMm()));
    }
    return paper;
}

bool ReadDimensionAnchor(const JsonValue& entry, const char* key, const std::string& context,
                         docjson::FieldError& err, DimensionAnchor& into) {
    const JsonValue* at = requireField(entry, key, JsonType::Object, context, err);
    if (at == nullptr) return false;
    const JsonValue* kindField = requireField(*at, "kind", JsonType::String, context, err);
    if (kindField == nullptr) return false;
    if (kindField->asString() == "Free")
        into.kind = DimensionAnchorKind::Free;
    else if (kindField->asString() == "Entity")
        into.kind = DimensionAnchorKind::Entity;
    else if (kindField->asString() == "InView")
        into.kind = DimensionAnchorKind::InView;
    else
        return false;
    const JsonValue* x = requireField(*at, "x", JsonType::Number, context, err);
    const JsonValue* y = requireField(*at, "y", JsonType::Number, context, err);
    if (x == nullptr || y == nullptr) return false;
    into.at = Vec2{x->asNumber(), y->asNumber()};
    if (const JsonValue* entityId = at->find("entityId"))
        if (const auto parsed = idFromString(entityId->asString())) into.entityId = *parsed;
    if (const JsonValue* snapIndex = at->find("snapIndex"))
        if (snapIndex->type() == JsonType::Number)
            into.snapIndex = static_cast<int>(snapIndex->asNumber());
    if (const JsonValue* viewId = at->find("viewId"))
        if (const auto parsed = idFromString(viewId->asString())) into.viewId = *parsed;
    if (const JsonValue* tolerance = at->find("toleranceMm"))
        if (tolerance->type() == JsonType::Number && tolerance->asNumber() > 0.0)
            into.toleranceMm = tolerance->asNumber();
    // OPTIONAL, because a file written before v45 has none -- and an anchor
    // from such a file keeps the Corner it was constructed with, which is what
    // those files meant when every anchor took the nearest point of any kind.
    //
    // A role that IS written and is not one this build knows is REFUSED, not
    // defaulted: a centre read as a corner is the silent re-attachment this
    // whole mechanism exists to stop.
    if (const JsonValue* role = at->find("role")) {
        if (role->type() != JsonType::String) return false;
        if (!ParseViewPointRole(role->asString(), into.role)) return false;
    }
    return true;
}

DrawingLoadResult loadFailure(SerializationError error, std::string message) {
    return DrawingLoadResult{nullptr, error, std::move(message)};
}

std::optional<SheetSize> sheetSizeFromString(std::string_view text) {
    if (text == "A0") return SheetSize::A0;
    if (text == "A1") return SheetSize::A1;
    if (text == "A2") return SheetSize::A2;
    if (text == "A3") return SheetSize::A3;
    if (text == "A4") return SheetSize::A4;
    if (text == "Custom") return SheetSize::Custom;
    return std::nullopt;
}

std::optional<SheetOrientation> sheetOrientationFromString(std::string_view text) {
    if (text == "Portrait") return SheetOrientation::Portrait;
    if (text == "Landscape") return SheetOrientation::Landscape;
    return std::nullopt;
}

// THE OTHER HALF OF WriteSheet, and the ONE reader (M44).
//
// A drawing from before M44 has its paper at the top level; one written since
// has a paper per page. Same fields, same rules -- so the same function, or
// the day one of them learns about a new sheet size the other silently makes
// it A4.
bool ReadSheet(const JsonValue& paper, const std::string& context, docjson::FieldError& err,
               Sheet& into) {
    const JsonValue* size = requireField(paper, "size", JsonType::String, context, err);
    if (size == nullptr) return false;
    const auto parsedSize = sheetSizeFromString(size->asString());
    if (!parsedSize.has_value()) {
        err = docjson::fieldError(SerializationError::InvalidEnumValue,
                                  context + ": unknown sheet size '" + size->asString() + "'");
        return false;
    }

    const JsonValue* orientation =
        requireField(paper, "orientation", JsonType::String, context, err);
    if (orientation == nullptr) return false;
    const auto parsedOrientation = sheetOrientationFromString(orientation->asString());
    if (!parsedOrientation.has_value()) {
        err = docjson::fieldError(SerializationError::InvalidEnumValue,
                                  context + ": unknown orientation '" +
                                      orientation->asString() + "'");
        return false;
    }

    const JsonValue* scale = requireField(paper, "scale", JsonType::String, context, err);
    if (scale == nullptr) return false;
    DrawingScale parsedScale;
    if (!ParseDrawingScale(scale->asString(), parsedScale)) {
        err = docjson::fieldError(SerializationError::InvalidFieldType,
                                  context + ": '" + scale->asString() +
                                      "' is not a scale like 1:2");
        return false;
    }

    ProjectionAngle angle = ProjectionAngle::First;
    if (const JsonValue* value = paper.find("projectionAngle")) {
        if (value->type() != JsonType::String) {
            err = docjson::fieldError(SerializationError::InvalidFieldType,
                                      context + ": field 'projectionAngle' is not a string");
            return false;
        }
        if (value->asString() == "First") {
            angle = ProjectionAngle::First;
        } else if (value->asString() == "Third") {
            angle = ProjectionAngle::Third;
        } else {
            err = docjson::fieldError(SerializationError::InvalidEnumValue,
                                      context + ": unknown projection angle '" +
                                          value->asString() + "'");
            return false;
        }
    }

    Sheet built{*parsedSize, *parsedOrientation};
    if (*parsedSize == SheetSize::Custom) {
        const JsonValue* width = requireField(paper, "widthMm", JsonType::Number, context, err);
        if (width == nullptr) return false;
        const JsonValue* height =
            requireField(paper, "heightMm", JsonType::Number, context, err);
        if (height == nullptr) return false;
        if (!built.setCustomSize(width->asNumber(), height->asNumber())) {
            err = docjson::fieldError(SerializationError::InvalidFieldType,
                                      context + ": a custom sheet has no area");
            return false;
        }
    }
    built.setScale(parsedScale);
    built.setProjectionAngle(angle);
    into = built;
    return true;
}


std::optional<ViewDirection> viewDirectionFromString(std::string_view text) {
    if (text == "Front") return ViewDirection::Front;
    if (text == "Back") return ViewDirection::Back;
    if (text == "Left") return ViewDirection::Left;
    if (text == "Right") return ViewDirection::Right;
    if (text == "Top") return ViewDirection::Top;
    if (text == "Bottom") return ViewDirection::Bottom;
    if (text == "Isometric") return ViewDirection::Isometric;
    return std::nullopt;
}

// EVERY REFERENCE THE LOADER CHECKS, CHECKED AT SAVE TIME (ADR-M3-008).
//
// The named worst case in this project is a document that saves cleanly and
// then refuses to load: the good file on disk is already gone by the time
// anybody finds out.
SaveResult validateSaveable(const DrawingDocument& document) {
    if (document.id() > kMaxObjectId)
        return SaveResult{SerializationError::InvalidFieldType,
                          "document id exceeds the maximum ObjectId this format can carry"};

    std::unordered_set<ObjectId> seen{document.id()};
    const auto claim = [&seen](ObjectId id, const char* what) -> SaveResult {
        if (id > kMaxObjectId)
            return SaveResult{SerializationError::InvalidFieldType,
                              std::string(what) + " id exceeds the maximum ObjectId this "
                                                  "format can carry"};
        if (!seen.insert(id).second)
            return SaveResult{SerializationError::DuplicateId,
                              std::string(what) + " id " + idToString(id) +
                                  " is used twice in this document"};
        return SaveResult{};
    };

    for (const ReferenceFrame* frame : document.frames())
        if (const SaveResult bad = claim(frame->id(), "frame"); !bad) return bad;
    for (const Connector* connector : document.connectors()) {
        if (const SaveResult bad = claim(connector->id(), "connector"); !bad) return bad;
        if (document.findFrame(connector->frameId()) == nullptr)
            return SaveResult{SerializationError::UnknownDependencyId,
                              "connector '" + connector->name() +
                                  "' names a frame that is not in this document"};
    }

    bool sawContinuous = false;
    for (const Linetype* linetype : document.linetypes()) {
        if (const SaveResult bad = claim(linetype->id(), "linetype"); !bad) return bad;
        if (linetype->name() == kContinuousLinetypeName) sawContinuous = true;
    }
    if (!sawContinuous)
        return SaveResult{SerializationError::MissingField,
                          "a drawing has no CONTINUOUS linetype, which every DXF reader "
                          "expects to find"};

    bool sawLayerZero = false;
    for (const Layer* layer : document.layers()) {
        if (const SaveResult bad = claim(layer->id(), "layer"); !bad) return bad;
        if (layer->name() == kDefaultLayerName) sawLayerZero = true;
        // A LAYER NAMING A LINETYPE THAT IS NOT IN THE TABLE is a file other
        // programs refuse to open. The loader checks it, so this checks it
        // first.
        if (document.findLinetypeNamed(layer->linetype()) == nullptr)
            return SaveResult{SerializationError::UnknownDependencyId,
                              "layer '" + layer->name() + "' uses linetype '" +
                                  layer->linetype() + "', which is not in this document"};
    }
    if (!sawLayerZero)
        return SaveResult{SerializationError::MissingField,
                          "a drawing has no layer '0', which every DXF reader expects to find"};

    if (document.findLayer(document.currentLayerId()) == nullptr)
        return SaveResult{SerializationError::UnknownDependencyId,
                          "the current layer is not a layer in this document"};

    for (const DrawingView* view : document.views()) {
        if (const SaveResult bad = claim(view->id(), "view"); !bad) return bad;
        // A view with no file can never build, so it can never be anything but
        // a failed row in the tree. Refused where the reason is near the cause.
        if (view->sourcePath().empty())
            return SaveResult{SerializationError::MissingField,
                              "view '" + view->name() + "' names no model file"};
        if (const std::string why = document.whyViewCannotSitAt(view->positionMm());
            !why.empty())
            return SaveResult{SerializationError::InvalidFieldType,
                              "view '" + view->name() + "': " + why};
        if (!view->scale().valid())
            return SaveResult{SerializationError::InvalidFieldType,
                              "view '" + view->name() + "' has a scale that is not a ratio"};
    }
    // v36 (M33). An entity names a layer and a linetype, and the loader checks
    // both -- so this checks them first (ADR-M3-008).
    for (const DrawingEntity* entity : document.entities()) {
        if (const SaveResult bad = claim(entity->id(), "entity"); !bad) return bad;
        if (document.findLayer(entity->layerId()) == nullptr)
            return SaveResult{SerializationError::UnknownDependencyId,
                              std::string(ShapeName(entity->shape())) +
                                  " is on a layer that is not in this document"};
        if (entity->linetype() != "BYLAYER" && entity->linetype() != "BYBLOCK" &&
            document.findLinetypeNamed(entity->linetype()) == nullptr)
            return SaveResult{SerializationError::UnknownDependencyId,
                              std::string(ShapeName(entity->shape())) + " uses linetype '" +
                                  entity->linetype() + "', which is not in this document"};
    }
    // v37 (M34). A style is a named table entry like a layer; a dimension
    // names one, and a layer. Both checked here first (ADR-M3-008).
    bool sawDefaultStyle = false;
    for (const DimensionStyle* style : document.dimensionStyles()) {
        if (const SaveResult bad = claim(style->id(), "dimension style"); !bad) return bad;
        if (style->name() == kDefaultDimensionStyleName) sawDefaultStyle = true;
    }
    if (!sawDefaultStyle)
        return SaveResult{SerializationError::MissingField,
                          "a drawing has no " + std::string(kDefaultDimensionStyleName) +
                              " dimension style, which every dimension falls back on"};
    for (const DrawingDimension* dimension : document.dimensions()) {
        if (const SaveResult bad = claim(dimension->id(), "dimension"); !bad) return bad;
        if (document.findDimensionStyle(dimension->styleId()) == nullptr)
            return SaveResult{SerializationError::UnknownDependencyId,
                              "a dimension names a style that is not in this document"};
        if (document.findLayer(dimension->layerId()) == nullptr)
            return SaveResult{SerializationError::UnknownDependencyId,
                              "a dimension is on a layer that is not in this document"};
    }
    // v38 (M35). The title block must still be able to identify the drawing,
    // and the frame must still fit the paper -- both checked HERE, because the
    // loader checks both, and a document that saves cleanly and then refuses
    // to load is the named worst case (ADR-M3-008).
    if (document.titleBlock().findField(kTitleBlockTitleLabel) == nullptr ||
        document.titleBlock().findField(kTitleBlockNumberLabel) == nullptr)
        return SaveResult{SerializationError::MissingField,
                          "this drawing's title block has lost the rows that identify it"};
    if (!(document.titleBlock().widthMm() > 0.0) ||
        !(document.titleBlock().rowHeightMm() > 0.0))
        return SaveResult{SerializationError::InvalidFieldType,
                          "a title block with no width or no row height draws nothing "
                          "anybody can read"};
    {
        std::vector<std::string> seenLabels;
        for (const TitleBlockField& field : document.titleBlock().fields()) {
            if (field.label.empty())
                return SaveResult{SerializationError::MissingField,
                                  "a title block row has no label"};
            for (const std::string& already : seenLabels)
                if (already == field.label)
                    return SaveResult{SerializationError::DuplicateId,
                                      "two title block rows are both called '" + field.label +
                                          "'"};
            seenLabels.push_back(field.label);
        }
    }
    // v40 (M36). A component must be identifiable and drawable, and a wire
    // must be a wire -- checked here because the loader checks the same
    // (ADR-M3-008).
    for (const SymbolPlacement* symbol : document.symbols()) {
        if (const SaveResult bad = claim(symbol->id(), "component"); !bad) return bad;
        if (symbol->tag().empty())
            return SaveResult{SerializationError::MissingField,
                              "a component has no tag, so nothing on this drawing can refer "
                              "to it"};
        if (symbol->symbolName().empty())
            return SaveResult{SerializationError::MissingField,
                              "a component names no symbol, so there is nothing to draw"};
        if (document.findLayer(symbol->layerId()) == nullptr)
            return SaveResult{SerializationError::UnknownDependencyId,
                              "a component is on a layer that is not in this document"};
    }
    for (const WireEntity* wire : document.wires()) {
        if (const SaveResult bad = claim(wire->id(), "wire"); !bad) return bad;
        if (wire->pointsMm().size() < 2)
            return SaveResult{SerializationError::InvalidFieldType,
                              "a wire has fewer than two points, so it connects nothing"};
        if (document.findLayer(wire->layerId()) == nullptr)
            return SaveResult{SerializationError::UnknownDependencyId,
                              "a wire is on a layer that is not in this document"};
    }

    // v46 (M44). EVERY OBJECT HAS TO BE ON A PAGE THAT IS HERE, and the check
    // is the DOCUMENT'S OWN -- the same call the loader makes. An object on a
    // page that has gone is on no tab at all: it cannot be found, moved or
    // deleted, and nothing on the screen says it exists.
    if (const std::string why = document.whyDrawingRefused(); !why.empty())
        return SaveResult{SerializationError::UnknownDependencyId, why};

    // v44 (M41). EVERY SYMBOL HAS TO BE ONE THAT CAN BE DRAWN, and the check
    // is the DOCUMENT'S OWN -- the same call the loader makes below, and the
    // same one the painter asks before drawing. Written out here as its own
    // list of rules it would be a third reading of ISO 1101, and the day one
    // of the three was corrected the other two would still be wrong.
    //
    // AND THIS HALF IS CURRENTLY UNREACHABLE, which is recorded here rather
    // than left for the next reader to work out: addAnnotation refuses an
    // undrawable body, setAnnotationBody refuses one, deleting a datum that
    // frames still name is refused, and undo restores in reverse order so a
    // frame never comes back before its datum. A mutation deleting the check
    // below therefore survives. It stays because the rule it states is the
    // LOADER'S, and a saver that does not state the loader's rule is how
    // ADR-M3-008's worst case -- a file that writes cleanly and will not
    // reopen -- arrives the next time one of those four doors is widened.
    for (const Annotation* annotation : document.annotations()) {
        if (const SaveResult bad = claim(annotation->id(), "symbol"); !bad) return bad;
        const std::string why = document.whyAnnotationRefused(annotation->id());
        if (!why.empty())
            return SaveResult{SerializationError::InvalidFieldType,
                              "a symbol on this sheet cannot be drawn: " + why};
    }

    // v43 (M39). A HOLE TABLE IS A TABLE OF A VIEW'S HOLES, so that view has
    // to be here. Checked at save because the loader checks it: a drawing that
    // wrote a table pointing at nothing would save cleanly and then refuse to
    // reopen, which is the named worst case (ADR-M3-008).
    for (const HoleTable* table : document.holeTables()) {
        if (const SaveResult bad = claim(table->id(), "hole table"); !bad) return bad;
        bool sawTheView = false;
        for (const DrawingView* view : document.views())
            if (view->id() == table->viewId()) sawTheView = true;
        if (!sawTheView)
            return SaveResult{SerializationError::UnknownDependencyId,
                              "hole table '" + table->name() +
                                  "' is a table of a view that is not in this drawing"};
    }

    // v39 (M35.6). A parts list is a view of an assembly: it must name one,
    // and its columns must be a real, distinct set -- checked HERE because the
    // loader checks the same, and a document that saves cleanly and then
    // refuses to load is the named worst case (ADR-M3-008).
    for (const BomTable* table : document.bomTables()) {
        if (const SaveResult bad = claim(table->id(), "parts list"); !bad) return bad;
        if (table->sourcePath().empty())
            return SaveResult{SerializationError::MissingField,
                              "a parts list names no assembly, so there is nothing for it "
                              "to count"};
        // NO CHECK HERE FOR AN EMPTY OR REPEATED COLUMN SET.
        //
        // Both were written, and both were dead: the constructor seeds four
        // columns, setColumns refuses an empty or repeating set, and
        // restoreBomTable routes through setColumns -- so no in-memory table
        // can reach either state. Mutations deleting them survived, which is
        // what dead defensive code looks like from the outside.
        //
        // The reachable version of the rule is the LOADER's, because a
        // hand-edited file can carry both. It is checked there, and tested.
        if (!(table->rowHeightMm() > 0.0))
            return SaveResult{SerializationError::InvalidFieldType,
                              "a parts list with no row height draws nothing anybody can read"};
    }
    if (!document.frameMargins().fitsOn(document.sheet().widthMm(),
                                        document.sheet().heightMm()))
        return SaveResult{SerializationError::InvalidFieldType,
                          "this drawing's margins are wider than its paper, so there is no "
                          "inside left to draw in"};
    if (!(document.frameZoneTargetMm() > 0.0))
        return SaveResult{SerializationError::InvalidFieldType,
                          "a zone size of zero divides the border into nothing"};
    // v42 (M38). A section has to be cut FROM something, and its line has to
    // have length -- both checked here because the loader checks both
    // (ADR-M3-008).
    for (const DrawingView* view : document.views()) {
        if (!view->isSection()) continue;
        if (view->parentViewId() == kInvalidObjectId)
            return SaveResult{SerializationError::MissingField,
                              "a section view is not cut from any view"};
        if (!view->sectionCut().usable())
            return SaveResult{SerializationError::InvalidFieldType,
                              "a section's cut line has no length, so it cuts nothing"};
    }

    // v41 (M37). A fit the build cannot compute must not be saved as if it
    // were fine: the file would open on another machine, print a size with no
    // tolerance where a fit was asked for, and look finished.
    for (const DrawingDimension* dimension : document.dimensions()) {
        if (dimension->tolerance().kind != ToleranceKind::Fit) continue;
        if (dimension->tolerance().fitCode.empty())
            return SaveResult{SerializationError::MissingField,
                              "a dimension is marked as a fit and names no fit code"};
        if (!document.dimensionFit(*dimension).has_value())
            return SaveResult{SerializationError::InvalidFieldType,
                              "a dimension names the fit '" + dimension->tolerance().fitCode +
                                  "', which this build cannot compute at that size"};
    }
    if (!document.sheet().scale().valid())
        return SaveResult{SerializationError::InvalidFieldType,
                          "the sheet scale is not a ratio"};
    return SaveResult{};
}

JsonValue toJson(const DrawingDocument& document) {
    JsonValue root = JsonValue::makeObject();
    docjson::writeHeader(root, "Drawing", document.id(), document.name());
    root.set("frames", docjson::framesToJson(document));
    root.set("connectors", docjson::connectorsToJson(document));

    // THE PAPER. A scale is written as the ratio the user typed ("1:2"), not
    // as a quotient -- 0.5 cannot be read back into a title block.

    // LINETYPES BEFORE LAYERS, because a layer names one. The order is the
    // loader's order too, so a file can be read straight through.
    JsonValue linetypes = JsonValue::makeArray();
    for (const Linetype* linetype : document.linetypes()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue::makeString(idToString(linetype->id())));
        entry.set("name", JsonValue::makeString(linetype->name()));
        entry.set("description", JsonValue::makeString(linetype->description()));
        JsonValue pattern = JsonValue::makeArray();
        for (const double segment : linetype->pattern())
            pattern.add(JsonValue::makeNumber(segment));
        entry.set("pattern", std::move(pattern));
        linetypes.add(std::move(entry));
    }
    root.set("linetypes", std::move(linetypes));

    JsonValue layers = JsonValue::makeArray();
    for (const Layer* layer : document.layers()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue::makeString(idToString(layer->id())));
        entry.set("name", JsonValue::makeString(layer->name()));
        entry.set("color", JsonValue::makeNumber(static_cast<double>(layer->color())));
        entry.set("linetype", JsonValue::makeString(layer->linetype()));
        // THREE FLAGS, not one visibility: off, frozen and locked do three
        // different things and a file that merged them would come back with
        // layers behaving differently than they were left.
        entry.set("on", JsonValue::makeBool(layer->isOn()));
        entry.set("frozen", JsonValue::makeBool(layer->isFrozen()));
        entry.set("locked", JsonValue::makeBool(layer->isLocked()));
        entry.set("lineweight",
                  JsonValue::makeNumber(static_cast<double>(layer->lineweight())));
        layers.add(std::move(entry));
    }
    root.set("layers", std::move(layers));
    root.set("currentLayerId", JsonValue::makeString(idToString(document.currentLayerId())));

    JsonValue views = JsonValue::makeArray();
    for (const DrawingView* view : document.views()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue::makeString(idToString(view->id())));
        // v46 (M44). WHICH PAGE, resolved -- a file never says "wherever the
        // first one is", because the reader would have to make the same guess
        // and two guesses is a pair to keep in step.
        entry.set("sheetId",
                  JsonValue::makeString(idToString(document.sheetOfObject(view->id()))));
        entry.set("name", JsonValue::makeString(view->name()));
        // A PATH AND A BODY NAME -- the sentence "that body, in that file",
        // exactly as an instance stores it (ADR-M22-003). No geometry: the
        // model file is the truth, and a cached picture would be a second
        // thing that has to be right about when the model changed.
        entry.set("sourcePath", JsonValue::makeString(view->sourcePath()));
        entry.set("bodyName", JsonValue::makeString(view->bodyName()));
        entry.set("direction", JsonValue::makeString(std::string(toString(view->direction()))));
        entry.set("xMm", JsonValue::makeNumber(view->positionMm().x));
        entry.set("yMm", JsonValue::makeNumber(view->positionMm().y));
        // `ownScale` says whether this view has an opinion at all, so "same as
        // the sheet" survives the sheet later being changed.
        entry.set("ownScale", JsonValue::makeBool(view->hasOwnScale()));
        entry.set("scale", JsonValue::makeString(view->scale().toString()));
        // v42 (M38). THE CUT LINE, AND NOT THE PLANE IT MAKES. The plane is
        // worked out from the parent's camera at every recompute, so writing
        // it would put a second answer in the file -- one that goes wrong the
        // moment somebody turns the parent, leaving a section of a place the
        // line no longer crosses.
        if (view->isSection()) {
            JsonValue cut = JsonValue::makeObject();
            cut.set("fromXMm", JsonValue::makeNumber(view->sectionCut().fromMm.x));
            cut.set("fromYMm", JsonValue::makeNumber(view->sectionCut().fromMm.y));
            cut.set("toXMm", JsonValue::makeNumber(view->sectionCut().toMm.x));
            cut.set("toYMm", JsonValue::makeNumber(view->sectionCut().toMm.y));
            cut.set("arrowSide",
                    JsonValue::makeNumber(static_cast<double>(view->sectionCut().arrowSide)));
            entry.set("section", std::move(cut));
        }
        // v49 (M49). THE CIRCLE, AND NOT THE CURVES IT KEPT. What survives the
        // crop is worked out from the parent's projection at every recompute,
        // so writing the cropped geometry would be a second answer -- one that
        // goes stale the moment the model changes, leaving a detail of a
        // feature that has moved.
        if (view->isDetail()) {
            JsonValue circle = JsonValue::makeObject();
            circle.set("centreXMm", JsonValue::makeNumber(view->detailFrame().centreMm.x));
            circle.set("centreYMm", JsonValue::makeNumber(view->detailFrame().centreMm.y));
            circle.set("radiusMm", JsonValue::makeNumber(view->detailFrame().radiusMm));
            entry.set("detail", std::move(circle));
        }
        // v50 (M50). WHERE THE MIDDLE WENT -- the span, not the shortened
        // curves. The curves are the part's, and a file that stored them
        // folded would be a drawing whose geometry is a length the part is
        // not.
        if (view->breakSpan().active) {
            JsonValue span = JsonValue::makeObject();
            span.set("fromMm", JsonValue::makeNumber(view->breakSpan().fromMm));
            span.set("toMm", JsonValue::makeNumber(view->breakSpan().toMm));
            span.set("horizontal", JsonValue::makeBool(view->breakSpan().horizontal));
            span.set("gapMm", JsonValue::makeNumber(view->breakSpan().gapMm));
            entry.set("break", std::move(span));
        }
        // v52 (M53). WHETHER THIS IS THE BLANK. Written for every view rather
        // than only when true: a reader that had to infer "not a flat pattern"
        // from an absent field would be inferring it for every view made
        // before this build too, and being right by luck is not the same as
        // being told.
        entry.set("flatPattern", JsonValue::makeBool(view->showsFlatPattern()));
        // DRAWING CONVENTIONS, on the view: two views of the same part on one
        // sheet may reasonably differ about them.
        entry.set("showHiddenLines", JsonValue::makeBool(view->showsHiddenLines()));
        entry.set("showTangentEdges", JsonValue::makeBool(view->showsTangentEdges()));
        // A CHILD STORES ITS PARENT AND AN OFFSET, NOT A POSITION. Where it
        // sits is composed (ADR-M10-002), so writing the composed answer would
        // put a second, stale copy of it in the file.
        entry.set("parentViewId", JsonValue::makeString(idToString(view->parentViewId())));
        entry.set("alignmentOffsetMm", JsonValue::makeNumber(view->alignmentOffsetMm()));
        views.add(std::move(entry));
    }
    root.set("views", std::move(views));

    // v36 (M33). THE AUTHORED half of the sheet: what a user drew, as against
    // what a view projected. Projected curves are NOT written -- they are
    // derived, and a file carrying them would be a file with a second, stale
    // answer about what the model looks like.
    JsonValue entities = JsonValue::makeArray();
    for (const DrawingEntity* entity : document.entities()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(entity->id())));
        // v46 (M44). WHICH PAGE, resolved -- a file never says "wherever the
        // first one is", because the reader would have to make the same guess
        // and two guesses is a pair to keep in step.
        item.set("sheetId",
                  JsonValue::makeString(idToString(document.sheetOfObject(entity->id()))));
        item.set("layerId", JsonValue::makeString(idToString(entity->layerId())));
        item.set("color", JsonValue::makeNumber(static_cast<double>(entity->color())));
        item.set("linetype", JsonValue::makeString(entity->linetype()));
        item.set("lineweight",
                 JsonValue::makeNumber(static_cast<double>(entity->lineweight())));
        item.set("kind", JsonValue::makeString(std::string(ShapeName(entity->shape()))));

        const auto point = [](Vec2 at) {
            JsonValue out = JsonValue::makeObject();
            out.set("x", JsonValue::makeNumber(at.x));
            out.set("y", JsonValue::makeNumber(at.y));
            return out;
        };
        std::visit(
            [&](const auto& shape) {
                using T = std::decay_t<decltype(shape)>;
                if constexpr (std::is_same_v<T, DrawPoint>) {
                    item.set("at", point(shape.at));
                } else if constexpr (std::is_same_v<T, DrawLine>) {
                    item.set("a", point(shape.a));
                    item.set("b", point(shape.b));
                } else if constexpr (std::is_same_v<T, DrawCircle>) {
                    item.set("centre", point(shape.centre));
                    item.set("radius", JsonValue::makeNumber(shape.radius));
                } else if constexpr (std::is_same_v<T, DrawArc>) {
                    item.set("centre", point(shape.centre));
                    item.set("radius", JsonValue::makeNumber(shape.radius));
                    // RADIANS, counter-clockwise, start to end -- the one
                    // convention the whole drawing layer uses. Degrees would
                    // be a second unit in a file that has none.
                    item.set("startAngle", JsonValue::makeNumber(shape.startAngle));
                    item.set("endAngle", JsonValue::makeNumber(shape.endAngle));
                } else if constexpr (std::is_same_v<T, DrawEllipse>) {
                    item.set("centre", point(shape.centre));
                    item.set("majorRadius", JsonValue::makeNumber(shape.majorRadius));
                    item.set("minorRadius", JsonValue::makeNumber(shape.minorRadius));
                    item.set("rotation", JsonValue::makeNumber(shape.rotation));
                } else if constexpr (std::is_same_v<T, DrawPolyline>) {
                    JsonValue vertices = JsonValue::makeArray();
                    for (const DrawVertex& vertex : shape.vertices) {
                        JsonValue one = point(vertex.at);
                        // THE BULGE, as DXF stores it. Converting to an angle
                        // here would make a round trip lossy at every vertex.
                        one.set("bulge", JsonValue::makeNumber(vertex.bulge));
                        vertices.add(std::move(one));
                    }
                    item.set("vertices", std::move(vertices));
                    item.set("closed", JsonValue::makeBool(shape.closed));
                } else {
                    item.set("at", point(shape.at));
                    item.set("text", JsonValue::makeString(shape.text));
                    item.set("heightMm", JsonValue::makeNumber(shape.heightMm));
                    item.set("rotation", JsonValue::makeNumber(shape.rotation));
                }
            },
            entity->shape());
        entities.add(std::move(item));
    }
    root.set("entities", std::move(entities));

    // v37 (M34). Styles before dimensions, because a dimension names one --
    // the same order the tables above follow, so a file reads straight
    // through.
    JsonValue styles = JsonValue::makeArray();
    for (const DimensionStyle* style : document.dimensionStyles()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(style->id())));
        item.set("name", JsonValue::makeString(style->name()));
        // PAPER MILLIMETRES, every one of them (see DimensionStyle.h). The
        // measurement is model millimetres and is never in here.
        item.set("textHeightMm", JsonValue::makeNumber(style->textHeightMm()));
        item.set("textGapMm", JsonValue::makeNumber(style->textGapMm()));
        item.set("arrowSizeMm", JsonValue::makeNumber(style->arrowSizeMm()));
        item.set("extensionGapMm", JsonValue::makeNumber(style->extensionGapMm()));
        item.set("extensionOvershootMm",
                 JsonValue::makeNumber(style->extensionOvershootMm()));
        item.set("decimals", JsonValue::makeNumber(static_cast<double>(style->decimals())));
        item.set("suffix", JsonValue::makeString(style->suffix()));
        item.set("overallScale", JsonValue::makeNumber(style->overallScale()));
        styles.add(std::move(item));
    }
    root.set("dimensionStyles", std::move(styles));
    root.set("currentDimensionStyleId",
             JsonValue::makeString(idToString(document.currentDimensionStyleId())));

    // v46 (M44). THE PAGES, in order -- and the order IS the numbering, so
    // "2 / 3" is never written down anywhere.
    JsonValue pages = JsonValue::makeArray();
    for (const SheetPage* page : document.sheetPages()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(page->id())));
        item.set("name", JsonValue::makeString(page->name()));
        item.set("paper", WriteSheet(page->paper()));
        item.set("bindingMm", JsonValue::makeNumber(page->frameMargins().bindingMm));
        item.set("marginMm", JsonValue::makeNumber(page->frameMargins().otherMm));
        item.set("zoneTargetMm", JsonValue::makeNumber(page->frameZoneTargetMm()));
        item.set("frameVisible", JsonValue::makeBool(page->isFrameVisible()));
        pages.add(std::move(item));
    }
    root.set("sheets", std::move(pages));
    root.set("currentSheetId", JsonValue::makeString(idToString(document.currentSheetId())));

    // ONE TITLE BLOCK FOR THE DRAWING (M44). The Sheet row is the only part of
    // it that differs page to page, and that row is derived from where the
    // page sits -- so there is nothing per-page to write.
    JsonValue titleBlock = JsonValue::makeObject();
    titleBlock.set("widthMm", JsonValue::makeNumber(document.titleBlock().widthMm()));
    titleBlock.set("rowHeightMm", JsonValue::makeNumber(document.titleBlock().rowHeightMm()));
    titleBlock.set("visible", JsonValue::makeBool(document.titleBlock().isVisible()));
    JsonValue rows = JsonValue::makeArray();
    for (const TitleBlockField& field : document.titleBlock().fields()) {
        JsonValue row = JsonValue::makeObject();
        row.set("label", JsonValue::makeString(field.label));
        row.set("source", JsonValue::makeString(std::string(toString(field.source))));
        // A DERIVED ROW WRITES NO VALUE. There is nothing to write: what it
        // prints is fetched at draw time, and a value beside it in the file
        // would be a second answer waiting to go stale.
        if (!field.isDerived()) row.set("value", JsonValue::makeString(field.value));
        rows.add(std::move(row));
    }
    titleBlock.set("fields", std::move(rows));
    root.set("titleBlock", std::move(titleBlock));

    JsonValue dimensions = JsonValue::makeArray();
    const auto& anchorToJson = WriteDimensionAnchor;
    for (const DrawingDimension* dimension : document.dimensions()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(dimension->id())));
        // v46 (M44). WHICH PAGE, resolved -- a file never says "wherever the
        // first one is", because the reader would have to make the same guess
        // and two guesses is a pair to keep in step.
        item.set("sheetId",
                  JsonValue::makeString(idToString(document.sheetOfObject(dimension->id()))));
        item.set("kind", JsonValue::makeString(std::string(toString(dimension->kind()))));
        item.set("direction",
                 JsonValue::makeString(std::string(toString(dimension->direction()))));
        item.set("first", anchorToJson(dimension->first()));
        item.set("second", anchorToJson(dimension->second()));
        item.set("lineXMm", JsonValue::makeNumber(dimension->linePositionMm().x));
        item.set("lineYMm", JsonValue::makeNumber(dimension->linePositionMm().y));
        item.set("styleId", JsonValue::makeString(idToString(dimension->styleId())));
        item.set("layerId", JsonValue::makeString(idToString(dimension->layerId())));
        // THE MEASUREMENT IS NOT WRITTEN. It is derived from the anchors, and
        // a file carrying it would hold a second, stale answer about the size
        // of the part -- which is the exact failure a drawing must not have.
        item.set("textOverride", JsonValue::makeString(dimension->textOverride()));
        // v41: the tolerance. A FIT WRITES ITS CODE AND NOT ITS NUMBERS --
        // storing the deviations would put a second answer in the file, and
        // the day the table is corrected every drawing already made would keep
        // the old numbers and look right.
        const DimensionTolerance& tolerance = dimension->tolerance();
        if (tolerance.kind != ToleranceKind::None) {
            JsonValue held = JsonValue::makeObject();
            held.set("kind", JsonValue::makeString(std::string(toString(tolerance.kind))));
            if (tolerance.kind == ToleranceKind::Fit)
                held.set("fit", JsonValue::makeString(tolerance.fitCode));
            if (tolerance.statesNumbers()) {
                held.set("upperMm", JsonValue::makeNumber(tolerance.upperMm));
                held.set("lowerMm", JsonValue::makeNumber(tolerance.lowerMm));
            }
            if (tolerance.decimals >= 0)
                held.set("decimals",
                         JsonValue::makeNumber(static_cast<double>(tolerance.decimals)));
            item.set("tolerance", std::move(held));
        }
        dimensions.add(std::move(item));
    }
    root.set("dimensions", std::move(dimensions));
    // WHAT UNMARKED SIZES MEAN, once, for the whole sheet -- like the
    // projection angle, and for the same reason.
    root.set("generalTolerance",
             JsonValue::makeString(std::string(toString(document.generalToleranceClass()))));

    // v38 (M35). ONLY WHAT A USER DECIDED.
    //
    // The frame's rectangle, its zone labels, and the scale/size/projection
    // rows of the title block are all DERIVED from the sheet and none of them
    // is written. A file carrying them would come back holding an A3 border on
    // a sheet somebody has since made A2 -- and it would look right.

    // v39 (M35.6). WHICH FILE, WHICH COLUMNS, HOW DEEP -- and the stamp, so
    // staleness survives a reopen.
    //
    // THE ROWS ARE NOT WRITTEN. They are counted from the assembly whenever
    // the list is drawn, and a file carrying them would hold a bill of
    // materials the assembly no longer has -- the wrong number being the one
    // somebody orders from.
    JsonValue tables = JsonValue::makeArray();
    for (const BomTable* table : document.bomTables()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(table->id())));
        // v46 (M44). WHICH PAGE, resolved -- a file never says "wherever the
        // first one is", because the reader would have to make the same guess
        // and two guesses is a pair to keep in step.
        item.set("sheetId",
                  JsonValue::makeString(idToString(document.sheetOfObject(table->id()))));
        item.set("name", JsonValue::makeString(table->name()));
        item.set("sourcePath", JsonValue::makeString(table->sourcePath()));
        item.set("xMm", JsonValue::makeNumber(table->positionMm().x));
        item.set("yMm", JsonValue::makeNumber(table->positionMm().y));
        item.set("depth", JsonValue::makeString(std::string(toString(table->depth()))));
        item.set("rowHeightMm", JsonValue::makeNumber(table->rowHeightMm()));
        item.set("growsUpward", JsonValue::makeBool(table->growsUpward()));
        item.set("sourceStamp",
                 JsonValue::makeString(std::to_string(table->sourceStamp())));
        JsonValue columns = JsonValue::makeArray();
        for (const BomColumn column : table->columns())
            columns.add(JsonValue::makeString(std::string(toString(column))));
        item.set("columns", std::move(columns));
        tables.add(std::move(item));
    }
    root.set("bomTables", std::move(tables));

    // v43 (M39). A HOLE TABLE STORES ONE SENTENCE: which view's holes, where
    // the table sits, and which corner the positions are measured from. Not a
    // single row -- rows are counted from the part when anybody asks, so a
    // table can never state hole positions the part no longer has.
    JsonValue holeTables = JsonValue::makeArray();
    for (const HoleTable* table : document.holeTables()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(table->id())));
        // v46 (M44). WHICH PAGE, resolved -- a file never says "wherever the
        // first one is", because the reader would have to make the same guess
        // and two guesses is a pair to keep in step.
        item.set("sheetId",
                  JsonValue::makeString(idToString(document.sheetOfObject(table->id()))));
        item.set("name", JsonValue::makeString(table->name()));
        item.set("viewId", JsonValue::makeString(idToString(table->viewId())));
        item.set("xMm", JsonValue::makeNumber(table->positionMm().x));
        item.set("yMm", JsonValue::makeNumber(table->positionMm().y));
        item.set("datumXMm", JsonValue::makeNumber(table->datumMm().x));
        item.set("datumYMm", JsonValue::makeNumber(table->datumMm().y));
        item.set("rowHeightMm", JsonValue::makeNumber(table->rowHeightMm()));
        JsonValue columns = JsonValue::makeArray();
        for (const HoleColumn column : table->columns())
            columns.add(JsonValue::makeString(std::string(toString(column))));
        item.set("columns", std::move(columns));
        holeTables.add(std::move(item));
    }
    root.set("holeTables", std::move(holeTables));

    // v44 (M41). One array for all three symbols, because they are one object
    // with three bodies. A DATUM'S LETTER IS NOT WRITTEN: it is derived from
    // the order below, and a stored letter would be a second answer that goes
    // stale the first time a datum is deleted.
    JsonValue symbols = JsonValue::makeArray();
    for (const Annotation* annotation : document.annotations()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(annotation->id())));
        // v46 (M44). WHICH PAGE, resolved -- a file never says "wherever the
        // first one is", because the reader would have to make the same guess
        // and two guesses is a pair to keep in step.
        item.set("sheetId",
                  JsonValue::makeString(idToString(document.sheetOfObject(annotation->id()))));
        item.set("xMm", JsonValue::makeNumber(annotation->positionMm().x));
        item.set("yMm", JsonValue::makeNumber(annotation->positionMm().y));
        item.set("layerId", JsonValue::makeString(idToString(annotation->layerId())));
        item.set("anchor", WriteDimensionAnchor(annotation->anchor()));

        // EVERY BODY, OR IT DOES NOT BUILD (M47).
        //
        // This was an if / else-if chain ending in "otherwise, write a datum".
        // Adding a fifth body to the variant would have saved a weld symbol as
        // a datum feature: a file that writes, loads and opens, with a welding
        // instruction replaced by a letter. Nothing would have thrown, and the
        // drawing would have looked finished.
        //
        // A visit with no default cannot do that. The next body added to
        // AnnotationBody is a compile error here until somebody writes it
        // down, which is the difference between a rule that is tested and a
        // rule that cannot be broken.
        std::visit(
            [&item](const auto& body) {
                using Body = std::decay_t<decltype(body)>;
                if constexpr (std::is_same_v<Body, SurfaceFinishSpec>) {
                    item.set("kind", JsonValue::makeString("surface-finish"));
                    item.set("symbol",
                             JsonValue::makeString(std::string(toString(body.symbol))));
                    item.set("raMicrometres", JsonValue::makeNumber(body.raMicrometres));
                    item.set("raLowerMicrometres",
                             JsonValue::makeNumber(body.raLowerMicrometres));
                    item.set("process", JsonValue::makeString(body.process));
                    item.set("lay", JsonValue::makeString(std::string(toString(body.lay))));
                    item.set("machiningAllowanceMm",
                             JsonValue::makeNumber(body.machiningAllowanceMm));
                    item.set("allAround", JsonValue::makeBool(body.allAround));
                } else if constexpr (std::is_same_v<Body, FeatureControlFrameSpec>) {
                    item.set("kind", JsonValue::makeString("frame"));
                    item.set("characteristic",
                             JsonValue::makeString(std::string(toString(body.characteristic))));
                    item.set("toleranceMm", JsonValue::makeNumber(body.toleranceMm));
                    item.set("diametricZone", JsonValue::makeBool(body.diametricZone));
                    item.set("condition",
                             JsonValue::makeString(std::string(toString(body.condition))));
                    JsonValue datums = JsonValue::makeArray();
                    for (const DatumReference& reference : body.datums) {
                        JsonValue one = JsonValue::makeObject();
                        one.set("datumId",
                                JsonValue::makeString(idToString(reference.datumId)));
                        one.set("condition", JsonValue::makeString(
                                                 std::string(toString(reference.condition))));
                        datums.add(std::move(one));
                    }
                    item.set("datums", std::move(datums));
                } else if constexpr (std::is_same_v<Body, BalloonSpec>) {
                    item.set("kind", JsonValue::makeString("balloon"));
                    // WHICH LIST AND WHICH ROW, and not the number the row
                    // currently carries -- the number is the list's answer,
                    // asked for again on every repaint.
                    item.set("tableId", JsonValue::makeString(idToString(body.tableId)));
                    item.set("sourceFile", JsonValue::makeString(body.sourceFile));
                    item.set("partName", JsonValue::makeString(body.partName));
                } else if constexpr (std::is_same_v<Body, DatumFeatureSpec>) {
                    item.set("kind", JsonValue::makeString("datum"));
                    item.set("note", JsonValue::makeString(body.note));
                } else if constexpr (std::is_same_v<Body, WeldSymbolSpec>) {
                    item.set("kind", JsonValue::makeString("weld"));
                    // THE SIDE IS THE STRUCTURE, on disk as in memory: a bead
                    // is written under the name of the side it is on, and
                    // there is no field that could name a different one.
                    if (body.arrowSide.has_value())
                        item.set("arrowSide", WriteWeldBead(*body.arrowSide));
                    if (body.otherSide.has_value())
                        item.set("otherSide", WriteWeldBead(*body.otherSide));
                    item.set("allAround", JsonValue::makeBool(body.allAround));
                    item.set("fieldWeld", JsonValue::makeBool(body.fieldWeld));
                    item.set("staggered", JsonValue::makeBool(body.staggered));
                    item.set("tail", JsonValue::makeString(body.tail));
                } else {
                    static_assert(kNoSaverFor<Body>,
                                  "a new annotation body has to be written here -- a "
                                  "fall-through would save it as something else");
                }
            },
            annotation->body());
        symbols.add(std::move(item));
    }
    root.set("symbols", std::move(symbols));

    // v48 (M48). THE DRAWING'S HISTORY, letters included.
    //
    // This is the one list in this file written down as it stands rather than
    // derived. Revision.h has the argument; the short form is that a balloon's
    // number points at a row that exists now, and Rev C is a fact somebody
    // else's purchase order already cites.
    //
    // THE ORDER IS THE MEANING: the last row is what the drawing is issued at,
    // and the title block prints it from here.
    JsonValue revisions = JsonValue::makeArray();
    for (const Revision& revision : document.revisions()) {
        JsonValue item = JsonValue::makeObject();
        item.set("letter", JsonValue::makeString(revision.letter));
        item.set("description", JsonValue::makeString(revision.description));
        item.set("date", JsonValue::makeString(revision.date));
        item.set("by", JsonValue::makeString(revision.by));
        revisions.add(std::move(item));
    }
    root.set("revisions", std::move(revisions));

    // The table that SHOWS it, which holds none of it.
    JsonValue revisionTables = JsonValue::makeArray();
    for (const RevisionTable* table : document.revisionTables()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(table->id())));
        item.set("sheetId",
                 JsonValue::makeString(idToString(document.sheetOfObject(table->id()))));
        item.set("name", JsonValue::makeString(table->name()));
        item.set("xMm", JsonValue::makeNumber(table->positionMm().x));
        item.set("yMm", JsonValue::makeNumber(table->positionMm().y));
        item.set("widthMm", JsonValue::makeNumber(table->widthMm()));
        item.set("rowHeightMm", JsonValue::makeNumber(table->rowHeightMm()));
        item.set("layerId", JsonValue::makeString(idToString(table->layerId())));
        revisionTables.add(std::move(item));
    }
    root.set("revisionTables", std::move(revisionTables));

    // v40 (M36). A component stores a SENTENCE -- which symbol, where, which
    // way round -- and not the geometry (ADR-M22-003): copying the shapes in
    // would mean a corrected symbol never reaches the drawings already made.
    JsonValue components = JsonValue::makeArray();
    for (const SymbolPlacement* symbol : document.symbols()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(symbol->id())));
        item.set("tag", JsonValue::makeString(symbol->tag()));
        item.set("symbol", JsonValue::makeString(symbol->symbolName()));
        item.set("xMm", JsonValue::makeNumber(symbol->positionMm().x));
        item.set("yMm", JsonValue::makeNumber(symbol->positionMm().y));
        item.set("rotationRad", JsonValue::makeNumber(symbol->rotationRad()));
        item.set("mirrored", JsonValue::makeBool(symbol->isMirrored()));
        item.set("layerId", JsonValue::makeString(idToString(symbol->layerId())));
        components.add(std::move(item));
    }
    root.set("components", std::move(components));

    // THE NETLIST IS NOT WRITTEN. It is derived from these wires and those
    // components every time it is asked for, and a file carrying it would
    // hold a circuit the drawing no longer shows -- and what gets built is the
    // netlist. The wire LABELS are written, because a name is something a user
    // typed and the nets have nowhere else to keep one.
    JsonValue wires = JsonValue::makeArray();
    for (const WireEntity* wire : document.wires()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(wire->id())));
        item.set("label", JsonValue::makeString(wire->label()));
        item.set("layerId", JsonValue::makeString(idToString(wire->layerId())));
        JsonValue points = JsonValue::makeArray();
        for (const Vec2 point : wire->pointsMm()) {
            JsonValue at = JsonValue::makeObject();
            at.set("x", JsonValue::makeNumber(point.x));
            at.set("y", JsonValue::makeNumber(point.y));
            points.add(std::move(at));
        }
        item.set("points", std::move(points));
        wires.add(std::move(item));
    }
    root.set("wires", std::move(wires));
    return root;
}

struct LinetypeData {
    ObjectId id = kInvalidObjectId;
    std::string name;
    std::string description;
    std::vector<double> pattern;
};

struct LayerData {
    ObjectId id = kInvalidObjectId;
    std::string name;
    int color = 7;
    std::string linetype;
    bool on = true;
    bool frozen = false;
    bool locked = false;
    int lineweight = kLineweightDefault;
};

struct ViewData {
    ObjectId id = kInvalidObjectId;
    std::string name;
    std::string sourcePath;
    std::string bodyName;
    ViewDirection direction = ViewDirection::Front;
    Vec2 positionMm{};
    DrawingScale scale{1, 1};
    bool ownScale = false;
    bool showHidden = true;
    bool showTangent = false;
    // v42 (M38): the cut LINE, on the parent's page. The plane it makes is
    // worked out at every recompute, so it is not in the file.
    bool sectionActive = false;
    Vec2 sectionFromMm{};
    Vec2 sectionToMm{};
    int sectionArrowSide = 1;
    // v52 (M53): the blank, rather than a projection of the folded part.
    bool flatPattern = false;
    // v49 (M49): the circle on the parent, for the same reason.
    bool detailActive = false;
    Vec2 detailCentreMm{};
    double detailRadiusMm = 0.0;
    // v50 (M50): the span, for the same reason.
    BreakSpan breakSpan{};
    ObjectId parentViewId = kInvalidObjectId;
    double alignmentOffsetMm = 0.0;
};

} // namespace

SaveResult saveDrawingDocument(const DrawingDocument& document, std::ostream& out) {
    if (const SaveResult invalid = validateSaveable(document); !invalid) return invalid;
    out << writeJson(toJson(document)) << '\n';
    if (!out.good())
        return SaveResult{SerializationError::IoError, "failed to write document to stream"};
    return SaveResult{};
}

DrawingLoadResult loadDrawingDocument(std::istream& in) {
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad())
        return loadFailure(SerializationError::IoError, "failed to read document from stream");

    JsonParseError parseError;
    const JsonValue root = parseJson(buffer.str(), parseError);
    if (!parseError.ok) {
        std::ostringstream message;
        message << "malformed JSON at line " << parseError.line << ", column "
                << parseError.column << ": " << parseError.message;
        return loadFailure(SerializationError::MalformedJson, message.str());
    }
    if (root.type() != JsonType::Object)
        return loadFailure(SerializationError::MalformedJson,
                           "top-level JSON value is not an object");

    FieldError err;
    ObjectId documentId = kInvalidObjectId;
    std::string documentName;
    if (!docjson::readHeader(root, "Drawing", err, documentId, documentName))
        return loadFailure(err.error, err.message);

    std::unordered_set<ObjectId> claimed{documentId};
    const docjson::IdClaim registerId = [&claimed](ObjectId id, const std::string& context,
                                                   FieldError& error) {
        if (!claimed.insert(id).second) {
            error = fieldError(SerializationError::DuplicateId,
                               context + ": id " + idToString(id) + " is used twice");
            return false;
        }
        return true;
    };

    std::vector<docjson::FrameData> frameData;
    std::vector<docjson::ConnectorData> connectorData;
    if (!docjson::readFrames(root, registerId, err, frameData))
        return loadFailure(err.error, err.message);
    if (!docjson::readConnectors(root, registerId, err, connectorData))
        return loadFailure(err.error, err.message);

    // --- The paper -----------------------------------------------------------
    SheetSize sheetSize = SheetSize::A3;
    SheetOrientation sheetOrientation = SheetOrientation::Landscape;
    DrawingScale sheetScale{1, 1};
    ProjectionAngle projectionAngle = ProjectionAngle::First;
    double customWidthMm = 0.0;
    double customHeightMm = 0.0;
    // WHICH PAGE EACH OBJECT IS ON, collected as the objects are read and
    // applied in ONE place once the pages exist. Applying it per object as it
    // is restored would mean six restore signatures growing a parameter, and
    // six chances for the next kind of object to be forgotten.
    std::vector<std::pair<ObjectId, ObjectId>> objectSheets;
    const auto noteSheet = [&objectSheets](const JsonValue& entry, ObjectId objectId) {
        if (const JsonValue* value = entry.find("sheetId"))
            if (value->type() == JsonType::String)
                if (const auto parsed = idFromString(value->asString()))
                    objectSheets.emplace_back(objectId, *parsed);
    };

    if (const JsonValue* paper = root.find("sheet")) {
        // A DRAWING FROM BEFORE M44: one sheet, at the top level. Read by the
        // SAME function a page's paper is, so the two cannot come to disagree
        // about what a sheet is -- and then unpacked into the locals the rest
        // of this loader already uses.
        const std::string context = "sheet";
        if (paper->type() != JsonType::Object)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'sheet' is not an object");
        Sheet legacy;
        if (!ReadSheet(*paper, context, err, legacy))
            return loadFailure(err.error, err.message);
        sheetSize = legacy.size();
        sheetOrientation = legacy.orientation();
        sheetScale = legacy.scale();
        projectionAngle = legacy.projectionAngle();
        customWidthMm = legacy.customWidthMm();
        customHeightMm = legacy.customHeightMm();
    }

    // --- Linetypes, then layers, then views ----------------------------------
    std::vector<LinetypeData> linetypeData;
    if (const JsonValue* field = root.find("linetypes")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'linetypes' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "linetypes[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            LinetypeData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
            if (name == nullptr) return loadFailure(err.error, err.message);
            if (name->asString().empty())
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": a linetype has no name");
            one.name = name->asString();

            if (const JsonValue* description = entry.find("description")) {
                if (description->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'description' is not a string");
                one.description = description->asString();
            }
            const JsonValue* pattern =
                requireField(entry, "pattern", JsonType::Array, context, err);
            if (pattern == nullptr) return loadFailure(err.error, err.message);
            for (const JsonValue& segment : pattern->items()) {
                if (segment.type() != JsonType::Number)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a dash pattern is a list of numbers");
                one.pattern.push_back(segment.asNumber());
            }
            linetypeData.push_back(std::move(one));
        }
    }

    std::vector<LayerData> layerData;
    if (const JsonValue* field = root.find("layers")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'layers' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "layers[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            LayerData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
            if (name == nullptr) return loadFailure(err.error, err.message);
            if (name->asString().empty())
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": a layer has no name");
            one.name = name->asString();

            const JsonValue* color = requireField(entry, "color", JsonType::Number, context, err);
            if (color == nullptr) return loadFailure(err.error, err.message);
            one.color = static_cast<int>(color->asNumber());

            const JsonValue* linetype =
                requireField(entry, "linetype", JsonType::String, context, err);
            if (linetype == nullptr) return loadFailure(err.error, err.message);
            // THE LINETYPE HAS TO BE IN THIS FILE. Checked here rather than
            // left to addLayer's throw, because a loader that throws is a
            // loader a caller cannot use.
            bool linetypeIsHere = false;
            for (const LinetypeData& candidate : linetypeData)
                if (candidate.name == linetype->asString()) linetypeIsHere = true;
            if (!linetypeIsHere)
                return loadFailure(SerializationError::UnknownDependencyId,
                                   context + ": linetype '" + linetype->asString() +
                                       "' is not in this document");
            one.linetype = linetype->asString();

            const auto readFlag = [&](const char* key, bool& into) -> bool {
                const JsonValue* flag = entry.find(key);
                if (flag == nullptr) return true; // absent keeps the default
                if (flag->type() != JsonType::Bool) return false;
                into = flag->asBool();
                return true;
            };
            if (!readFlag("on", one.on) || !readFlag("frozen", one.frozen) ||
                !readFlag("locked", one.locked))
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": a layer flag is not a boolean");
            if (const JsonValue* lineweight = entry.find("lineweight")) {
                if (lineweight->type() != JsonType::Number)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'lineweight' is not a number");
                one.lineweight = static_cast<int>(lineweight->asNumber());
            }
            layerData.push_back(std::move(one));
        }
    }

    std::vector<ViewData> viewData;
    if (const JsonValue* field = root.find("views")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'views' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "views[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            ViewData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
            if (name == nullptr) return loadFailure(err.error, err.message);
            one.name = name->asString();

            const JsonValue* source =
                requireField(entry, "sourcePath", JsonType::String, context, err);
            if (source == nullptr) return loadFailure(err.error, err.message);
            if (source->asString().empty())
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": a view names no model file");
            one.sourcePath = source->asString();

            const JsonValue* bodyName =
                requireField(entry, "bodyName", JsonType::String, context, err);
            if (bodyName == nullptr) return loadFailure(err.error, err.message);
            one.bodyName = bodyName->asString();

            const JsonValue* direction =
                requireField(entry, "direction", JsonType::String, context, err);
            if (direction == nullptr) return loadFailure(err.error, err.message);
            const auto parsedDirection = viewDirectionFromString(direction->asString());
            if (!parsedDirection.has_value())
                return loadFailure(SerializationError::InvalidEnumValue,
                                   context + ": unknown view direction '" +
                                       direction->asString() + "'");
            one.direction = *parsedDirection;

            const JsonValue* x = requireField(entry, "xMm", JsonType::Number, context, err);
            if (x == nullptr) return loadFailure(err.error, err.message);
            const JsonValue* y = requireField(entry, "yMm", JsonType::Number, context, err);
            if (y == nullptr) return loadFailure(err.error, err.message);
            one.positionMm = Vec2{x->asNumber(), y->asNumber()};

            if (const JsonValue* ownScale = entry.find("ownScale")) {
                if (ownScale->type() != JsonType::Bool)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'ownScale' is not a boolean");
                one.ownScale = ownScale->asBool();
            }
            const JsonValue* scale = requireField(entry, "scale", JsonType::String, context, err);
            if (scale == nullptr) return loadFailure(err.error, err.message);
            if (!ParseDrawingScale(scale->asString(), one.scale))
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": '" + scale->asString() +
                                       "' is not a scale like 1:2");
            // ON THE PAPER THIS FILE DESCRIBES. Checked against a sheet built
            // from what was just read, not against the document's default --
            // the document does not exist yet, and validating against A3 when
            // the file says A1 would refuse perfectly good drawings.
            //
            // Found by M32_SER_005: the save side refused an off-sheet view
            // and the load side did not, which is half of ADR-M3-008.
            //
            // A CHILD IS NOT CHECKED, because it has no position of its own --
            // its place is composed from its parent's, and its stored one is
            // the zero it was constructed with.
            if (one.parentViewId == kInvalidObjectId) {
                Sheet paper(sheetSize, sheetOrientation);
                if (sheetSize == SheetSize::Custom)
                    paper.setCustomSize(customWidthMm, customHeightMm);
                if (one.positionMm.x < 0.0 || one.positionMm.y < 0.0 ||
                    one.positionMm.x > paper.widthMm() || one.positionMm.y > paper.heightMm())
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": this view sits off the sheet");
            }
            if (const JsonValue* parent = entry.find("parentViewId")) {
                if (parent->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'parentViewId' is not a string");
                const auto parsed = idFromString(parent->asString());
                if (!parsed.has_value())
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": 'parentViewId' is not a valid ObjectId");
                one.parentViewId = *parsed;
            }
            if (const JsonValue* offset = entry.find("alignmentOffsetMm")) {
                if (offset->type() != JsonType::Number)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'alignmentOffsetMm' is not a number");
                one.alignmentOffsetMm = offset->asNumber();
            }

            const auto readViewFlag = [&](const char* key, bool& into) -> bool {
                const JsonValue* flag = entry.find(key);
                if (flag == nullptr) return true; // absent keeps the default
                if (flag->type() != JsonType::Bool) return false;
                into = flag->asBool();
                return true;
            };
            if (!readViewFlag("showHiddenLines", one.showHidden) ||
                !readViewFlag("showTangentEdges", one.showTangent))
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": a view flag is not a boolean");
            if (const JsonValue* cut = entry.find("section")) {
                if (cut->type() != JsonType::Object)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'section' is not an object");
                const JsonValue* fx =
                    requireField(*cut, "fromXMm", JsonType::Number, context, err);
                const JsonValue* fy =
                    requireField(*cut, "fromYMm", JsonType::Number, context, err);
                const JsonValue* tx =
                    requireField(*cut, "toXMm", JsonType::Number, context, err);
                const JsonValue* ty =
                    requireField(*cut, "toYMm", JsonType::Number, context, err);
                if (fx == nullptr || fy == nullptr || tx == nullptr || ty == nullptr)
                    return loadFailure(err.error, err.message);
                one.sectionFromMm = Vec2{fx->asNumber(), fy->asNumber()};
                one.sectionToMm = Vec2{tx->asNumber(), ty->asNumber()};
                // A CUT LINE OF NO LENGTH CUTS NOTHING, and a section with one
                // would project the whole part and look entirely ordinary.
                if (std::fabs(one.sectionToMm.x - one.sectionFromMm.x) < 1e-9 &&
                    std::fabs(one.sectionToMm.y - one.sectionFromMm.y) < 1e-9)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": this section's cut line has no length");
                if (const JsonValue* side = cut->find("arrowSide")) {
                    if (side->type() != JsonType::Number)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'arrowSide' is not a number");
                    one.sectionArrowSide = side->asNumber() >= 0.0 ? 1 : -1;
                }
                one.sectionActive = true;
            }
            if (const JsonValue* flat = entry.find("flatPattern")) {
                if (flat->type() != JsonType::Bool)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'flatPattern' is not a boolean");
                one.flatPattern = flat->asBool();
            }
            if (const JsonValue* circle = entry.find("detail")) {
                if (circle->type() != JsonType::Object)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'detail' is not an object");
                const JsonValue* cx =
                    requireField(*circle, "centreXMm", JsonType::Number, context, err);
                const JsonValue* cy =
                    requireField(*circle, "centreYMm", JsonType::Number, context, err);
                const JsonValue* r =
                    requireField(*circle, "radiusMm", JsonType::Number, context, err);
                if (cx == nullptr || cy == nullptr || r == nullptr)
                    return loadFailure(err.error, err.message);
                one.detailCentreMm = Vec2{cx->asNumber(), cy->asNumber()};
                one.detailRadiusMm = r->asNumber();
                // A CIRCLE OF NO SIZE ENCLOSES NOTHING, and a detail with one
                // would project the whole part at the enlarged scale and look
                // like a view somebody meant to put there. The saver refuses
                // it, so the loader refuses it (ADR-M3-008).
                if (!(one.detailRadiusMm > 1e-9))
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": this detail's circle has no size");
                // AND IT IS NOT ALSO A SECTION. One view cannot be both: the
                // recompute would cut and crop with two ideas of what its
                // camera is, and the picture would be of neither.
                if (one.sectionActive)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a view is either a section or a detail, "
                                                 "not both");
                one.detailActive = true;
            }
            if (const JsonValue* span = entry.find("break")) {
                if (span->type() != JsonType::Object)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'break' is not an object");
                const JsonValue* from =
                    requireField(*span, "fromMm", JsonType::Number, context, err);
                const JsonValue* to =
                    requireField(*span, "toMm", JsonType::Number, context, err);
                if (from == nullptr || to == nullptr) return loadFailure(err.error, err.message);
                one.breakSpan.active = true;
                one.breakSpan.fromMm = from->asNumber();
                one.breakSpan.toMm = to->asNumber();
                if (const JsonValue* horizontal = span->find("horizontal"))
                    if (horizontal->type() == JsonType::Bool)
                        one.breakSpan.horizontal = horizontal->asBool();
                if (const JsonValue* gap = span->find("gapMm"))
                    if (gap->type() == JsonType::Number) one.breakSpan.gapMm = gap->asNumber();
                // WHAT THE SAVER REFUSES, THE LOADER REFUSES, by calling the
                // same function (ADR-M3-008). The part's extent is not back
                // yet -- the view has not been projected -- so this is the
                // half of the rule that does not need it: a break that removes
                // nothing, or leaves less than no gap.
                const std::string why = WhyBreakRefused(one.breakSpan, 0.0, 0.0);
                if (!why.empty())
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": " + why);
            }
            viewData.push_back(std::move(one));
        }
    }

    // EVERY PARENT IS A VIEW IN THIS FILE, and no chain of them closes a loop.
    //
    // Checked after all the views are read, because a child may legitimately
    // precede its parent in the array -- the file is in creation order, and a
    // view can be projected off one made later only if somebody reorders it by
    // hand, which is exactly the case this has to survive.
    //
    // A LOOP WOULD HANG viewPositionMm's walk. That walk is bounded so it
    // cannot spin for ever, but a bounded walk over a cyclic chain returns a
    // position nobody can explain -- so the file is refused instead.
    for (std::size_t i = 0; i < viewData.size(); ++i) {
        if (viewData[i].parentViewId == kInvalidObjectId) continue;
        const std::string context = "views[" + std::to_string(i) + "]";
        std::size_t walk = i;
        for (std::size_t step = 0; step <= viewData.size(); ++step) {
            const ObjectId parent = viewData[walk].parentViewId;
            if (parent == kInvalidObjectId) break;
            std::size_t found = viewData.size();
            for (std::size_t j = 0; j < viewData.size(); ++j)
                if (viewData[j].id == parent) found = j;
            if (found == viewData.size())
                return loadFailure(SerializationError::UnknownDependencyId,
                                   context + ": parentViewId " + idToString(parent) +
                                       " is not a view in this document");
            if (found == i)
                return loadFailure(SerializationError::InvalidDependency,
                                   context + ": this view is projected from itself");
            walk = found;
            if (step == viewData.size())
                return loadFailure(SerializationError::InvalidDependency,
                                   context + ": these views are projected from each other in "
                                             "a loop");
        }
    }

    struct EntityData {
        ObjectId id = kInvalidObjectId;
        DrawShape shape;
        ObjectId layerId = kInvalidObjectId;
        int color = kColorByLayer;
        std::string linetype = "BYLAYER";
        int lineweight = kLineweightByLayer;
    };
    std::vector<EntityData> entityData;
    if (const JsonValue* field = root.find("entities")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'entities' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "entities[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            EntityData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* layerField =
                requireField(entry, "layerId", JsonType::String, context, err);
            if (layerField == nullptr) return loadFailure(err.error, err.message);
            const auto layerId = idFromString(layerField->asString());
            if (!layerId.has_value())
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": 'layerId' is not a valid ObjectId");
            bool layerIsHere = false;
            for (const LayerData& candidate : layerData)
                if (candidate.id == *layerId) layerIsHere = true;
            if (!layerIsHere)
                return loadFailure(SerializationError::UnknownDependencyId,
                                   context + ": layerId " + idToString(*layerId) +
                                       " is not a layer in this document");
            one.layerId = *layerId;

            if (const JsonValue* color = entry.find("color")) {
                if (color->type() != JsonType::Number)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'color' is not a number");
                one.color = static_cast<int>(color->asNumber());
            }
            if (const JsonValue* linetype = entry.find("linetype")) {
                if (linetype->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'linetype' is not a string");
                const std::string& name = linetype->asString();
                if (name != "BYLAYER" && name != "BYBLOCK") {
                    bool isHere = false;
                    for (const LinetypeData& candidate : linetypeData)
                        if (candidate.name == name) isHere = true;
                    if (!isHere)
                        return loadFailure(SerializationError::UnknownDependencyId,
                                           context + ": linetype '" + name +
                                               "' is not in this document");
                }
                one.linetype = name;
            }
            if (const JsonValue* lineweight = entry.find("lineweight")) {
                if (lineweight->type() != JsonType::Number)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'lineweight' is not a number");
                one.lineweight = static_cast<int>(lineweight->asNumber());
            }

            const JsonValue* kind = requireField(entry, "kind", JsonType::String, context, err);
            if (kind == nullptr) return loadFailure(err.error, err.message);

            const auto readPoint = [&](const char* key, Vec2& into) -> bool {
                const JsonValue* at = requireField(entry, key, JsonType::Object, context, err);
                if (at == nullptr) return false;
                const JsonValue* x = requireField(*at, "x", JsonType::Number, context, err);
                const JsonValue* y = requireField(*at, "y", JsonType::Number, context, err);
                if (x == nullptr || y == nullptr) return false;
                into = Vec2{x->asNumber(), y->asNumber()};
                return true;
            };
            const auto readNumber = [&](const char* key, double& into) -> bool {
                const JsonValue* value = requireField(entry, key, JsonType::Number, context, err);
                if (value == nullptr) return false;
                into = value->asNumber();
                return true;
            };

            const std::string& shapeKind = kind->asString();
            if (shapeKind == "Point") {
                DrawPoint shape;
                if (!readPoint("at", shape.at)) return loadFailure(err.error, err.message);
                one.shape = shape;
            } else if (shapeKind == "Line") {
                DrawLine shape;
                if (!readPoint("a", shape.a) || !readPoint("b", shape.b))
                    return loadFailure(err.error, err.message);
                one.shape = shape;
            } else if (shapeKind == "Circle") {
                DrawCircle shape;
                if (!readPoint("centre", shape.centre) || !readNumber("radius", shape.radius))
                    return loadFailure(err.error, err.message);
                if (!(shape.radius > 0.0))
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a circle has no radius");
                one.shape = shape;
            } else if (shapeKind == "Arc") {
                DrawArc shape;
                if (!readPoint("centre", shape.centre) || !readNumber("radius", shape.radius) ||
                    !readNumber("startAngle", shape.startAngle) ||
                    !readNumber("endAngle", shape.endAngle))
                    return loadFailure(err.error, err.message);
                if (!(shape.radius > 0.0))
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": an arc has no radius");
                one.shape = shape;
            } else if (shapeKind == "Ellipse") {
                DrawEllipse shape;
                if (!readPoint("centre", shape.centre) ||
                    !readNumber("majorRadius", shape.majorRadius) ||
                    !readNumber("minorRadius", shape.minorRadius) ||
                    !readNumber("rotation", shape.rotation))
                    return loadFailure(err.error, err.message);
                // THE MAJOR AXIS IS THE LONGER ONE, by definition. A file that
                // says otherwise would draw an ellipse turned ninety degrees
                // from the one it describes.
                if (!(shape.majorRadius >= shape.minorRadius) || !(shape.minorRadius > 0.0))
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": an ellipse's major axis is not its longer "
                                                 "one");
                one.shape = shape;
            } else if (shapeKind == "Polyline") {
                DrawPolyline shape;
                const JsonValue* vertices =
                    requireField(entry, "vertices", JsonType::Array, context, err);
                if (vertices == nullptr) return loadFailure(err.error, err.message);
                for (const JsonValue& vertex : vertices->items()) {
                    if (vertex.type() != JsonType::Object)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": a vertex is not an object");
                    const JsonValue* x = requireField(vertex, "x", JsonType::Number, context, err);
                    const JsonValue* y = requireField(vertex, "y", JsonType::Number, context, err);
                    if (x == nullptr || y == nullptr) return loadFailure(err.error, err.message);
                    double bulge = 0.0;
                    if (const JsonValue* value = vertex.find("bulge")) {
                        if (value->type() != JsonType::Number)
                            return loadFailure(SerializationError::InvalidFieldType,
                                               context + ": a bulge is not a number");
                        bulge = value->asNumber();
                    }
                    shape.vertices.push_back(
                        DrawVertex{Vec2{x->asNumber(), y->asNumber()}, bulge});
                }
                if (const JsonValue* closed = entry.find("closed")) {
                    if (closed->type() != JsonType::Bool)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'closed' is not a boolean");
                    shape.closed = closed->asBool();
                }
                one.shape = shape;
            } else if (shapeKind == "Text") {
                DrawText shape;
                const JsonValue* text =
                    requireField(entry, "text", JsonType::String, context, err);
                if (text == nullptr) return loadFailure(err.error, err.message);
                shape.text = text->asString();
                if (!readPoint("at", shape.at) || !readNumber("heightMm", shape.heightMm) ||
                    !readNumber("rotation", shape.rotation))
                    return loadFailure(err.error, err.message);
                if (!(shape.heightMm > 0.0))
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": text with no height cannot be read");
                one.shape = shape;
            } else {
                return loadFailure(SerializationError::InvalidEnumValue,
                                   context + ": unknown entity kind '" + shapeKind + "'");
            }
            entityData.push_back(std::move(one));
        }
    }

    struct StyleData {
        ObjectId id = kInvalidObjectId;
        std::string name;
        double textHeightMm = 3.5;
        double textGapMm = 0.8;
        double arrowSizeMm = 3.5;
        double extensionGapMm = 1.5;
        double extensionOvershootMm = 2.0;
        int decimals = 2;
        std::string suffix;
        double overallScale = 1.0;
    };
    std::vector<StyleData> styleData;
    if (const JsonValue* field = root.find("dimensionStyles")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'dimensionStyles' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "dimensionStyles[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            StyleData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);
            const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
            if (name == nullptr) return loadFailure(err.error, err.message);
            if (name->asString().empty())
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": a dimension style has no name");
            one.name = name->asString();

            const auto readOptional = [&](const char* key, double& into) -> bool {
                const JsonValue* value = entry.find(key);
                if (value == nullptr) return true;
                if (value->type() != JsonType::Number) return false;
                into = value->asNumber();
                return true;
            };
            if (!readOptional("textHeightMm", one.textHeightMm) ||
                !readOptional("textGapMm", one.textGapMm) ||
                !readOptional("arrowSizeMm", one.arrowSizeMm) ||
                !readOptional("extensionGapMm", one.extensionGapMm) ||
                !readOptional("extensionOvershootMm", one.extensionOvershootMm) ||
                !readOptional("overallScale", one.overallScale))
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": a style measurement is not a number");
            // A ZERO TEXT HEIGHT IS TEXT NOBODY CAN READ, and it would be
            // found at plot time -- a long way from this file.
            if (!(one.textHeightMm > 0.0) || !(one.arrowSizeMm > 0.0) ||
                !(one.overallScale > 0.0))
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": a style with no text height, arrow or scale "
                                             "draws nothing anybody can read");
            if (const JsonValue* decimals = entry.find("decimals")) {
                if (decimals->type() != JsonType::Number)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'decimals' is not a number");
                one.decimals = static_cast<int>(decimals->asNumber());
                if (one.decimals < 0 || one.decimals > 9)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a decimal count past nine prints digits "
                                                 "that are not measurements");
            }
            if (const JsonValue* suffix = entry.find("suffix")) {
                if (suffix->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'suffix' is not a string");
                one.suffix = suffix->asString();
            }
            styleData.push_back(std::move(one));
        }
    }

    struct DimensionData {
        ObjectId id = kInvalidObjectId;
        DimensionKind kind = DimensionKind::Linear;
        LinearDirection direction = LinearDirection::Aligned;
        DimensionAnchor first;
        DimensionAnchor second;
        Vec2 linePositionMm{};
        ObjectId styleId = kInvalidObjectId;
        ObjectId layerId = kInvalidObjectId;
        std::string textOverride;
        DimensionTolerance tolerance;
    };
    std::vector<DimensionData> dimensionData;
    if (const JsonValue* field = root.find("dimensions")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'dimensions' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "dimensions[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            DimensionData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* kind = requireField(entry, "kind", JsonType::String, context, err);
            if (kind == nullptr) return loadFailure(err.error, err.message);
            if (kind->asString() == "Linear") one.kind = DimensionKind::Linear;
            else if (kind->asString() == "Radius") one.kind = DimensionKind::Radius;
            else if (kind->asString() == "Diameter") one.kind = DimensionKind::Diameter;
            else if (kind->asString() == "Angular") one.kind = DimensionKind::Angular;
            else
                return loadFailure(SerializationError::InvalidEnumValue,
                                   context + ": unknown dimension kind '" + kind->asString() +
                                       "'");
            if (const JsonValue* direction = entry.find("direction")) {
                if (direction->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'direction' is not a string");
                if (direction->asString() == "Aligned")
                    one.direction = LinearDirection::Aligned;
                else if (direction->asString() == "Horizontal")
                    one.direction = LinearDirection::Horizontal;
                else if (direction->asString() == "Vertical")
                    one.direction = LinearDirection::Vertical;
                else
                    return loadFailure(SerializationError::InvalidEnumValue,
                                       context + ": unknown direction '" +
                                           direction->asString() + "'");
            }

            const auto readAnchor = [&](const char* key, DimensionAnchor& into) {
                return ReadDimensionAnchor(entry, key, context, err, into);
            };
            if (!readAnchor("first", one.first) || !readAnchor("second", one.second))
                // `readAnchor` fills `err` for a missing or mistyped field and
                // returns false with it untouched for an unknown anchor kind,
                // so the fallback message covers the second case rather than
                // reporting an empty one.
                return loadFailure(err.message.empty() ? SerializationError::InvalidEnumValue
                                                       : err.error,
                                   err.message.empty()
                                       ? context + ": an anchor names a kind this format does "
                                                   "not know"
                                       : err.message);

            const JsonValue* x = requireField(entry, "lineXMm", JsonType::Number, context, err);
            const JsonValue* y = requireField(entry, "lineYMm", JsonType::Number, context, err);
            if (x == nullptr || y == nullptr) return loadFailure(err.error, err.message);
            one.linePositionMm = Vec2{x->asNumber(), y->asNumber()};

            const JsonValue* styleField =
                requireField(entry, "styleId", JsonType::String, context, err);
            if (styleField == nullptr) return loadFailure(err.error, err.message);
            const auto styleId = idFromString(styleField->asString());
            if (!styleId.has_value())
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": 'styleId' is not a valid ObjectId");
            bool styleIsHere = false;
            for (const StyleData& candidate : styleData)
                if (candidate.id == *styleId) styleIsHere = true;
            if (!styleIsHere)
                return loadFailure(SerializationError::UnknownDependencyId,
                                   context + ": styleId " + idToString(*styleId) +
                                       " is not a dimension style in this document");
            one.styleId = *styleId;

            const JsonValue* layerField =
                requireField(entry, "layerId", JsonType::String, context, err);
            if (layerField == nullptr) return loadFailure(err.error, err.message);
            const auto layerId = idFromString(layerField->asString());
            if (!layerId.has_value())
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": 'layerId' is not a valid ObjectId");
            bool layerIsHere = false;
            for (const LayerData& candidate : layerData)
                if (candidate.id == *layerId) layerIsHere = true;
            if (!layerIsHere)
                return loadFailure(SerializationError::UnknownDependencyId,
                                   context + ": layerId " + idToString(*layerId) +
                                       " is not a layer in this document");
            one.layerId = *layerId;

            if (const JsonValue* held = entry.find("tolerance")) {
                if (held->type() != JsonType::Object)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'tolerance' is not an object");
                const JsonValue* kind =
                    requireField(*held, "kind", JsonType::String, context, err);
                if (kind == nullptr) return loadFailure(err.error, err.message);
                // REFUSED, not defaulted to None. A tolerance kind this build
                // does not know, read from a newer file, would turn a
                // toleranced size into an untoleranced one -- silently, on a
                // feature somebody toleranced on purpose.
                if (!ParseToleranceKind(kind->asString(), one.tolerance.kind))
                    return loadFailure(SerializationError::InvalidEnumValue,
                                       context + ": unknown tolerance kind '" +
                                           kind->asString() + "'");
                if (const JsonValue* fit = held->find("fit")) {
                    if (fit->type() != JsonType::String)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'fit' is not a string");
                    one.tolerance.fitCode = fit->asString();
                }
                if (one.tolerance.kind == ToleranceKind::Fit &&
                    one.tolerance.fitCode.empty())
                    return loadFailure(SerializationError::MissingField,
                                       context + ": a dimension is marked as a fit and "
                                                 "names no fit code");
                const auto readNumber = [&](const char* key, double& into) -> bool {
                    const JsonValue* value = held->find(key);
                    if (value == nullptr) return true;
                    if (value->type() != JsonType::Number) return false;
                    into = value->asNumber();
                    return true;
                };
                if (!readNumber("upperMm", one.tolerance.upperMm) ||
                    !readNumber("lowerMm", one.tolerance.lowerMm))
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a tolerance deviation is not a number");
                if (one.tolerance.statesNumbers() &&
                    one.tolerance.upperMm < one.tolerance.lowerMm)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a tolerance whose upper is below its "
                                                 "lower describes a size nothing can be "
                                                 "made to");
                if (const JsonValue* decimals = held->find("decimals")) {
                    if (decimals->type() != JsonType::Number)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'decimals' is not a number");
                    one.tolerance.decimals = static_cast<int>(decimals->asNumber());
                    if (one.tolerance.decimals < 0 || one.tolerance.decimals > 9)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": a decimal count past nine prints "
                                                     "digits that are not measurements");
                }
            }
            if (const JsonValue* text = entry.find("textOverride")) {
                if (text->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'textOverride' is not a string");
                one.textOverride = text->asString();
            }
            dimensionData.push_back(std::move(one));
        }
    }

    struct ComponentData {
        ObjectId id = kInvalidObjectId;
        std::string tag;
        std::string symbolName;
        Vec2 positionMm{};
        double rotationRad = 0.0;
        bool mirrored = false;
        ObjectId layerId = kInvalidObjectId;
    };
    std::vector<ComponentData> componentData;
    if (const JsonValue* field = root.find("components")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'components' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "components[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            ComponentData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* tag = requireField(entry, "tag", JsonType::String, context, err);
            if (tag == nullptr) return loadFailure(err.error, err.message);
            if (tag->asString().empty())
                return loadFailure(SerializationError::MissingField,
                                   context + ": a component has no tag, so nothing on this "
                                             "drawing can refer to it");
            one.tag = tag->asString();
            // TWO COMPONENTS CANNOT SHARE A TAG. A wiring list pointing at two
            // parts with one name sends an electrician to whichever they find.
            for (const ComponentData& already : componentData)
                if (already.tag == one.tag)
                    return loadFailure(SerializationError::DuplicateId,
                                       context + ": two components are both called '" +
                                           one.tag + "'");

            const JsonValue* symbol =
                requireField(entry, "symbol", JsonType::String, context, err);
            if (symbol == nullptr) return loadFailure(err.error, err.message);
            if (symbol->asString().empty())
                return loadFailure(SerializationError::MissingField,
                                   context + ": a component names no symbol, so there is "
                                             "nothing to draw");
            one.symbolName = symbol->asString();

            const JsonValue* x = requireField(entry, "xMm", JsonType::Number, context, err);
            const JsonValue* y = requireField(entry, "yMm", JsonType::Number, context, err);
            if (x == nullptr || y == nullptr) return loadFailure(err.error, err.message);
            one.positionMm = Vec2{x->asNumber(), y->asNumber()};

            if (const JsonValue* rotation = entry.find("rotationRad")) {
                if (rotation->type() != JsonType::Number)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'rotationRad' is not a number");
                one.rotationRad = rotation->asNumber();
            }
            if (const JsonValue* mirrored = entry.find("mirrored")) {
                if (mirrored->type() != JsonType::Bool)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'mirrored' is not a boolean");
                one.mirrored = mirrored->asBool();
            }
            const JsonValue* layer =
                requireField(entry, "layerId", JsonType::String, context, err);
            if (layer == nullptr) return loadFailure(err.error, err.message);
            const auto layerId = idFromString(layer->asString());
            if (!layerId.has_value())
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": 'layerId' is not a valid ObjectId");
            bool layerIsHere = false;
            for (const LayerData& candidate : layerData)
                if (candidate.id == *layerId) layerIsHere = true;
            if (!layerIsHere)
                return loadFailure(SerializationError::UnknownDependencyId,
                                   context + ": layerId " + idToString(*layerId) +
                                       " is not a layer in this document");
            one.layerId = *layerId;
            componentData.push_back(std::move(one));
        }
    }

    struct WireData {
        ObjectId id = kInvalidObjectId;
        std::vector<Vec2> pointsMm;
        std::string label;
        ObjectId layerId = kInvalidObjectId;
    };
    std::vector<WireData> wireData;
    if (const JsonValue* field = root.find("wires")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'wires' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "wires[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            WireData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* points =
                requireField(entry, "points", JsonType::Array, context, err);
            if (points == nullptr) return loadFailure(err.error, err.message);
            // A WIRE WITH FEWER THAN TWO POINTS CONNECTS NOTHING, and would sit
            // on the sheet looking like nothing.
            if (points->items().size() < 2)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": a wire has fewer than two points, so it "
                                             "connects nothing");
            for (const JsonValue& point : points->items()) {
                if (point.type() != JsonType::Object)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a wire point is not an object");
                const JsonValue* px =
                    requireField(point, "x", JsonType::Number, context, err);
                const JsonValue* py =
                    requireField(point, "y", JsonType::Number, context, err);
                if (px == nullptr || py == nullptr) return loadFailure(err.error, err.message);
                one.pointsMm.push_back(Vec2{px->asNumber(), py->asNumber()});
            }
            if (const JsonValue* label = entry.find("label")) {
                if (label->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'label' is not a string");
                one.label = label->asString();
            }
            const JsonValue* layer =
                requireField(entry, "layerId", JsonType::String, context, err);
            if (layer == nullptr) return loadFailure(err.error, err.message);
            const auto layerId = idFromString(layer->asString());
            if (!layerId.has_value())
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": 'layerId' is not a valid ObjectId");
            bool layerIsHere = false;
            for (const LayerData& candidate : layerData)
                if (candidate.id == *layerId) layerIsHere = true;
            if (!layerIsHere)
                return loadFailure(SerializationError::UnknownDependencyId,
                                   context + ": layerId " + idToString(*layerId) +
                                       " is not a layer in this document");
            one.layerId = *layerId;
            wireData.push_back(std::move(one));
        }
    }

    struct BomData {
        ObjectId id = kInvalidObjectId;
        std::string name;
        std::string sourcePath;
        Vec2 positionMm{};
        BomDepth depth = BomDepth::TopLevel;
        std::vector<BomColumn> columns;
        double rowHeightMm = 8.0;
        bool growsUpward = true;
        long long sourceStamp = 0;
    };
    struct SymbolData {
        ObjectId id = kInvalidObjectId;
        AnnotationBody body;
        DimensionAnchor anchor;
        Vec2 positionMm{};
        ObjectId layerId = kInvalidObjectId;
    };
    std::vector<SymbolData> symbolData;

    struct HoleTableData {
        ObjectId id = kInvalidObjectId;
        std::string name;
        ObjectId viewId = kInvalidObjectId;
        Vec2 positionMm{};
        Vec2 datumMm{};
        std::vector<HoleColumn> columns;
        double rowHeightMm = 7.0;
    };
    std::vector<HoleTableData> holeTableData;
    std::vector<BomData> bomData;
    // v46 (M44). THE PAGES.
    //
    // A file WITHOUT this array is a drawing from before there was more than
    // one page: its sheet, frame and title block are at the top level and the
    // document was constructed with exactly the page they describe, so that
    // path needs no code here at all -- it is the absence of this block.
    struct PageData {
        ObjectId id = kInvalidObjectId;
        std::string name;
        Sheet paper;
        FrameMargins margins;
        double zoneTargetMm = 100.0;
        bool frameVisible = true;
    };
    std::vector<PageData> pageData;
    ObjectId currentPage = kInvalidObjectId;
    if (const JsonValue* field = root.find("sheets")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'sheets' is not an array");
        // A DRAWING WITH NO PAGES IS NOT A DRAWING, and an empty array is a
        // clearer way to write that than a missing one.
        if (field->items().empty())
            return loadFailure(SerializationError::MissingField,
                               "document: this drawing has no sheets");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "sheets[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            PageData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
            if (name == nullptr) return loadFailure(err.error, err.message);
            if (name->asString().empty())
                return loadFailure(SerializationError::MissingField,
                                   context + ": a sheet has no name");
            one.name = name->asString();
            for (const PageData& already : pageData)
                if (already.name == one.name)
                    return loadFailure(SerializationError::DuplicateId,
                                       context + ": two sheets are both called '" +
                                           one.name + "'");

            const JsonValue* paper =
                requireField(entry, "paper", JsonType::Object, context, err);
            if (paper == nullptr) return loadFailure(err.error, err.message);
            if (!ReadSheet(*paper, context, err, one.paper))
                return loadFailure(err.error, err.message);

            if (const JsonValue* binding = entry.find("bindingMm"))
                if (binding->type() == JsonType::Number)
                    one.margins.bindingMm = binding->asNumber();
            if (const JsonValue* margin = entry.find("marginMm"))
                if (margin->type() == JsonType::Number) one.margins.otherMm = margin->asNumber();
            if (const JsonValue* zone = entry.find("zoneTargetMm"))
                if (zone->type() == JsonType::Number) one.zoneTargetMm = zone->asNumber();
            if (const JsonValue* visible = entry.find("frameVisible"))
                if (visible->type() == JsonType::Bool) one.frameVisible = visible->asBool();
            pageData.push_back(std::move(one));
        }
        if (const JsonValue* current = root.find("currentSheetId"))
            if (const auto parsed = idFromString(current->asString())) currentPage = *parsed;
    }

    if (const JsonValue* field = root.find("symbols")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'symbols' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "symbols[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            SymbolData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* x = requireField(entry, "xMm", JsonType::Number, context, err);
            const JsonValue* y = requireField(entry, "yMm", JsonType::Number, context, err);
            if (x == nullptr || y == nullptr) return loadFailure(err.error, err.message);
            one.positionMm = Vec2{x->asNumber(), y->asNumber()};
            if (const JsonValue* layerId = entry.find("layerId"))
                if (const auto parsed = idFromString(layerId->asString()))
                    one.layerId = *parsed;
            // THE SAME CODEC A DIMENSION USES. Not a second reading of the
            // same struct -- see WriteDimensionAnchor.
            if (!ReadDimensionAnchor(entry, "anchor", context, err, one.anchor))
                // ReadDimensionAnchor fills `err` for a missing or mistyped
                // field and leaves it untouched for an anchor kind this build
                // does not know -- the same contract the dimension reader
                // relies on, so the two failures keep their own messages.
                return err.ok() ? loadFailure(SerializationError::InvalidEnumValue,
                                              context + ": this symbol points at something in "
                                                        "a way this build does not know")
                                : loadFailure(err.error, err.message);

            const JsonValue* kind = requireField(entry, "kind", JsonType::String, context, err);
            if (kind == nullptr) return loadFailure(err.error, err.message);
            if (kind->asString() == "surface-finish") {
                SurfaceFinishSpec finish;
                const JsonValue* symbol =
                    requireField(entry, "symbol", JsonType::String, context, err);
                if (symbol == nullptr) return loadFailure(err.error, err.message);
                // REFUSED, NOT DEFAULTED. "Material must be removed" and
                // "material must NOT be removed" are opposite instructions,
                // and quietly picking one scraps either a casting or a batch.
                if (!ParseSurfaceSymbol(symbol->asString(), finish.symbol))
                    return loadFailure(SerializationError::InvalidEnumValue,
                                       context + ": unknown surface symbol '" +
                                           symbol->asString() + "'");
                const JsonValue* ra =
                    requireField(entry, "raMicrometres", JsonType::Number, context, err);
                if (ra == nullptr) return loadFailure(err.error, err.message);
                finish.raMicrometres = ra->asNumber();
                if (const JsonValue* lower = entry.find("raLowerMicrometres"))
                    if (lower->type() == JsonType::Number)
                        finish.raLowerMicrometres = lower->asNumber();
                if (const JsonValue* process = entry.find("process"))
                    if (process->type() == JsonType::String) finish.process = process->asString();
                if (const JsonValue* lay = entry.find("lay")) {
                    if (lay->type() != JsonType::String)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'lay' is not a string");
                    if (!ParseSurfaceLay(lay->asString(), finish.lay))
                        return loadFailure(SerializationError::InvalidEnumValue,
                                           context + ": unknown lay '" + lay->asString() + "'");
                }
                if (const JsonValue* allowance = entry.find("machiningAllowanceMm"))
                    if (allowance->type() == JsonType::Number)
                        finish.machiningAllowanceMm = allowance->asNumber();
                if (const JsonValue* around = entry.find("allAround"))
                    if (around->type() == JsonType::Bool) finish.allAround = around->asBool();
                one.body = std::move(finish);
            } else if (kind->asString() == "frame") {
                FeatureControlFrameSpec frame;
                const JsonValue* characteristic =
                    requireField(entry, "characteristic", JsonType::String, context, err);
                if (characteristic == nullptr) return loadFailure(err.error, err.message);
                // A characteristic this build does not know would otherwise
                // become the default -- position -- and a flatness frame read
                // as a position frame is a different specification entirely.
                if (!ParseGeometricCharacteristic(characteristic->asString(),
                                                  frame.characteristic))
                    return loadFailure(SerializationError::InvalidEnumValue,
                                       context + ": unknown characteristic '" +
                                           characteristic->asString() + "'");
                const JsonValue* tolerance =
                    requireField(entry, "toleranceMm", JsonType::Number, context, err);
                if (tolerance == nullptr) return loadFailure(err.error, err.message);
                frame.toleranceMm = tolerance->asNumber();
                if (const JsonValue* zone = entry.find("diametricZone"))
                    if (zone->type() == JsonType::Bool) frame.diametricZone = zone->asBool();
                if (const JsonValue* condition = entry.find("condition")) {
                    if (condition->type() != JsonType::String)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'condition' is not a string");
                    if (!ParseMaterialCondition(condition->asString(), frame.condition))
                        return loadFailure(SerializationError::InvalidEnumValue,
                                           context + ": unknown material condition '" +
                                               condition->asString() + "'");
                }
                if (const JsonValue* datums = entry.find("datums")) {
                    if (datums->type() != JsonType::Array)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'datums' is not an array");
                    for (const JsonValue& each : datums->items()) {
                        if (each.type() != JsonType::Object)
                            return loadFailure(SerializationError::InvalidFieldType,
                                               context + ": a datum reference is not an object");
                        DatumReference reference;
                        const JsonValue* datumId =
                            requireField(each, "datumId", JsonType::String, context, err);
                        if (datumId == nullptr) return loadFailure(err.error, err.message);
                        const auto parsed = idFromString(datumId->asString());
                        if (!parsed.has_value() || *parsed > kMaxObjectId)
                            return loadFailure(SerializationError::InvalidFieldType,
                                               context + ": a datum reference is not a valid id");
                        reference.datumId = *parsed;
                        if (const JsonValue* condition = each.find("condition")) {
                            if (condition->type() != JsonType::String)
                                return loadFailure(SerializationError::InvalidFieldType,
                                                   context + ": a datum's condition is not a "
                                                             "string");
                            if (!ParseMaterialCondition(condition->asString(),
                                                        reference.condition))
                                return loadFailure(SerializationError::InvalidEnumValue,
                                                   context + ": unknown material condition '" +
                                                       condition->asString() + "'");
                        }
                        frame.datums.push_back(reference);
                    }
                }
                one.body = std::move(frame);
            } else if (kind->asString() == "balloon") {
                BalloonSpec balloon;
                const JsonValue* tableId =
                    requireField(entry, "tableId", JsonType::String, context, err);
                if (tableId == nullptr) return loadFailure(err.error, err.message);
                const auto parsed = idFromString(tableId->asString());
                if (!parsed.has_value() || *parsed > kMaxObjectId)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a balloon's parts list is not a valid id");
                balloon.tableId = *parsed;
                const JsonValue* sourceFile =
                    requireField(entry, "sourceFile", JsonType::String, context, err);
                if (sourceFile == nullptr) return loadFailure(err.error, err.message);
                balloon.sourceFile = sourceFile->asString();
                if (const JsonValue* partName = entry.find("partName"))
                    if (partName->type() == JsonType::String)
                        balloon.partName = partName->asString();
                one.body = std::move(balloon);
            } else if (kind->asString() == "weld") {
                WeldSymbolSpec weld;
                // A SIDE IS PRESENT OR IT IS NOT. There is no field naming a
                // side, so a file cannot say arrow and mean other -- the same
                // property the type has in memory, kept across the disk.
                if (const JsonValue* arrow = entry.find("arrowSide")) {
                    if (arrow->type() != JsonType::Object)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'arrowSide' is not an object");
                    WeldBead bead;
                    if (!ReadWeldBead(*arrow, context + ".arrowSide", err, bead))
                        return loadFailure(err.error, err.message);
                    weld.arrowSide = bead;
                }
                if (const JsonValue* other = entry.find("otherSide")) {
                    if (other->type() != JsonType::Object)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'otherSide' is not an object");
                    WeldBead bead;
                    if (!ReadWeldBead(*other, context + ".otherSide", err, bead))
                        return loadFailure(err.error, err.message);
                    weld.otherSide = bead;
                }
                if (const JsonValue* around = entry.find("allAround"))
                    if (around->type() == JsonType::Bool) weld.allAround = around->asBool();
                if (const JsonValue* field = entry.find("fieldWeld"))
                    if (field->type() == JsonType::Bool) weld.fieldWeld = field->asBool();
                if (const JsonValue* staggered = entry.find("staggered"))
                    if (staggered->type() == JsonType::Bool)
                        weld.staggered = staggered->asBool();
                if (const JsonValue* tail = entry.find("tail"))
                    if (tail->type() == JsonType::String) weld.tail = tail->asString();
                // NO REFUSAL CHECK HERE, ON PURPOSE.
                //
                // The first cut of M47 called WhyWeldRefused right at this
                // point. The mutation gate found it survives being deleted,
                // and the reason is the good one: the loader ALREADY asks
                // whyAnnotationRefused of every symbol once they are all back
                // (see the end of this file), and that is the same function
                // the saver asks -- which is what ADR-M3-008 actually wants.
                //
                // A second copy here would be a second place stating one rule:
                // it would pass today, and the day a weld rule learns to
                // depend on something outside the spec, the two would answer
                // differently and the file would be accepted by one and
                // refused by the other.
                one.body = std::move(weld);
            } else if (kind->asString() == "datum") {
                DatumFeatureSpec datum;
                if (const JsonValue* note = entry.find("note"))
                    if (note->type() == JsonType::String) datum.note = note->asString();
                one.body = std::move(datum);
            } else {
                return loadFailure(SerializationError::InvalidEnumValue,
                                   context + ": '" + kind->asString() +
                                       "' is not a symbol this build knows");
            }
            symbolData.push_back(std::move(one));
        }
    }

    // v48 (M48). The history, and then the tables that show it.
    std::vector<Revision> revisionData;
    if (const JsonValue* field = root.find("revisions")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'revisions' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "revisions[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            Revision one;
            const JsonValue* letter =
                requireField(entry, "letter", JsonType::String, context, err);
            if (letter == nullptr) return loadFailure(err.error, err.message);
            one.letter = letter->asString();
            const JsonValue* description =
                requireField(entry, "description", JsonType::String, context, err);
            if (description == nullptr) return loadFailure(err.error, err.message);
            one.description = description->asString();
            if (const JsonValue* date = entry.find("date"))
                if (date->type() == JsonType::String) one.date = date->asString();
            if (const JsonValue* by = entry.find("by"))
                if (by->type() == JsonType::String) one.by = by->asString();
            // WHAT THE SAVER REFUSES, THE LOADER REFUSES, by calling the same
            // function (ADR-M3-008) -- and it is asked against the rows read so
            // far, so a file carrying two Rev Cs is refused rather than opened
            // with a history nobody can cite.
            const std::string why = WhyRevisionRefused(one, revisionData);
            if (!why.empty())
                return loadFailure(SerializationError::InvalidFieldType, context + ": " + why);
            revisionData.push_back(std::move(one));
        }
    }

    struct RevisionTableData {
        ObjectId id = kInvalidObjectId;
        std::string name;
        Vec2 positionMm{};
        double widthMm = 0.0;
        double rowHeightMm = 0.0;
        ObjectId layerId = kInvalidObjectId;
    };
    std::vector<RevisionTableData> revisionTableData;
    if (const JsonValue* field = root.find("revisionTables")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'revisionTables' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "revisionTables[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            RevisionTableData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);
            const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
            if (name == nullptr) return loadFailure(err.error, err.message);
            one.name = name->asString();
            const JsonValue* x = requireField(entry, "xMm", JsonType::Number, context, err);
            const JsonValue* y = requireField(entry, "yMm", JsonType::Number, context, err);
            if (x == nullptr || y == nullptr) return loadFailure(err.error, err.message);
            one.positionMm = Vec2{x->asNumber(), y->asNumber()};
            one.widthMm = 120.0;
            one.rowHeightMm = 6.0;
            if (const JsonValue* width = entry.find("widthMm"))
                if (width->type() == JsonType::Number) one.widthMm = width->asNumber();
            if (const JsonValue* height = entry.find("rowHeightMm"))
                if (height->type() == JsonType::Number) one.rowHeightMm = height->asNumber();
            if (const JsonValue* layerId = entry.find("layerId"))
                if (const auto parsed = idFromString(layerId->asString()))
                    one.layerId = *parsed;
            revisionTableData.push_back(std::move(one));
        }
    }

    if (const JsonValue* field = root.find("holeTables")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'holeTables' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "holeTables[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            HoleTableData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
            if (name == nullptr) return loadFailure(err.error, err.message);
            if (name->asString().empty())
                return loadFailure(SerializationError::MissingField,
                                   context + ": a hole table has no name");
            one.name = name->asString();

            const JsonValue* viewField =
                requireField(entry, "viewId", JsonType::String, context, err);
            if (viewField == nullptr) return loadFailure(err.error, err.message);
            const auto viewId = idFromString(viewField->asString());
            if (!viewId.has_value() || *viewId == kInvalidObjectId || *viewId > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'viewId' is not a valid ObjectId");
            one.viewId = *viewId;

            const JsonValue* x = requireField(entry, "xMm", JsonType::Number, context, err);
            const JsonValue* y = requireField(entry, "yMm", JsonType::Number, context, err);
            if (x == nullptr || y == nullptr) return loadFailure(err.error, err.message);
            one.positionMm = Vec2{x->asNumber(), y->asNumber()};

            const JsonValue* datumX =
                requireField(entry, "datumXMm", JsonType::Number, context, err);
            const JsonValue* datumY =
                requireField(entry, "datumYMm", JsonType::Number, context, err);
            if (datumX == nullptr || datumY == nullptr)
                return loadFailure(err.error, err.message);
            one.datumMm = Vec2{datumX->asNumber(), datumY->asNumber()};

            if (const JsonValue* height = entry.find("rowHeightMm")) {
                if (height->type() != JsonType::Number)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'rowHeightMm' is not a number");
                one.rowHeightMm = height->asNumber();
                if (!(one.rowHeightMm > 0.0))
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a row of no height would draw every hole "
                                                 "on one line");
            }

            const JsonValue* columns =
                requireField(entry, "columns", JsonType::Array, context, err);
            if (columns == nullptr) return loadFailure(err.error, err.message);
            if (columns->items().empty())
                return loadFailure(SerializationError::MissingField,
                                   context + ": a hole table with no columns is a rectangle");
            for (const JsonValue& column : columns->items()) {
                if (column.type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a column is not a string");
                HoleColumn which = HoleColumn::Tag;
                // REFUSED. A column this build does not know would otherwise
                // become the tag column, and a table whose X turned into a
                // repeated tag is one somebody drills from.
                if (!ParseHoleColumn(column.asString(), which))
                    return loadFailure(SerializationError::InvalidEnumValue,
                                       context + ": unknown hole table column '" +
                                           column.asString() + "'");
                for (const HoleColumn already : one.columns)
                    if (already == which)
                        return loadFailure(SerializationError::DuplicateId,
                                           context + ": the column '" + column.asString() +
                                               "' appears twice");
                one.columns.push_back(which);
            }
            holeTableData.push_back(std::move(one));
        }
    }

    if (const JsonValue* field = root.find("bomTables")) {
        if (field->type() != JsonType::Array)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'bomTables' is not an array");
        for (std::size_t i = 0; i < field->items().size(); ++i) {
            const JsonValue& entry = field->items()[i];
            const std::string context = "bomTables[" + std::to_string(i) + "]";
            if (entry.type() != JsonType::Object)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": entry is not an object");
            BomData one;
            const JsonValue* idField = requireField(entry, "id", JsonType::String, context, err);
            if (idField == nullptr) return loadFailure(err.error, err.message);
            const auto id = idFromString(idField->asString());
            if (!id.has_value() || *id == kInvalidObjectId || *id > kMaxObjectId)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'id' is not a valid ObjectId");
            if (!registerId(*id, context, err)) return loadFailure(err.error, err.message);
            one.id = *id;
            noteSheet(entry, one.id);

            const JsonValue* name = requireField(entry, "name", JsonType::String, context, err);
            if (name == nullptr) return loadFailure(err.error, err.message);
            if (name->asString().empty())
                return loadFailure(SerializationError::MissingField,
                                   context + ": a parts list has no name");
            one.name = name->asString();

            const JsonValue* source =
                requireField(entry, "sourcePath", JsonType::String, context, err);
            if (source == nullptr) return loadFailure(err.error, err.message);
            if (source->asString().empty())
                return loadFailure(SerializationError::MissingField,
                                   context + ": a parts list names no assembly, so there is "
                                             "nothing for it to count");
            one.sourcePath = source->asString();

            const JsonValue* x = requireField(entry, "xMm", JsonType::Number, context, err);
            const JsonValue* y = requireField(entry, "yMm", JsonType::Number, context, err);
            if (x == nullptr || y == nullptr) return loadFailure(err.error, err.message);
            one.positionMm = Vec2{x->asNumber(), y->asNumber()};

            if (const JsonValue* depth = entry.find("depth")) {
                if (depth->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'depth' is not a string");
                // REFUSED, not defaulted: "every part however deep" and "what
                // this is made of" are different lists, and quietly picking one
                // gives a reader a bill of materials for something else.
                if (!ParseBomDepth(depth->asString(), one.depth))
                    return loadFailure(SerializationError::InvalidEnumValue,
                                       context + ": unknown parts list depth '" +
                                           depth->asString() + "'");
            }
            if (const JsonValue* height = entry.find("rowHeightMm")) {
                if (height->type() != JsonType::Number)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'rowHeightMm' is not a number");
                one.rowHeightMm = height->asNumber();
                if (!(one.rowHeightMm > 0.0))
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a parts list with no row height draws "
                                                 "nothing anybody can read");
            }
            if (const JsonValue* upward = entry.find("growsUpward")) {
                if (upward->type() != JsonType::Bool)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'growsUpward' is not a boolean");
                one.growsUpward = upward->asBool();
            }
            if (const JsonValue* stamp = entry.find("sourceStamp")) {
                if (stamp->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'sourceStamp' is not a string");
                try {
                    one.sourceStamp = std::stoll(stamp->asString());
                } catch (const std::exception&) {
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'sourceStamp' is not a number");
                }
            }

            const JsonValue* columns =
                requireField(entry, "columns", JsonType::Array, context, err);
            if (columns == nullptr) return loadFailure(err.error, err.message);
            if (columns->items().empty())
                return loadFailure(SerializationError::MissingField,
                                   context + ": a parts list with no columns is a rectangle");
            for (const JsonValue& column : columns->items()) {
                if (column.type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": a column is not a string");
                BomColumn which = BomColumn::Item;
                // REFUSED. A column this build does not know would otherwise
                // silently become the item number, and a parts list whose
                // quantity column turned into a row number is one somebody
                // orders from.
                if (!ParseBomColumn(column.asString(), which))
                    return loadFailure(SerializationError::InvalidEnumValue,
                                       context + ": unknown parts list column '" +
                                           column.asString() + "'");
                for (const BomColumn already : one.columns)
                    if (already == which)
                        return loadFailure(SerializationError::DuplicateId,
                                           context + ": the column '" + column.asString() +
                                               "' is shown twice");
                one.columns.push_back(which);
            }
            bomData.push_back(std::move(one));
        }
    }

    FrameMargins margins;
    double zoneTargetMm = 100.0;
    bool frameVisible = true;
    if (const JsonValue* field = root.find("frame")) {
        if (field->type() != JsonType::Object)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'frame' is not an object");
        const auto readNumber = [&](const char* key, double& into) -> bool {
            const JsonValue* value = field->find(key);
            if (value == nullptr) return true;
            if (value->type() != JsonType::Number) return false;
            into = value->asNumber();
            return true;
        };
        if (!readNumber("bindingMm", margins.bindingMm) ||
            !readNumber("otherMm", margins.otherMm) ||
            !readNumber("zoneTargetMm", zoneTargetMm))
            return loadFailure(SerializationError::InvalidFieldType,
                               "frame: a margin is not a number");
        if (!(zoneTargetMm > 0.0))
            return loadFailure(SerializationError::InvalidFieldType,
                               "frame: a zone size of zero divides the border into nothing");
        if (const JsonValue* visible = field->find("visible")) {
            if (visible->type() != JsonType::Bool)
                return loadFailure(SerializationError::InvalidFieldType,
                                   "frame: field 'visible' is not a boolean");
            frameVisible = visible->asBool();
        }
    }

    bool haveTitleBlock = false;
    TitleBlock titleBlock;
    if (const JsonValue* field = root.find("titleBlock")) {
        if (field->type() != JsonType::Object)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'titleBlock' is not an object");
        haveTitleBlock = true;
        double widthMm = titleBlock.widthMm();
        double rowHeightMm = titleBlock.rowHeightMm();
        if (const JsonValue* value = field->find("widthMm")) {
            if (value->type() != JsonType::Number)
                return loadFailure(SerializationError::InvalidFieldType,
                                   "titleBlock: field 'widthMm' is not a number");
            widthMm = value->asNumber();
        }
        if (const JsonValue* value = field->find("rowHeightMm")) {
            if (value->type() != JsonType::Number)
                return loadFailure(SerializationError::InvalidFieldType,
                                   "titleBlock: field 'rowHeightMm' is not a number");
            rowHeightMm = value->asNumber();
        }
        if (!(widthMm > 0.0) || !(rowHeightMm > 0.0))
            return loadFailure(SerializationError::InvalidFieldType,
                               "titleBlock: a block with no width or no row height draws "
                               "nothing anybody can read");
        titleBlock.restoreSize(widthMm, rowHeightMm);
        if (const JsonValue* value = field->find("visible")) {
            if (value->type() != JsonType::Bool)
                return loadFailure(SerializationError::InvalidFieldType,
                                   "titleBlock: field 'visible' is not a boolean");
            titleBlock.setVisible(value->asBool());
        }

        const JsonValue* rows = field->find("fields");
        if (rows != nullptr) {
            if (rows->type() != JsonType::Array)
                return loadFailure(SerializationError::InvalidFieldType,
                                   "titleBlock: field 'fields' is not an array");
            std::vector<TitleBlockField> read;
            for (std::size_t i = 0; i < rows->items().size(); ++i) {
                const JsonValue& entry = rows->items()[i];
                const std::string context = "titleBlock.fields[" + std::to_string(i) + "]";
                if (entry.type() != JsonType::Object)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": entry is not an object");
                const JsonValue* label =
                    requireField(entry, "label", JsonType::String, context, err);
                if (label == nullptr) return loadFailure(err.error, err.message);
                if (label->asString().empty())
                    return loadFailure(SerializationError::MissingField,
                                       context + ": a title block row has no label");
                for (const TitleBlockField& already : read)
                    if (already.label == label->asString())
                        return loadFailure(SerializationError::DuplicateId,
                                           context + ": two rows are both called '" +
                                               label->asString() + "'");

                TitleBlockField one;
                one.label = label->asString();
                const JsonValue* source =
                    requireField(entry, "source", JsonType::String, context, err);
                if (source == nullptr) return loadFailure(err.error, err.message);
                // REFUSED, not defaulted to Free. A source this build does not
                // know, read from a newer file, would turn a derived row into
                // a typed one holding whatever string was beside it -- which
                // is how a title block starts stating a scale nothing was
                // plotted at.
                if (!ParseTitleBlockSource(source->asString(), one.source))
                    return loadFailure(SerializationError::InvalidEnumValue,
                                       context + ": unknown title block source '" +
                                           source->asString() + "'");
                if (const JsonValue* value = entry.find("value")) {
                    if (value->type() != JsonType::String)
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": field 'value' is not a string");
                    if (one.isDerived() && !value->asString().empty())
                        return loadFailure(SerializationError::InvalidFieldType,
                                           context + ": a derived row carries a typed value, "
                                                     "which is a second answer about the "
                                                     "sheet waiting to go stale");
                    one.value = value->asString();
                }
                read.push_back(std::move(one));
            }
            bool sawTitle = false;
            bool sawNumber = false;
            for (const TitleBlockField& one : read) {
                if (one.label == kTitleBlockTitleLabel) sawTitle = true;
                if (one.label == kTitleBlockNumberLabel) sawNumber = true;
            }
            if (!sawTitle || !sawNumber)
                return loadFailure(SerializationError::MissingField,
                                   "titleBlock: the rows that identify this drawing are "
                                   "missing");
            titleBlock.restoreFields(std::move(read));
        }
    }

    GeneralToleranceClass generalTolerance = GeneralToleranceClass::None;
    if (const JsonValue* field = root.find("generalTolerance")) {
        if (field->type() != JsonType::String)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: 'generalTolerance' is not a string");
        if (!ParseGeneralToleranceClass(field->asString(), generalTolerance))
            return loadFailure(SerializationError::InvalidEnumValue,
                               "document: unknown general tolerance class '" +
                                   field->asString() + "'");
    }

    ObjectId currentStyleId = kInvalidObjectId;
    if (const JsonValue* field = root.find("currentDimensionStyleId")) {
        if (field->type() != JsonType::String)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: 'currentDimensionStyleId' is not a string");
        const auto parsed = idFromString(field->asString());
        if (!parsed.has_value())
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: 'currentDimensionStyleId' is not a valid ObjectId");
        bool isHere = false;
        for (const StyleData& candidate : styleData)
            if (candidate.id == *parsed) isHere = true;
        if (!isHere)
            return loadFailure(SerializationError::UnknownDependencyId,
                               "document: the current dimension style is not in this document");
        currentStyleId = *parsed;
    }

    ObjectId currentLayerId = kInvalidObjectId;
    if (const JsonValue* field = root.find("currentLayerId")) {
        if (field->type() != JsonType::String)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'currentLayerId' is not a string");
        const auto parsed = idFromString(field->asString());
        if (!parsed.has_value())
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: 'currentLayerId' is not a valid ObjectId");
        bool isHere = false;
        for (const LayerData& candidate : layerData)
            if (candidate.id == *parsed) isHere = true;
        if (!isHere)
            return loadFailure(SerializationError::UnknownDependencyId,
                               "document: currentLayerId " + idToString(*parsed) +
                                   " is not a layer in this document");
        currentLayerId = *parsed;
    }

    // --- Build ---------------------------------------------------------------
    auto document = std::make_unique<DrawingDocument>(documentId, documentName);
    // WHO OWNS THE TABLES ON LOAD. The constructor seeds layer 0 and
    // CONTINUOUS, and the file carries its own -- so restoring naively would
    // give the document two layers called "0", one of them with an id nothing
    // in the file points at. The file wins: those are the layers every
    // reference means. The same rule the assembly loader applies to frames.
    if (!layerData.empty() || !linetypeData.empty()) {
        for (const Layer* existing : document->layers())
            document->restoreRemoveObject(existing->id());
        for (const Linetype* existing : document->linetypes())
            document->restoreRemoveObject(existing->id());
    }
    // ...and the seeded dimension style, for the same reason: the file carries
    // its own ISO-25 and two of them would leave one with an id nothing points
    // at.
    if (!styleData.empty())
        for (const DimensionStyle* existing : document->dimensionStyles())
            document->restoreRemoveObject(existing->id());
    if (!frameData.empty())
        for (const ReferenceFrame* existing : document->frames())
            document->restoreRemoveObject(existing->id());
    docjson::restoreFramesAndConnectors(*document, frameData, connectorData);

    for (auto& one : linetypeData)
        document->restoreLinetype(one.id, std::move(one.name), std::move(one.description),
                                  std::move(one.pattern));
    for (auto& one : layerData)
        document->restoreLayer(one.id, std::move(one.name), one.color, std::move(one.linetype),
                               one.on, one.frozen, one.locked, one.lineweight);
    // VIEWS LAST, because a view sits on the sheet and the sheet has to be the
    // one the file describes before a position can be judged against it.
    document->restoreSheet(sheetSize, sheetOrientation, sheetScale, customWidthMm,
                           customHeightMm, projectionAngle);
    // PARENTS FIRST. `restoreView` makes the graph edge to the parent, and a
    // dependency on a node that is not there yet is an edge that never
    // existed -- so a child restored first would never move when its parent
    // did. Ordered by walking the chain rather than by trusting the file's
    // order, for the reason the loop check above gives.
    std::vector<std::size_t> order;
    std::vector<bool> placed(viewData.size(), false);
    for (std::size_t pass = 0; pass < viewData.size() + 1 && order.size() < viewData.size();
         ++pass) {
        for (std::size_t i = 0; i < viewData.size(); ++i) {
            if (placed[i]) continue;
            const ObjectId parent = viewData[i].parentViewId;
            bool parentReady = parent == kInvalidObjectId;
            for (const std::size_t done : order)
                if (viewData[done].id == parent) parentReady = true;
            if (!parentReady) continue;
            order.push_back(i);
            placed[i] = true;
        }
    }
    for (const std::size_t index : order) {
        auto& one = viewData[index];
        DrawingView& made = document->restoreView(
            one.id, std::move(one.name), ComputeState::Dirty, std::move(one.sourcePath),
            std::move(one.bodyName), one.direction, one.positionMm, one.scale, one.ownScale,
            one.showHidden, one.showTangent, one.parentViewId, one.alignmentOffsetMm);
        if (one.sectionActive) {
            DrawingView::SectionCut cut;
            cut.active = true;
            cut.fromMm = one.sectionFromMm;
            cut.toMm = one.sectionToMm;
            cut.arrowSide = one.sectionArrowSide;
            made.setSectionCut(cut);
        }
        if (one.detailActive) {
            DrawingView::DetailFrame frame;
            frame.active = true;
            frame.centreMm = one.detailCentreMm;
            frame.radiusMm = one.detailRadiusMm;
            made.setDetailFrame(frame);
        }
        if (one.flatPattern) made.setShowsFlatPattern(true);
        if (one.breakSpan.active) made.setBreakSpan(one.breakSpan);
    }
    // ENTITIES AFTER THE TABLES, because each names a layer.
    for (auto& one : entityData)
        document->restoreEntity(one.id, std::move(one.shape), one.layerId, one.color,
                                std::move(one.linetype), one.lineweight);
    for (auto& one : styleData) {
        DimensionStyle& style = document->restoreDimensionStyle(one.id, std::move(one.name));
        style.setTextHeightMm(one.textHeightMm);
        style.setTextGapMm(one.textGapMm);
        style.setArrowSizeMm(one.arrowSizeMm);
        style.setExtensionGapMm(one.extensionGapMm);
        style.setExtensionOvershootMm(one.extensionOvershootMm);
        style.setDecimals(one.decimals);
        style.setSuffix(std::move(one.suffix));
        style.setOverallScale(one.overallScale);
    }
    // DIMENSIONS LAST, because one names a style, a layer and possibly a view.
    for (auto& one : dimensionData) {
        DrawingDimension& made = document->restoreDimension(
            one.id, one.kind, one.first, one.second, one.direction, one.linePositionMm,
            one.styleId, one.layerId, std::move(one.textOverride));
        made.setTolerance(std::move(one.tolerance));
    }
    document->restoreGeneralToleranceClass(generalTolerance);
    if (currentStyleId != kInvalidObjectId)
        document->restoreCurrentDimensionStyle(currentStyleId);
    for (auto& one : componentData)
        document->restoreSymbol(one.id, std::move(one.tag), std::move(one.symbolName),
                                one.positionMm, one.rotationRad, one.mirrored, one.layerId);
    for (auto& one : wireData)
        document->restoreWire(one.id, std::move(one.pointsMm), one.layerId,
                              std::move(one.label));
    // THE PAGES BEFORE EVERYTHING ELSE, because every object says which one it
    // is on and the check at the end asks whether that page is here.
    if (!pageData.empty()) {
        document->clearSheetPagesForRestore();
        for (auto& one : pageData)
            document->restoreSheetPage(one.id, std::move(one.name), std::move(one.paper),
                                       one.margins, one.zoneTargetMm, one.frameVisible);
        // A CURRENT PAGE THE FILE DOES NOT HAVE falls back to the first, rather
        // than leaving the document pointing at nothing. Which page was on
        // screen when somebody saved is a convenience, not a fact about the
        // drawing, and it is the one field here worth being forgiving about.
        document->restoreCurrentSheet(document->findSheetPage(currentPage) != nullptr
                                          ? currentPage
                                          : document->sheetPages().front()->id());
    }

    // THE DATUMS FIRST, so a frame restored after them can find the datum it
    // names. Written in one array and separated here, because the ORDER a
    // datum sits in that array is what its letter is derived from -- reading
    // datums out of order would re-letter the drawing.
    for (auto& one : symbolData)
        if (std::holds_alternative<DatumFeatureSpec>(one.body))
            document->restoreAnnotation(one.id, one.body, one.anchor, one.positionMm,
                                        one.layerId);
    for (auto& one : symbolData)
        if (!std::holds_alternative<DatumFeatureSpec>(one.body))
            document->restoreAnnotation(one.id, one.body, one.anchor, one.positionMm,
                                        one.layerId);
    // ...and the balloons cannot be checked until the parts lists are back, so
    // the check below runs after restoreBomTable. Moved rather than repeated:
    // one check, in one place, after everything it reads exists.
    // ...AND THEN THE SAME CHECK THE SAVER MAKES (ADR-M3-008). Not a second
    // list of rules here: the document is asked, exactly as it is asked before
    // writing, so what one refuses the other refuses and neither can drift.
    for (auto& one : holeTableData) {
        // THE VIEW HAS TO BE HERE, and it is by now -- views are restored
        // first. A table pointing at nothing would come back as an empty box
        // on the paper with no way for a reader to tell what it was meant to
        // list, which is worse than a file that says why it will not open.
        if (document->findView(one.viewId) == nullptr)
            return loadFailure(SerializationError::UnknownDependencyId,
                               "hole table '" + one.name +
                                   "' is a table of a view that is not in this drawing");
    }
    for (auto& one : holeTableData)
        document->restoreHoleTable(one.id, std::move(one.name), one.viewId, one.positionMm,
                                   one.datumMm, std::move(one.columns), one.rowHeightMm);
    for (auto& one : bomData)
        document->restoreBomTable(one.id, std::move(one.name), std::move(one.sourcePath),
                                  one.positionMm, one.depth, std::move(one.columns),
                                  one.rowHeightMm, one.growsUpward, one.sourceStamp);
    // v48 (M48). THE HISTORY FIRST, then the tables that show it -- a table
    // restored before the rows would draw an empty box on the first repaint
    // and be right only by the time anybody looked.
    for (std::size_t i = 0; i < revisionData.size(); ++i)
        document->restoreRevision(std::move(revisionData[i]), i);
    for (auto& one : revisionTableData) {
        RevisionTable& table = document->restoreRevisionTable(
            one.id, std::move(one.name), one.positionMm, one.widthMm, one.rowHeightMm);
        table.setLayerId(one.layerId);
    }
    // AND WHERE EVERYTHING SITS, in one place, now the pages are here.
    //
    // A PAGE THIS DRAWING DOES NOT HAVE IS A REFUSAL. setObjectSheet says no
    // and leaves the object where it was -- which resolves to the first page,
    // so the drawing looks perfectly consistent afterwards and the object has
    // silently moved to sheet 1. Nobody would ever see that happen, which is
    // exactly why the load stops here instead.
    for (const auto& [objectId, sheetId] : objectSheets)
        if (!document->setObjectSheet(objectId, sheetId))
            return loadFailure(SerializationError::UnknownDependencyId,
                               "an object in this file is on a sheet that is not in the "
                               "drawing");

    // EVERY OBJECT ON A PAGE THAT IS HERE (M44) -- the saver's rule, asked of
    // the same function, once the pages and the objects are all back.
    if (const std::string why = document->whyDrawingRefused(); !why.empty())
        return loadFailure(SerializationError::UnknownDependencyId, why);

    // THE SAME CHECK THE SAVER MAKES (ADR-M3-008), once everything a symbol
    // reads is back: a frame's datums, and a balloon's parts list.
    for (const auto& one : symbolData) {
        const std::string why = document->whyAnnotationRefused(one.id);
        if (!why.empty())
            return loadFailure(SerializationError::InvalidFieldType,
                               "a symbol in this file cannot be drawn: " + why);
    }
    // ONLY FOR A FILE FROM BEFORE M44. A drawing with pages carries its frame
    // on each of them, and applying the legacy defaults here would quietly
    // overwrite what those pages said -- which it did: a reopened drawing came
    // back with a zone size nobody had chosen.
    if (pageData.empty()) document->restoreFrame(margins, zoneTargetMm, frameVisible);
    // A FILE WITHOUT A TITLE BLOCK KEEPS THE SEEDED ONE, rather than being
    // given an empty box -- an older file predates the block, and a drawing
    // with nowhere to put its number is not an improvement on one with the
    // standard rows.
    if (haveTitleBlock) document->restoreTitleBlock(std::move(titleBlock));
    if (currentLayerId != kInvalidObjectId) document->restoreCurrentLayer(currentLayerId);

    // A loaded document starts with an EMPTY history (ADR-M9-001).
    return DrawingLoadResult{std::move(document), SerializationError::None, {}};
}

SaveResult saveDrawingDocumentToFile(const DrawingDocument& document, const std::string& path) {
    // VALIDATED BEFORE THE FILE IS OPENED. Opening it first truncates it, so a
    // refused save would already have destroyed the good version on disk.
    if (const SaveResult invalid = validateSaveable(document); !invalid) return invalid;
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return SaveResult{SerializationError::IoError, "could not open '" + path + "' for writing"};
    return saveDrawingDocument(document, out);
}

DrawingLoadResult loadDrawingDocumentFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return loadFailure(SerializationError::FileNotFound, "could not open '" + path + "'");
    return loadDrawingDocument(in);
}

} // namespace paramcad
