#include "Import/Dxf/DxfReader.h"

#include "Core/Sketch/SketchConstraint.h"

#include <drw_entities.h>
#include <drw_header.h>
#include <drw_interface.h>
#include <libdxfrw.h>

#include <cmath>
#include <initializer_list>
#include <filesystem>

// The ONLY translation unit that names libdxfrw (ADR-M6-001/003).
//
// libdxfrw is GPL-2.0-only, adopted by explicit owner decision. Everything it
// produces leaves this file as ImportedSketchGeometry, which names no library
// type -- so Core, the importer and every test can be built and run with the
// library absent, and replacing it later means rewriting this file and nothing
// else.

namespace paramcad {

namespace {

// DXF $INSUNITS. The values are fixed by the format, so they are written as the
// format defines them rather than inferred from a sample file.
ImportedLengthUnit UnitFromInsUnits(int insUnits) noexcept {
    // ALL of them. Mapping only 0,1,2,4,5,6 left mils (9), microinches (8),
    // microns (13), kilometres (7), yards (10), miles (3) and the rest falling
    // through to the millimetre default -- a PCB drawing 39.4x too large, a
    // survey drawing 1e6x too small -- each reported as "the file did not state
    // a usable unit" when the file had stated one perfectly clearly.
    switch (insUnits) {
        case 0: return ImportedLengthUnit::Unitless;
        case 1: return ImportedLengthUnit::Inch;
        case 2: return ImportedLengthUnit::Foot;
        case 3: return ImportedLengthUnit::Mile;
        case 4: return ImportedLengthUnit::Millimeter;
        case 5: return ImportedLengthUnit::Centimeter;
        case 6: return ImportedLengthUnit::Meter;
        case 7: return ImportedLengthUnit::Kilometre;
        case 8: return ImportedLengthUnit::Microinch;
        case 9: return ImportedLengthUnit::Mil;
        case 10: return ImportedLengthUnit::Yard;
        case 13: return ImportedLengthUnit::Micrometre;
        case 14: return ImportedLengthUnit::Decimetre;
        case 15: return ImportedLengthUnit::Decametre;
        case 16: return ImportedLengthUnit::Hectometre;
        // 11 (angstrom), 12 (nanometre), 17 (gigametre), 18 (astronomical
        // unit), 19 (light year), 20 (parsec) are defined by the format but
        // are not lengths a CAD part is modelled in; mapping them would invite
        // a 1e16x scale factor from a typo. Reported as unrecognised.
        default: return ImportedLengthUnit::Unrecognized;
    }
}

constexpr double kPi = 3.14159265358979323846;

// Smallest arc sweep that is still an arc. Matches the model's own rule in
// IsValidSketchGeometry, so the reader rejects exactly what the model would.
constexpr double kMinSweepRad = 1e-6;

bool AllFinite(std::initializer_list<double> values) noexcept {
    for (double value : values)
        if (!std::isfinite(value)) return false;
    return true;
}

// Collects entities as libdxfrw walks the file, converting to millimetres on
// the way in. This is the single unit-conversion boundary (ADR-M6-002): nothing
// downstream converts again, and nothing downstream may guess.
class Collector final : public DRW_Interface {
public:
    ImportedSketchGeometry geometry;

