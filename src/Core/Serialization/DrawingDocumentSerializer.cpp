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
        views.add(std::move(entry));
    }
    root.set("views", std::move(views));
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
            Sheet paper(sheetSize, sheetOrientation);
            if (sheetSize == SheetSize::Custom)
                paper.setCustomSize(customWidthMm, customHeightMm);
            if (one.positionMm.x < 0.0 || one.positionMm.y < 0.0 ||
                one.positionMm.x > paper.widthMm() || one.positionMm.y > paper.heightMm())
                return loadFailure(SerializationError::InvalidFieldType,
                                   context + ": this view sits off the sheet");
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
                           customHeightMm);
    for (auto& one : viewData)
        document->restoreView(one.id, std::move(one.name), ComputeState::Dirty,
                              std::move(one.sourcePath), std::move(one.bodyName), one.direction,
                              one.positionMm, one.scale, one.ownScale, one.showHidden,
                              one.showTangent);
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
