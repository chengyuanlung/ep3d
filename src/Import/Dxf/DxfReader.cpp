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
    switch (insUnits) {
        case 0: return ImportedLengthUnit::Unitless;
        case 1: return ImportedLengthUnit::Inch;
        case 2: return ImportedLengthUnit::Foot;
        case 4: return ImportedLengthUnit::Millimeter;
        case 5: return ImportedLengthUnit::Centimeter;
        case 6: return ImportedLengthUnit::Meter;
        default: return ImportedLengthUnit::Unrecognized;
    }
}

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

    void finaliseUnits() {
        const std::optional<double> factor = MillimetresPerUnit(geometry.unit);
        if (factor.has_value()) {
            geometry.millimetresPerUnit = *factor;
            geometry.unitWasDefaulted = false;
        } else {
            // Documented default, and RECORDED as a default. Assuming a unit
            // silently is how a drawing arrives 25.4x wrong with nothing to
            // point at (ADR-M6-002).
            geometry.millimetresPerUnit = 1.0;
            geometry.unitWasDefaulted = true;
        }
    }

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
        // Resolved HERE, not after the read: libdxfrw delivers the header
        // before the entities, and every coordinate below is scaled by it.
        finaliseUnits();
    }

    void addLine(const DRW_Line& data) override {
        const double x1 = data.basePoint.x * scale();
        const double y1 = data.basePoint.y * scale();
        const double x2 = data.secPoint.x * scale();
        const double y2 = data.secPoint.y * scale();
        if (!AllFinite({x1, y1, x2, y2})) {
            note(ImportSkipReason::NonFiniteValue, "LINE",
                 "an endpoint coordinate is not a finite number");
            return;
        }
        // A zero-length line is degenerate geometry the Sketch would refuse, so
        // it is reported here rather than failing the whole import later
        // (spec 17 names this case explicitly).
        if (std::hypot(x2 - x1, y2 - y1) < kMinSketchDimensionMm) {
            note(ImportSkipReason::InvalidGeometry, "LINE",
                 "the line has zero length after unit conversion");
            return;
        }
        geometry.lines.push_back(ImportedLine2D{Vec2{x1, y1}, Vec2{x2, y2}});
    }

    void addCircle(const DRW_Circle& data) override {
        const double cx = data.basePoint.x * scale();
        const double cy = data.basePoint.y * scale();
        const double radius = data.radious * scale();
        if (!AllFinite({cx, cy, radius})) {
            note(ImportSkipReason::NonFiniteValue, "CIRCLE",
                 "the centre or radius is not a finite number");
            return;
        }
        if (radius < kMinSketchDimensionMm) {
            note(ImportSkipReason::InvalidGeometry, "CIRCLE",
                 "the radius is zero or negative after unit conversion");
            return;
        }
        geometry.circles.push_back(ImportedCircle2D{Vec2{cx, cy}, radius});
    }

    void addArc(const DRW_Arc& data) override {
        const double cx = data.basePoint.x * scale();
        const double cy = data.basePoint.y * scale();
        const double radius = data.radious * scale();
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
        if (radius < kMinSketchDimensionMm) {
            note(ImportSkipReason::InvalidGeometry, "ARC",
                 "the radius is zero or negative after unit conversion");
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
    virtual void addBlock(const DRW_Block& ) override {}
    virtual void setBlock(const int ) override {}
    virtual void endBlock() override {}
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
    double scale() const noexcept { return geometry.millimetresPerUnit; }

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
    // The header arrives before the entities, so the unit factor is known
    // before any coordinate is scaled -- but libdxfrw does not guarantee a
    // header at all, so the factor starts at the documented default and
    // addHeader only narrows it.
    collector.finaliseUnits();

    const bool ok = reader.read(&collector, /*ext=*/false);

    if (!ok) {
        result.error = DxfReadError::MalformedFile;
        result.message = "the file could not be parsed as DXF: " + path;
        return result;
    }

    result.geometry = std::move(collector.geometry);
    return result;
}

} // namespace paramcad
