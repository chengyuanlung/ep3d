#include "Core/Export/DxfWriter.h"

#include "Core/Drawing/Geometry2D.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <variant>

namespace paramcad {

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kPi = kTwoPi / 2.0;

// A DXF is PAIRS OF LINES: a group code, then its value. Everything below goes
// through these two, so no call site has to remember the shape.
void Pair(std::ostream& out, int code, const std::string& value) {
    out << code << "\n" << value << "\n";
}

void Pair(std::ostream& out, int code, int value) {
    out << code << "\n" << value << "\n";
}

void Pair(std::ostream& out, int code, double value) {
    // SIX DECIMALS, FIXED. DXF has no notion of significant figures, and an
    // exponent form ("1e-05") is read by some programs and not by others --
    // the ones that fail do it silently, by treating the entity as malformed
    // and dropping it.
    std::ostringstream text;
    text << std::fixed << std::setprecision(6) << value;
    out << code << "\n" << text.str() << "\n";
}

double Degrees(double radians) noexcept { return radians * 180.0 / kPi; }

// WHAT THE ENTITIES ACTUALLY USED.
//
// An entity naming a layer or a linetype the tables do not declare is a file
// most readers either reject or silently repair -- and the silent repair is
// worse, because it puts the drawing on the wrong layer without a word.
//
// The first draft declared the DOCUMENT's tables and then wrote views on a
// layer named after the view and hidden edges on a linetype called HIDDEN,
// neither of which was in either table. That is the same "two things that must
// agree, kept by hand" defect the comment above it claimed to prevent -- so
// now the entities are written to a buffer that RECORDS every name as it goes,
// and the tables are built from what it recorded. A name cannot be emitted
// without being declared, because the same call does both.
struct UsedNames {
    std::vector<std::string> layers;
    std::vector<std::string> linetypes;

    void use(std::vector<std::string>& into, const std::string& name) {
        if (name.empty()) return;
        for (const std::string& already : into)
            if (already == name) return;
        into.push_back(name);
    }
    void useLayer(const std::string& name) { use(layers, name); }
    void useLinetype(const std::string& name) { use(linetypes, name); }
};

// The three codes every entity carries.
void EntityHead(std::ostream& out, UsedNames& used, const char* kind, const std::string& layer,
                int color, const std::string& linetype) {
    Pair(out, 0, std::string(kind));
    Pair(out, 8, layer);
    used.useLayer(layer);
    // ByLayer is the ABSENCE of an override in DXF, so 256 is written as
    // nothing at all rather than as the number -- some readers take a literal
    // 256 as an unknown colour and fall back to white.
    if (color != kColorByLayer) Pair(out, 62, color);
    if (!linetype.empty() && linetype != "BYLAYER") {
        Pair(out, 6, linetype);
        used.useLinetype(linetype);
    }
}

// The dash pattern for a linetype the DOCUMENT does not define -- which is how
// HIDDEN gets into a file whose drawing never declared it: the writer emits it
// for a view's hidden edges, because a DXF has no "this edge is hidden" flag
// and the convention IS the meaning.
//
// ISO 128's dashed line, in millimetres.
std::vector<double> FallbackPattern(const std::string& name) {
    if (name == "HIDDEN") return {4.0, -2.0};
    if (name == "CENTER" || name == "CENTRE") return {12.0, -2.0, 2.0, -2.0};
    if (name == "PHANTOM") return {12.0, -2.0, 2.0, -2.0, 2.0, -2.0};
    return {};
}

} // namespace

