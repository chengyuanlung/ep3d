#include "Core/Serialization/DrawingDocumentSerializer.h"

#include "Core/Serialization/DocumentJson.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace paramcad {

namespace {

using docjson::FieldError;
using docjson::fieldError;
using docjson::idFromString;
using docjson::idToString;
using docjson::requireField;

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
    const Sheet& sheet = document.sheet();
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
    root.set("sheet", std::move(paper));

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

    JsonValue dimensions = JsonValue::makeArray();
    const auto anchorToJson = [](const DimensionAnchor& anchor) {
        JsonValue out = JsonValue::makeObject();
        out.set("kind", JsonValue::makeString(std::string(toString(anchor.kind))));
        // AN ANCHOR IS A REFERENCE, not a coordinate -- except for Free, which
        // is a coordinate on purpose. What is written is exactly what the
        // resolver asks again on the next rebuild.
        out.set("x", JsonValue::makeNumber(anchor.at.x));
        out.set("y", JsonValue::makeNumber(anchor.at.y));
        out.set("entityId", JsonValue::makeString(idToString(anchor.entityId)));
        out.set("snapIndex", JsonValue::makeNumber(static_cast<double>(anchor.snapIndex)));
        out.set("viewId", JsonValue::makeString(idToString(anchor.viewId)));
        out.set("toleranceMm", JsonValue::makeNumber(anchor.toleranceMm));
        return out;
    };
    for (const DrawingDimension* dimension : document.dimensions()) {
        JsonValue item = JsonValue::makeObject();
        item.set("id", JsonValue::makeString(idToString(dimension->id())));
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
        dimensions.add(std::move(item));
    }
    root.set("dimensions", std::move(dimensions));
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
    if (const JsonValue* paper = root.find("sheet")) {
        const std::string context = "sheet";
        if (paper->type() != JsonType::Object)
            return loadFailure(SerializationError::InvalidFieldType,
                               "document: field 'sheet' is not an object");
        const JsonValue* size = requireField(*paper, "size", JsonType::String, context, err);
        if (size == nullptr) return loadFailure(err.error, err.message);
        const auto parsedSize = sheetSizeFromString(size->asString());
        if (!parsedSize.has_value())
            return loadFailure(SerializationError::InvalidEnumValue,
                               context + ": unknown sheet size '" + size->asString() + "'");
        sheetSize = *parsedSize;

        const JsonValue* orientation =
            requireField(*paper, "orientation", JsonType::String, context, err);
        if (orientation == nullptr) return loadFailure(err.error, err.message);
        const auto parsedOrientation = sheetOrientationFromString(orientation->asString());
        if (!parsedOrientation.has_value())
            return loadFailure(SerializationError::InvalidEnumValue,
                               context + ": unknown orientation '" + orientation->asString() +
                                   "'");
        sheetOrientation = *parsedOrientation;

        const JsonValue* scale = requireField(*paper, "scale", JsonType::String, context, err);
        if (scale == nullptr) return loadFailure(err.error, err.message);
        if (!ParseDrawingScale(scale->asString(), sheetScale))
            return loadFailure(SerializationError::InvalidFieldType,
                               context + ": '" + scale->asString() +
                                   "' is not a scale like 1:2");

        if (const JsonValue* angle = paper->find("projectionAngle")) {
            if (angle->type() != JsonType::String)
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": field 'projectionAngle' is not a string");
            if (angle->asString() == "First") projectionAngle = ProjectionAngle::First;
            else if (angle->asString() == "Third") projectionAngle = ProjectionAngle::Third;
            else
                return loadFailure(SerializationError::InvalidEnumValue,
                                   context + ": unknown projection angle '" +
                                       angle->asString() + "'");
        }

        if (sheetSize == SheetSize::Custom) {
            const JsonValue* width =
                requireField(*paper, "widthMm", JsonType::Number, context, err);
            if (width == nullptr) return loadFailure(err.error, err.message);
            const JsonValue* height =
                requireField(*paper, "heightMm", JsonType::Number, context, err);
            if (height == nullptr) return loadFailure(err.error, err.message);
            customWidthMm = width->asNumber();
            customHeightMm = height->asNumber();
            if (!(customWidthMm > 0.0) || !(customHeightMm > 0.0))
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": a custom sheet has no area");
        }
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

            const auto readAnchor = [&](const char* key, DimensionAnchor& into) -> bool {
                const JsonValue* at = requireField(entry, key, JsonType::Object, context, err);
                if (at == nullptr) return false;
                const JsonValue* kindField =
                    requireField(*at, "kind", JsonType::String, context, err);
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
                    if (const auto parsed = idFromString(entityId->asString()))
                        into.entityId = *parsed;
                if (const JsonValue* snapIndex = at->find("snapIndex"))
                    if (snapIndex->type() == JsonType::Number)
                        into.snapIndex = static_cast<int>(snapIndex->asNumber());
                if (const JsonValue* viewId = at->find("viewId"))
                    if (const auto parsed = idFromString(viewId->asString()))
                        into.viewId = *parsed;
                if (const JsonValue* tolerance = at->find("toleranceMm"))
                    if (tolerance->type() == JsonType::Number && tolerance->asNumber() > 0.0)
                        into.toleranceMm = tolerance->asNumber();
                return true;
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

            if (const JsonValue* text = entry.find("textOverride")) {
                if (text->type() != JsonType::String)
                    return loadFailure(SerializationError::InvalidFieldType,
                                       context + ": field 'textOverride' is not a string");
                one.textOverride = text->asString();
            }
            dimensionData.push_back(std::move(one));
        }
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
        document->restoreView(one.id, std::move(one.name), ComputeState::Dirty,
                              std::move(one.sourcePath), std::move(one.bodyName), one.direction,
                              one.positionMm, one.scale, one.ownScale, one.showHidden,
                              one.showTangent, one.parentViewId, one.alignmentOffsetMm);
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
    for (auto& one : dimensionData)
        document->restoreDimension(one.id, one.kind, one.first, one.second, one.direction,
                                   one.linePositionMm, one.styleId, one.layerId,
                                   std::move(one.textOverride));
    if (currentStyleId != kInvalidObjectId)
        document->restoreCurrentDimensionStyle(currentStyleId);
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