    // Applies the unit factor to everything collected, ONCE, after the whole
    // file has been read.
    //
    // Scaling each entity as it arrived depended on the HEADER reaching us
    // before the ENTITIES section. libdxfrw dispatches sections in FILE ORDER,
    // so a file that puts entities first was scaled by the 1.0 default and only
    // then learned the unit -- 25.4x wrong, while the result cheerfully
    // reported `unit = inches, unitWasDefaulted = false`. The one signal
    // ADR-M6-002 relies on to make that error visible was asserting the
    // opposite. A file with two HEADER sections split one sketch across two
    // scales.
    //
    // Collecting raw and scaling at the end removes the ordering assumption
    // rather than checking it, which is the only version that cannot be got
    // wrong by a file we have not seen.
    void finalise() {
        const std::optional<double> factor = MillimetresPerUnit(geometry.unit);
        if (factor.has_value()) {
            geometry.millimetresPerUnit = *factor;
            geometry.unitWasDefaulted = false;
        } else {
            // Documented default, and RECORDED as a default.
            geometry.millimetresPerUnit = 1.0;
            geometry.unitWasDefaulted = true;
        }

        const double k = geometry.millimetresPerUnit;
        for (ImportedLine2D& line : geometry.lines) {
            line.start.x *= k; line.start.y *= k;
            line.end.x *= k;   line.end.y *= k;
        }
        for (ImportedCircle2D& circle : geometry.circles) {
            circle.center.x *= k; circle.center.y *= k;
            circle.radiusMm *= k;
        }
        for (ImportedArc2D& arc : geometry.arcs) {
            arc.center.x *= k; arc.center.y *= k;
            arc.radiusMm *= k; // angles are not lengths
        }

        // The degenerate checks have to run on the SCALED values, because
        // "shorter than kMinSketchDimensionMm" is a statement about millimetres.
        auto tooSmall = [](double v) { return v < kMinSketchDimensionMm; };
        for (auto it = geometry.lines.begin(); it != geometry.lines.end();) {
            if (tooSmall(std::hypot(it->end.x - it->start.x, it->end.y - it->start.y))) {
                note(ImportSkipReason::InvalidGeometry, "LINE",
                     "the line has zero length after unit conversion");
                it = geometry.lines.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = geometry.circles.begin(); it != geometry.circles.end();) {
            if (tooSmall(it->radiusMm)) {
                note(ImportSkipReason::InvalidGeometry, "CIRCLE",
                     "the radius is zero or negative after unit conversion");
                it = geometry.circles.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = geometry.arcs.begin(); it != geometry.arcs.end();) {
            if (tooSmall(it->radiusMm)) {
                note(ImportSkipReason::InvalidGeometry, "ARC",
                     "the radius is zero or negative after unit conversion");
                it = geometry.arcs.erase(it);
            } else {
                ++it;
            }
        }
    }

    // BLOCK DEFINITIONS ARE NOT MODEL GEOMETRY.
    //
    // libdxfrw dispatches a block's contents through the very same
    // addLine/addCircle/addArc it uses for model space, so without this the
    // collector could not tell them apart -- and it did not. A file whose
    // ENTITIES section held only an INSERT imported the block's geometry at
    // DEFINITION coordinates, losing placement, scale, rotation and
    // multiplicity, while reporting the INSERT itself as "skipped".
    //
    // The realistic case is worse: every DXF carrying a DIMENSION also carries
    // an anonymous *D1... block holding its dimension and extension lines. Those
    // annotation lines were being imported as model geometry -- exactly what
    // spec 4 says must not silently happen.
    void addBlock(const DRW_Block&) override { inBlock_ = true; }
    void setBlock(const int) override {}
    void endBlock() override { inBlock_ = false; }

    void addHeader(const DRW_Header* data) override {
        if (data == nullptr) return;
        // Read through the public `vars` map. DRW_Header::getInt is private in
        // this version, and reaching past that would tie us to one release of a
        // library we already intend to be replaceable (ADR-M6-001).
        const auto it = data->vars.find("$INSUNITS");
        if (it != data->vars.end() && it->second != nullptr &&
            it->second->type() == DRW_Variant::INTEGER) {
            geometry.unit = UnitFromInsUnits(static_cast<int>(it->second->content.i));
        }
    }

    void addLine(const DRW_Line& data) override {
        if (skipBlockContent("LINE")) return;
        const double x1 = data.basePoint.x;
        const double y1 = data.basePoint.y;
        const double x2 = data.secPoint.x;
        const double y2 = data.secPoint.y;
        if (!AllFinite({x1, y1, x2, y2})) {
            note(ImportSkipReason::NonFiniteValue, "LINE",
                 "an endpoint coordinate is not a finite number");
            return;
        }
        geometry.lines.push_back(ImportedLine2D{Vec2{x1, y1}, Vec2{x2, y2}});
    }

    void addCircle(const DRW_Circle& data) override {
        if (skipBlockContent("CIRCLE")) return;
        if (skipNonDefaultExtrusion("CIRCLE", data.extPoint)) return;
        const double cx = data.basePoint.x;
        const double cy = data.basePoint.y;
        const double radius = data.radious;
        if (!AllFinite({cx, cy, radius})) {
            note(ImportSkipReason::NonFiniteValue, "CIRCLE",
                 "the centre or radius is not a finite number");
            return;
        }
        geometry.circles.push_back(ImportedCircle2D{Vec2{cx, cy}, radius});
    }

    void addArc(const DRW_Arc& data) override {
        if (skipBlockContent("ARC")) return;
        if (skipNonDefaultExtrusion("ARC", data.extPoint)) return;
        const double cx = data.basePoint.x;
        const double cy = data.basePoint.y;
        const double radius = data.radious;
        // libdxfrw's header comments say codes 50/51 arrive in RADIANS even
        // though the file stores degrees. That claim is not taken on trust: an
        // arc fixture with deliberately nontrivial angles is measured
        // geometrically, and the test fails if the units are wrong either way.
        const double start = data.staangle;
        const double end = data.endangle;
        if (!AllFinite({cx, cy, radius, start, end})) {
            note(ImportSkipReason::NonFiniteValue, "ARC",
                 "the centre, radius or an angle is not a finite number");
            return;
        }
        // The SWEEP, which nothing checked. A 0 -> 360 arc is legal DXF and
        // several exporters emit one instead of a CIRCLE; the model refuses a
        // normalised sweep of 0, so such an arc reached the importer, failed
        // its validation, and rolled back the WHOLE file -- 500 good lines
        // discarded with it. Every other degenerate entity is skipped and
        // reported (ADR-M6-005); the sweep was the one hole in that rule.
        double sweep = end - start;
        while (sweep < 0.0) sweep += 2.0 * kPi;
        while (sweep >= 2.0 * kPi) sweep -= 2.0 * kPi;
        if (sweep < kMinSweepRad || sweep > 2.0 * kPi - kMinSweepRad) {
            note(ImportSkipReason::InvalidGeometry, "ARC",
                 "the arc has no sweep (start and end angles coincide)");
            return;
        }
        geometry.arcs.push_back(ImportedArc2D{Vec2{cx, cy}, radius, start, end});
    }

    virtual void addLType(const DRW_LType& ) override {}
    virtual void addLayer(const DRW_Layer& ) override {}
    virtual void addDimStyle(const DRW_Dimstyle& ) override {}
    virtual void addVport(const DRW_Vport& ) override {}
    virtual void addTextStyle(const DRW_Textstyle& ) override {}
    virtual void addAppId(const DRW_AppId& ) override {}
    virtual void addPoint(const DRW_Point& data) override { noteUnsupported("POINT"); }
    virtual void addRay(const DRW_Ray& data) override { noteUnsupported("RAY"); }
    virtual void addXline(const DRW_Xline& data) override { noteUnsupported("XLINE"); }
    virtual void addEllipse(const DRW_Ellipse& data) override { noteUnsupported("ELLIPSE"); }
    virtual void addLWPolyline(const DRW_LWPolyline& data) override { noteUnsupported("LWPOLYLINE"); }
    virtual void addPolyline(const DRW_Polyline& data) override { noteUnsupported("POLYLINE"); }
    virtual void addSpline(const DRW_Spline* data) override { noteUnsupported("SPLINE"); }
    virtual void addKnot(const DRW_Entity& ) override {}
    virtual void addInsert(const DRW_Insert& data) override { noteUnsupported("INSERT"); }
    virtual void addTrace(const DRW_Trace& data) override { noteUnsupported("TRACE"); }
    virtual void add3dFace(const DRW_3Dface& data) override { noteUnsupported("3DFACE"); }
    virtual void addSolid(const DRW_Solid& data) override { noteUnsupported("SOLID"); }
    virtual void addMText(const DRW_MText& data) override { noteUnsupported("MTEXT"); }
    virtual void addText(const DRW_Text& data) override { noteUnsupported("TEXT"); }
    virtual void addDimAlign(const DRW_DimAligned *data) override { noteUnsupported("DIMENSION"); }
    virtual void addDimLinear(const DRW_DimLinear *data) override { noteUnsupported("DIMENSION"); }
    virtual void addDimRadial(const DRW_DimRadial *data) override { noteUnsupported("DIMENSION"); }
    virtual void addDimDiametric(const DRW_DimDiametric *data) override { noteUnsupported("DIMENSION"); }
    virtual void addDimAngular(const DRW_DimAngular *data) override { noteUnsupported("DIMENSION"); }
    virtual void addDimAngular3P(const DRW_DimAngular3p *data) override { noteUnsupported("DIMENSION"); }
    virtual void addDimOrdinate(const DRW_DimOrdinate *data) override { noteUnsupported("DIMENSION"); }
    virtual void addLeader(const DRW_Leader *data) override { noteUnsupported("LEADER"); }
    virtual void addHatch(const DRW_Hatch *data) override { noteUnsupported("HATCH"); }
    virtual void addViewport(const DRW_Viewport& data) override { noteUnsupported("VIEWPORT"); }
    virtual void addImage(const DRW_Image *data) override { noteUnsupported("IMAGE"); }
    virtual void linkImage(const DRW_ImageDef *) override {}
    virtual void addComment(const char* ) override {}
    virtual void addPlotSettings(const DRW_PlotSettings *) override {}
    virtual void writeHeader(DRW_Header& data) override {}
    virtual void writeBlocks() override {}
    virtual void writeBlockRecords() override {}
    virtual void writeEntities() override {}
    virtual void writeLTypes() override {}
    virtual void writeLayers() override {}
    virtual void writeTextstyles() override {}
    virtual void writeVports() override {}
    virtual void writeDimstyles() override {}
    virtual void writeObjects() override {}
    virtual void writeAppId() override {}

private:
    bool inBlock_{false};

    // True when this entity belongs to a BLOCK DEFINITION rather than model
    // space. Reported once per block rather than once per entity, so a
    // dimension's three annotation lines do not produce three identical rows.
    bool skipBlockContent(const char* kind) {
        if (!inBlock_) return false;
        if (!blockReported_) {
            blockReported_ = true;
            note(ImportSkipReason::UnsupportedEntity, "BLOCK",
                 "block definitions are not imported; their contents are not "
                 "model geometry and their placement (INSERT) is not applied");
        }
        (void)kind;
        return true;
    }

    // DXF stores CIRCLE and ARC in the entity's own coordinate system (OCS),
    // defined by the extrusion vector in group code 210, while LINE stores
    // world coordinates. Ignoring 210 therefore MIXES TWO FRAMES inside one
    // file: with the common (0,0,-1) the correct result is a MIRROR, so a hole
    // at (25,30) silently imported at (-25,30) -- and a mirrored part has
    // identical area, volume and mass, so no analytical oracle can catch it.
    //
    // Applying an arbitrary OCS plane is a modelling decision beyond M6's
    // scope, so entities carrying a non-default extrusion are SKIPPED AND
    // REPORTED. Refusing to import is recoverable; importing something
    // mirrored and calling it success is not.
    bool skipNonDefaultExtrusion(const char* kind, const DRW_Coord& extrusion) {
        const bool isDefault = std::fabs(extrusion.x) < 1e-12 &&
                               std::fabs(extrusion.y) < 1e-12 &&
                               std::fabs(extrusion.z - 1.0) < 1e-12;
        if (isDefault) return false;
        note(ImportSkipReason::UnsupportedEntity, kind,
             "the entity carries a non-default extrusion direction (DXF code "
             "210); importing it would silently mirror or reorient it");
        return true;
    }

    bool blockReported_{false};

    void note(ImportSkipReason reason, const char* kind, const char* detail) {
        geometry.skipped.push_back(ImportedSkip{reason, kind, detail});
    }

    // An entity kind M6 does not import. Reported, never reinterpreted as
    // another kind (spec 4).
    void noteUnsupported(const char* kind) {
        geometry.skipped.push_back(
            ImportedSkip{ImportSkipReason::UnsupportedEntity, kind,
                         "M6 imports LINE, CIRCLE and ARC; this entity was skipped"});
    }
};

} // namespace

const char* DxfReadErrorName(DxfReadError error) noexcept {
    switch (error) {
        case DxfReadError::None: return "no error";
        case DxfReadError::FileNotFound: return "file not found";
        case DxfReadError::NotReadable: return "file could not be read";
        case DxfReadError::MalformedFile: return "malformed DXF";
    }
    return "unknown";
}

DxfReadResult ReadDxfFile(const std::string& path) {
    DxfReadResult result;

    // Checked before handing the path to the parser, so "there is no such file"
    // is distinguishable from "the file is not DXF" -- spec 11 requires the
    // cause, and a parser that returns false for both cannot supply it.
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        result.error = DxfReadError::FileNotFound;
        result.message = "no such file: " + path;
        return result;
    }
    if (std::filesystem::is_directory(path, ec)) {
        result.error = DxfReadError::NotReadable;
        result.message = "not a file: " + path;
        return result;
    }

    Collector collector;
    dxfRW reader(path.c_str());
    const bool ok = reader.read(&collector, /*ext=*/false);
    // Units applied ONCE, here, after the whole file has been seen -- so the
    // result cannot depend on whether HEADER preceded ENTITIES.
    collector.finalise();

    if (!ok) {
        result.error = DxfReadError::MalformedFile;
        result.message = "the file could not be parsed as DXF: " + path;
        return result;
    }

    result.geometry = std::move(collector.geometry);
    return result;
}

} // namespace paramcad