DxfWriteResult WriteDxf(const DrawingDocument& document, std::ostream& out) {
    DxfWriteResult result;
    if (!out) {
        result.why = "that file could not be opened for writing";
        return result;
    }

    // THE ENTITIES ARE WRITTEN FIRST, into a buffer, so the tables below can
    // be built from the names they actually used rather than from a second
    // list kept by hand.
    UsedNames used;
    std::ostringstream entities;

    // --- The comment a reader sees first -------------------------------------
    //
    // WHAT THIS FILE IS NOT is worth saying at the top. A DXF cannot carry a
    // view's associativity, and whoever opens this needs to know the curves
    // are a picture of the model rather than a link to it.
    Pair(out, 999, std::string("EP3D drawing, exported to DXF R12"));
    Pair(out, 999,
         std::string("Views are flattened: the curves are where they sit on the paper, at "
                     "the scale they were drawn, and no longer follow the model."));

    // --- HEADER ---------------------------------------------------------------
    Pair(out, 0, std::string("SECTION"));
    Pair(out, 2, std::string("HEADER"));
    Pair(out, 9, std::string("$ACADVER"));
    Pair(out, 1, std::string("AC1009"));
    // MILLIMETRES, SAID OUT LOUD. A DXF without $INSUNITS is a file whose
    // numbers mean whatever the program that opens it assumes -- and the
    // reader on the other side of this project already has a whole enum for
    // how badly that goes (ImportedLengthUnit).
    Pair(out, 9, std::string("$INSUNITS"));
    Pair(out, 70, 4);
    Pair(out, 9, std::string("$EXTMIN"));
    Pair(out, 10, 0.0);
    Pair(out, 20, 0.0);
    Pair(out, 9, std::string("$EXTMAX"));
    Pair(out, 10, document.sheet().widthMm());
    Pair(out, 20, document.sheet().heightMm());
    Pair(out, 0, std::string("ENDSEC"));

    // EVERY LAYER THE DRAWING DECLARES is used, whether an entity sits on it
    // or not: a recipient who opens this to draw on it needs the layers the
    // author set up, not just the ones that happened to be occupied.
    for (const Layer* layer : document.layers()) used.useLayer(layer->name());
    for (const Linetype* linetype : document.linetypes())
        used.useLinetype(linetype->name());

    // --- ENTITIES (into the buffer) -------------------------------------------
    const auto line = [&](Vec2 a, Vec2 b, const std::string& layer, int color,
                          const std::string& linetype) {
        EntityHead(entities, used, "LINE", layer, color, linetype);
        Pair(entities, 10, a.x);
        Pair(entities, 20, a.y);
        Pair(entities, 30, 0.0);
        Pair(entities, 11, b.x);
        Pair(entities, 21, b.y);
        Pair(entities, 31, 0.0);
        ++result.entities;
    };

    const auto arc = [&](Vec2 centre, double radius, double startRad, double endRad,
                         const std::string& layer, int color, const std::string& linetype) {
        EntityHead(entities, used, "ARC", layer, color, linetype);
        Pair(entities, 10, centre.x);
        Pair(entities, 20, centre.y);
        Pair(entities, 30, 0.0);
        Pair(entities, 40, radius);
        // DXF ARCS ARE ALWAYS COUNTER-CLOCKWISE from start to end, in DEGREES.
        // Both halves of that matter: the project works in radians and stores
        // arcs counter-clockwise already (ADR-M5-006), so this is a unit
        // conversion and not a reversal.
        Pair(entities, 50, Degrees(startRad));
        Pair(entities, 51, Degrees(endRad));
        ++result.entities;
    };

    const auto circle = [&](Vec2 centre, double radius, const std::string& layer, int color,
                            const std::string& linetype) {
        EntityHead(entities, used, "CIRCLE", layer, color, linetype);
        Pair(entities, 10, centre.x);
        Pair(entities, 20, centre.y);
        Pair(entities, 30, 0.0);
        Pair(entities, 40, radius);
        ++result.entities;
    };

    // A polyline, with its bulges intact -- R12's POLYLINE/VERTEX/SEQEND
    // rather than LWPOLYLINE, which R12 does not have.
    const auto polyline = [&](const std::vector<DrawVertex>& vertices, bool closed,
                              const std::string& layer, int color,
                              const std::string& linetype) {
        EntityHead(entities, used, "POLYLINE", layer, color, linetype);
        Pair(entities, 66, 1); // vertices follow -- required in R12
        Pair(entities, 10, 0.0);
        Pair(entities, 20, 0.0);
        Pair(entities, 30, 0.0);
        Pair(entities, 70, closed ? 1 : 0);
        for (const DrawVertex& vertex : vertices) {
            Pair(entities, 0, std::string("VERTEX"));
            Pair(entities, 8, layer);
            Pair(entities, 10, vertex.at.x);
            Pair(entities, 20, vertex.at.y);
            Pair(entities, 30, 0.0);
            // THE BULGE SURVIVES. Flattening it here would turn every arc
            // segment a drafter drew into a chord, and the recipient could
            // never get it back.
            if (std::fabs(vertex.bulge) > 1e-12) Pair(entities, 42, vertex.bulge);
        }
        Pair(entities, 0, std::string("SEQEND"));
        Pair(entities, 8, layer);
        ++result.entities;
    };

    bool saidEllipse = false;
    for (const DrawingEntity* entity : document.entities()) {
        const std::string layer =
            document.findLayer(entity->layerId()) != nullptr
                ? document.findLayer(entity->layerId())->name()
                : std::string(kDefaultLayerName);
        const int color = entity->color();
        const std::string linetype = entity->linetype();

        if (const auto* shape = std::get_if<DrawLine>(&entity->shape())) {
            line(shape->a, shape->b, layer, color, linetype);
        } else if (const auto* shape = std::get_if<DrawCircle>(&entity->shape())) {
            circle(shape->centre, shape->radius, layer, color, linetype);
        } else if (const auto* shape = std::get_if<DrawArc>(&entity->shape())) {
            arc(shape->centre, shape->radius, shape->startAngle, shape->endAngle, layer, color,
                linetype);
        } else if (const auto* shape = std::get_if<DrawPolyline>(&entity->shape())) {
            polyline(shape->vertices, shape->closed, layer, color, linetype);
        } else if (const auto* shape = std::get_if<DrawPoint>(&entity->shape())) {
            EntityHead(entities, used, "POINT", layer, color, linetype);
            Pair(entities, 10, shape->at.x);
            Pair(entities, 20, shape->at.y);
            Pair(entities, 30, 0.0);
            ++result.entities;
        } else if (const auto* shape = std::get_if<DrawText>(&entity->shape())) {
            EntityHead(entities, used, "TEXT", layer, color, linetype);
            Pair(entities, 10, shape->at.x);
            Pair(entities, 20, shape->at.y);
            Pair(entities, 30, 0.0);
            Pair(entities, 40, shape->heightMm);
            Pair(entities, 1, shape->text);
            if (std::fabs(shape->rotation) > 1e-12) Pair(entities, 50, Degrees(shape->rotation));
            ++result.entities;
        } else {
            // AN ELLIPSE. R12 HAS NO ENTITY FOR ONE.
            //
            // Written as a closed polyline through the same flattening the
            // canvas uses, and RECORDED AS A LOSS -- a file that opened
            // cleanly and is subtly not the drawing is worse than one that
            // said what it could not carry, because nobody re-checks a file
            // that opened.
            const std::vector<Vec2> points = entity->flatten(0.02);
            std::vector<DrawVertex> vertices;
            vertices.reserve(points.size());
            for (const Vec2 point : points) vertices.push_back(DrawVertex{point, 0.0});
            polyline(vertices, true, layer, color, linetype);
            if (!saidEllipse) {
                result.losses.push_back(
                    DxfWriteLoss{"ELLIPSE",
                                 "R12 has no ellipse entity, so it was written as a closed "
                                 "polyline within 0.02 mm of the true curve"});
                saidEllipse = true;
            }
        }
    }

    // --- The views, FLATTENED INTO PLACE --------------------------------------
    for (const DrawingView* view : document.views()) {
        if (view->currentState() != ComputeState::Valid) continue;
        const double factor = document.viewScaleFactor(view->id());
        // THROUGH THE DOCUMENT, not worked out here. Projected curves are in
        // model millimetres (ProjectedGeometry.h) and a DXF is in sheet
        // millimetres, and there is exactly one function that converts.
        const auto place = [&](Vec2 modelMm) {
            return document.viewPointToSheetMm(view->id(), modelMm);
        };
        // A LAYER PER VIEW, named after it. A recipient who wants the hidden
        // detail off needs a switch, and "everything on layer 0" is not one.
        const std::string layer = view->name();

        for (const ProjectedCurve& curve : view->projected().curves) {
            // HIDDEN LINES GO ON A DASHED LINETYPE, which is the convention
            // carrying the meaning -- a DXF has no "this edge is hidden" flag,
            // so the only way to say it is the way a drawing says it.
            const std::string linetype =
                curve.visibility == ProjectedVisibility::Hidden ? "HIDDEN" : "";
            if (const auto* shape = std::get_if<ProjectedLine>(&curve.shape)) {
                line(place(shape->a), place(shape->b), layer, kColorByLayer, linetype);
            } else if (const auto* shape = std::get_if<ProjectedArc>(&curve.shape)) {
                if (shape->isFullCircle)
                    circle(place(shape->centre), shape->radius * factor, layer, kColorByLayer,
                           linetype);
                else
                    arc(place(shape->centre), shape->radius * factor, shape->startAngle,
                        shape->endAngle, layer, kColorByLayer, linetype);
            } else if (const auto* shape = std::get_if<ProjectedPolyline>(&curve.shape)) {
                std::vector<DrawVertex> vertices;
                vertices.reserve(shape->points.size());
                for (const Vec2 point : shape->points)
                    vertices.push_back(DrawVertex{place(point), 0.0});
                polyline(vertices, false, layer, kColorByLayer, linetype);
            }
        }
    }

    // --- The dimensions -------------------------------------------------------
    //
    // WRITTEN WITH THEIR MEASURED VALUE, and that is a real loss stated
    // plainly: DXF's associativity does not survive a round trip through most
    // programs anyway, and a dimension that arrived carrying no number would
    // read as blank on the recipient's screen.
    bool saidDimension = false;
    for (const DrawingDimension* dimension : document.dimensions()) {
        const DimensionMeasurement measured = document.measure(*dimension);
        // A DANGLING DIMENSION IS NOT WRITTEN. Exporting one would put a "<?>"
        // on somebody else's drawing with no way for them to find out what it
        // was meant to measure.
        if (!measured.ok) {
            result.losses.push_back(DxfWriteLoss{
                "DIMENSION", "a dimension that had lost what it measured was not written"});
            continue;
        }
        const std::string layer = document.findLayer(dimension->layerId()) != nullptr
                                      ? document.findLayer(dimension->layerId())->name()
                                      : std::string(kDefaultLayerName);
        EntityHead(entities, used, "DIMENSION", layer, kColorByLayer, "");
        // R12 wants the block that draws it; an empty name means "the reader
        // draws it from the style", which every program in practice does.
        Pair(entities, 2, std::string("*D") + std::to_string(result.entities));
        Pair(entities, 10, dimension->linePositionMm().x);
        Pair(entities, 20, dimension->linePositionMm().y);
        Pair(entities, 30, 0.0);
        // 13/23 and 14/24 are the EXTENSION LINE ORIGINS -- the points on the
        // geometry, which is exactly what our own reader matches against.
        Pair(entities, 13, measured.firstMm.x);
        Pair(entities, 23, measured.firstMm.y);
        Pair(entities, 33, 0.0);
        Pair(entities, 14, measured.secondMm.x);
        Pair(entities, 24, measured.secondMm.y);
        Pair(entities, 34, 0.0);
        int type = 0; // rotated / horizontal / vertical
        switch (dimension->kind()) {
            case DimensionKind::Linear:
                type = dimension->direction() == LinearDirection::Aligned ? 1 : 0;
                break;
            case DimensionKind::Angular: type = 2; break;
            case DimensionKind::Diameter: type = 3; break;
            case DimensionKind::Radius: type = 4; break;
        }
        Pair(entities, 70, type);
        if (dimension->direction() == LinearDirection::Vertical) Pair(entities, 50, 90.0);
        else Pair(entities, 50, 0.0);
        // THE MEASUREMENT, as the file's own cross-check field.
        Pair(entities, 42, measured.valueMm);
        if (!dimension->textOverride().empty()) Pair(entities, 1, dimension->textOverride());
        ++result.entities;
        if (!saidDimension) {
            result.losses.push_back(DxfWriteLoss{
                "DIMENSION",
                "dimensions carry their measured value rather than a link to the geometry: "
                "a recipient's program will show the number, not follow the model"});
            saidDimension = true;
        }
    }

    Pair(entities, 0, std::string("ENDSEC"));

    // --- TABLES, from what the entities used ----------------------------------
    //
    // Layer "0" and CONTINUOUS are declared whether anything used them or not:
    // DXF requires both to exist, and a reader that meets a file without them
    // has nothing to fall back on.
    used.useLayer(std::string(kDefaultLayerName));
    used.useLinetype(std::string(kContinuousLinetypeName));

    Pair(out, 0, std::string("SECTION"));
    Pair(out, 2, std::string("TABLES"));

    Pair(out, 0, std::string("TABLE"));
    Pair(out, 2, std::string("LTYPE"));
    Pair(out, 70, static_cast<int>(used.linetypes.size()));
    for (const std::string& name : used.linetypes) {
        const Linetype* known = document.findLinetypeNamed(name);
        // A LINETYPE THE DRAWING NEVER DECLARED still has to be in the file if
        // an entity names it. HIDDEN is the case that matters: the writer uses
        // it for a view's hidden edges because a DXF has no flag for those.
        const std::vector<double> pattern =
            known != nullptr ? known->pattern() : FallbackPattern(name);
        double length = 0.0;
        for (const double segment : pattern) length += std::fabs(segment);

        Pair(out, 0, std::string("LTYPE"));
        Pair(out, 2, name);
        Pair(out, 70, 0);
        Pair(out, 3, known != nullptr ? known->description() : std::string());
        Pair(out, 72, 65); // 'A', the only alignment R12 defines
        Pair(out, 73, static_cast<int>(pattern.size()));
        Pair(out, 40, length);
        // THE SIGNS ARE THE PATTERN. Positive is a dash, negative is a gap,
        // zero is a dot -- DXF's own convention, which is why M33 stored them
        // this way instead of as pairs.
        for (const double segment : pattern) Pair(out, 49, segment);
    }
    Pair(out, 0, std::string("ENDTAB"));

    Pair(out, 0, std::string("TABLE"));
    Pair(out, 2, std::string("LAYER"));
    Pair(out, 70, static_cast<int>(used.layers.size()));
    for (const std::string& name : used.layers) {
        const Layer* known = document.findLayerNamed(name);
        Pair(out, 0, std::string("LAYER"));
        Pair(out, 2, name);
        // FROZEN IS A FLAG; OFF IS A NEGATIVE COLOUR. Two different mechanisms
        // for two different things, which is DXF's own arrangement and the
        // reason M33 kept three separate flags rather than one "visible".
        Pair(out, 70, known != nullptr && known->isFrozen() ? 1 : 0);
        const int color = known != nullptr ? known->color() : 7;
        Pair(out, 62, known == nullptr || known->isOn() ? color : -color);
        Pair(out, 6, known != nullptr ? known->linetype()
                                      : std::string(kContinuousLinetypeName));
    }
    Pair(out, 0, std::string("ENDTAB"));
    Pair(out, 0, std::string("ENDSEC"));

    // ...and only now the entities themselves.
    Pair(out, 0, std::string("SECTION"));
    Pair(out, 2, std::string("ENTITIES"));
    out << entities.str();
    Pair(out, 0, std::string("EOF"));

    if (!out) {
        result.why = "writing stopped part way through";
        return result;
    }
    result.ok = true;
    return result;
}

DxfWriteResult WriteDxfFile(const DrawingDocument& document, const std::string& path) {
    DxfWriteResult result;
    if (path.empty()) {
        result.why = "a DXF needs somewhere to go";
        return result;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        result.why = "that file could not be opened for writing";
        return result;
    }
    result = WriteDxf(document, out);
    out.close();
    // CHECKED AFTER THE CLOSE. A stream that looked fine and failed on flush
    // leaves a truncated file that opens and is missing its last entities --
    // the failure nobody notices until the part is made short.
    //
    // NO TEST FORCES THIS ONE. A flush failure means a full disk or a
    // disconnected share, and neither can be produced from a unit test here;
    // the mid-write failure it mirrors IS tested, through the stream overload
    // above with a buffer that fails part way (M35_DXF_012). A mutation
    // deleting these three lines survives, and saying so is better than
    // writing a test that could only pass.
    if (result.ok && !out) {
        result.ok = false;
        result.why = "the file could not be finished";
    }
    return result;
}

} // namespace paramcad
