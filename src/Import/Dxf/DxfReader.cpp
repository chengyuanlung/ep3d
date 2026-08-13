#include "Import/Dxf/DxfReader.h"

#include "Core/Sketch/SketchConstraint.h"

#include <drw_entities.h>
#include <drw_header.h>
#include <drw_interface.h>
#include <libdxfrw.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>
#include <initializer_list>
#include <type_traits>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>

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

// Two facts about the file that the callback interface cannot report, read in
// one pass over the group codes.
//
// This is not a second DXF parser. It reads code/value pairs, tracks which
// section it is in, and answers exactly two questions -- both of them questions
// libdxfrw structurally cannot be asked. It returns `Unknown` for binary DXF
// rather than guessing.
//
// FACT 1: does the BLOCKS section close every BLOCK it opens?
//
// This exists because the callback interface CANNOT answer the question. A
// BLOCK with no ENDBLK makes libdxfrw read the whole rest of the file as block
// content -- the entire ENTITIES section disappears -- and then call endBlock()
// at EOF, so an `inBlock_ still true` check at the end never fires. I wrote
// that check first and measured it doing nothing; the observable symptom is a
// SUCCESSFUL read with zero geometry and one ordinary "block definitions are
// not imported" note, which aims the user at their geometry when the fault is
// one missing group code in a section they never open.
//
// `BLOCK_RECORD` in TABLES does not collide -- the comparison is for equality,
// not prefix.
//
// FACT 2: does any model-space LINE carry a start point but no end point?
//
// libdxfrw default-initialises a missing second point to (0,0,0) and exposes no
// group-code-presence flag, so a LINE with codes 10/20 and no 11/21 arrives
// indistinguishable from a real line drawn to the origin. It imported silently:
// a phantom entity, a success message, and a profile that opens or branches.
// spec 4 says nothing may be silently misinterpreted, and this was the one place
// left where something was.
//
// This was accepted as a limitation for two rounds on the stated grounds that
// "detecting it needs a group-code-presence hook the library does not offer".
// That reason stopped being true the moment FACT 1 forced a group-code scan to
// exist: the hook is not needed, because the file says so directly.
enum class BlocksSectionState { Unknown, Terminated, Unterminated };

struct DxfStructureScan {
    BlocksSectionState blocks{BlocksSectionState::Unknown};
    // ENTITIES-section LINEs whose second point is incomplete, recorded as the
    // whole phantom the library will synthesise: {start, end}, where each
    // missing component of the end is the 0.0 libdxfrw default-initialises it
    // to. Storing the START alone was not enough -- a LINE with code 11 and no
    // 21 becomes (x11, 0), not the origin -- and matching on the start alone
    // erases whichever line comes first, which can be a real one that happens
    // to share the start point.
    //
    // Matched by value rather than by index because addLine can decline an
    // entity (a non-finite coordinate), which would slide every later index by
    // one and delete the wrong line.
    struct Phantom { double sx, sy, ex, ey; };
    std::vector<Phantom> truncatedLines;
};

DxfStructureScan ScanDxfStructure(const std::string& path) {
    DxfStructureScan scan;
    std::ifstream in(path, std::ios::binary);
    if (!in) return scan;

    // Binary DXF has no group-code lines to read. Untested and documented as
    // such; reporting Unknown is honest, guessing would not be.
    char magic[22] = {};
    in.read(magic, 21);
    if (std::string(magic, static_cast<size_t>(in.gcount())).rfind("AutoCAD Binary DXF", 0) == 0)
        return scan;
    in.clear();
    in.seekg(0);

    const auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
            s.pop_back();
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        return s.substr(i);
    };

    enum class Section { Other, Blocks, Entities };
    Section section = Section::Other;
    bool sectionNameExpected = false;
    int depth = 0;
    bool blocksSeen = false;

    // State for the LINE record currently being read, if any.
    bool inLine = false;
    bool has10 = false, has20 = false, has11 = false, has21 = false;
    double x10 = 0.0, y10 = 0.0, x11 = 0.0, y11 = 0.0;

    const auto closeLineRecord = [&] {
        if (inLine && has10 && has20 && !(has11 && has21)) {
            // Whatever half of the second point is present is kept: the phantom
            // to look for is exactly what the library will have built.
            scan.truncatedLines.push_back(
                DxfStructureScan::Phantom{x10, y10, has11 ? x11 : 0.0,
                                          has21 ? y11 : 0.0});
        }
        inLine = false;
        has10 = has20 = has11 = has21 = false;
        x10 = y10 = x11 = y11 = 0.0;
    };

    std::string code;
    std::string value;
    while (std::getline(in, code) && std::getline(in, value)) {
        const std::string c = trim(std::move(code));
        const std::string v = trim(std::move(value));

        if (sectionNameExpected && c == "2") {
            sectionNameExpected = false;
            if (v == "BLOCKS") {
                section = Section::Blocks;
                blocksSeen = true;
                depth = 0;
            } else if (v == "ENTITIES") {
                section = Section::Entities;
            } else {
                section = Section::Other;
            }
            continue;
        }

        if (c != "0") {
            if (inLine) {
                if (c == "10") { has10 = true; x10 = std::strtod(v.c_str(), nullptr); }
                else if (c == "20") { has20 = true; y10 = std::strtod(v.c_str(), nullptr); }
                else if (c == "11") { has11 = true; x11 = std::strtod(v.c_str(), nullptr); }
                else if (c == "21") { has21 = true; y11 = std::strtod(v.c_str(), nullptr); }
            }
            continue;
        }

        // A code 0 ends whatever entity record was open.
        closeLineRecord();

        if (v == "SECTION") {
            sectionNameExpected = true;
        } else if (v == "ENDSEC") {
            if (section == Section::Blocks && scan.blocks == BlocksSectionState::Unknown)
                scan.blocks = depth == 0 ? BlocksSectionState::Terminated
                                         : BlocksSectionState::Unterminated;
            section = Section::Other;
        } else if (section == Section::Blocks) {
            if (v == "BLOCK") ++depth;
            else if (v == "ENDBLK") --depth;
        } else if (section == Section::Entities && v == "LINE") {
            inLine = true;
        }
    }
    // No closeLineRecord() here. A file that ends with an entity record still
    // open is refused by libdxfrw outright -- measured: it returns false, the
    // reader reports MalformedFile, and this scan's result is never consulted.
    // So a trailing close could not change any outcome, and a line that cannot
    // change an outcome cannot be tested. It is left out rather than left in
    // and excused, on the same grounds as the `inBlock_ at end of read` check
    // that ADR-M6-015 records me writing and measuring doing nothing.

    // EOF inside BLOCKS with a block still open: unterminated.
    if (scan.blocks == BlocksSectionState::Unknown)
        scan.blocks = (blocksSeen && section == Section::Blocks && depth != 0)
                          ? BlocksSectionState::Unterminated
                          : BlocksSectionState::Terminated;
    return scan;
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

// A DXF handle in the uppercase hex the format writes it in, so a diagnostic
// naming a dimension names it the way the user's own file and CAD package do.
//
// DIAGNOSTIC ONLY. Spec 7 allows a handle to be retained as source metadata and
// forbids correctness from depending on it: nothing downstream resolves,
// matches or persists anything by this string.
std::string ToHexHandle(duint32 handle) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    if (handle == 0) return {};
    std::string text;
    while (handle != 0) {
        text.insert(text.begin(), kDigits[handle & 0xF]);
        handle >>= 4;
    }
    return text;
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
        for (ImportedDimension2D& dimension : geometry.dimensions) {
            // The definition POINTS are lengths and scale; the direction is an
            // angle and does not. A stated measurement is a length in the same
            // drawing units as everything else, so it scales too -- omitting it
            // would make a dimension in an inches file state 100 mm while its
            // own definition points said 2540, and M7 would then refuse the
            // pair as disagreeing (ADR-M7-009). Wrong in a way that looks like
            // caution.
            dimension.measureFrom.x *= k; dimension.measureFrom.y *= k;
            dimension.measureTo.x *= k;   dimension.measureTo.y *= k;
            if (dimension.statedValueMm.has_value()) *dimension.statedValueMm *= k;
        }

        // The degenerate checks have to run on the SCALED values, because
        // "shorter than kMinSketchDimensionMm" is a statement about millimetres.
        //
        // Finiteness is re-checked here too. Collecting raw and scaling later
        // moved the AllFinite checks to BEFORE the multiply, so a coordinate
        // that overflowed to infinity during scaling (1e305 in a file measured
        // in miles) passed the reader untouched and was then refused by the
        // model -- aborting the WHOLE file. That is precisely the class
        // ADR-M6-012 was written to eliminate, reintroduced through the
        // refactor that fixed the unit ordering. `tooSmall` cannot catch it
        // either: `inf < 1e-5` is false, and hypot of two infinities is NaN.
        auto tooSmall = [](double v) { return v < kMinSketchDimensionMm; };
        auto overflowed = [](std::initializer_list<double> values) {
            return !AllFinite(values);
        };
        for (auto it = geometry.lines.begin(); it != geometry.lines.end();) {
            if (overflowed({it->start.x, it->start.y, it->end.x, it->end.y})) {
                note(ImportSkipReason::NonFiniteValue, "LINE",
                     "a coordinate overflowed to infinity during unit conversion");
                it = geometry.lines.erase(it);
            } else if (tooSmall(
                           std::hypot(it->end.x - it->start.x, it->end.y - it->start.y))) {
                note(ImportSkipReason::InvalidGeometry, "LINE",
                     "the line has zero length after unit conversion");
                it = geometry.lines.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = geometry.circles.begin(); it != geometry.circles.end();) {
            if (overflowed({it->center.x, it->center.y, it->radiusMm})) {
                note(ImportSkipReason::NonFiniteValue, "CIRCLE",
                     "a value overflowed to infinity during unit conversion");
                it = geometry.circles.erase(it);
            } else if (tooSmall(it->radiusMm)) {
                note(ImportSkipReason::InvalidGeometry, "CIRCLE",
                     "the radius is zero or negative after unit conversion");
                it = geometry.circles.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = geometry.arcs.begin(); it != geometry.arcs.end();) {
            if (overflowed({it->center.x, it->center.y, it->radiusMm})) {
                note(ImportSkipReason::NonFiniteValue, "ARC",
                     "a value overflowed to infinity during unit conversion");
                it = geometry.arcs.erase(it);
            } else if (tooSmall(it->radiusMm)) {
                note(ImportSkipReason::InvalidGeometry, "ARC",
                     "the radius is zero or negative after unit conversion");
                it = geometry.arcs.erase(it);
            } else {
                ++it;
            }
        }

        for (auto it = geometry.dimensions.begin(); it != geometry.dimensions.end();) {
            const double stated = it->statedValueMm.value_or(0.0);
            if (overflowed({it->measureFrom.x, it->measureFrom.y, it->measureTo.x,
                            it->measureTo.y, stated})) {
                note(ImportSkipReason::NonFiniteValue, "DIMENSION",
                     "a value overflowed to infinity during unit conversion");
                it = geometry.dimensions.erase(it);
            } else {
                ++it;
            }
        }

        // Angles are deliberately NOT re-checked here: they are not scaled, so
        // unit conversion cannot make a finite angle infinite. The raw check in
        // addArc is their only guard -- which is exactly why it needs a test of
        // its own, and now has one.
        //
        // A dimension whose points are too CLOSE together is likewise not
        // dropped here. That is a reconstruction judgement, not an import one:
        // the reader's job is to report faithfully what the file said, and M7's
        // analysis is where "too small to be a usable dimension" is decided,
        // with a diagnostic naming the dimension. Dropping it here would delete
        // the evidence before anything could explain it.
    }

    // Removes the LINEs the file never gave an end point. Runs BEFORE finalise
    // so the comparison is against the raw coordinates the scanner read from
    // the same text -- after scaling, both sides would have been multiplied and
    // the equality would depend on the order of two floating-point products.
    //
    // Both endpoints are compared, not just the start. A real line that happens
    // to share a start point with a phantom must survive -- deleting genuine
    // geometry to remove an invented entity would be worse than the bug.
    //
    // Failing to match removes nothing and reports nothing, which is exactly
    // the behaviour this replaces. It cannot make the result worse.
    void dropTruncatedLines(const std::vector<DxfStructureScan::Phantom>& phantoms) {
        for (const auto& phantom : phantoms) {
            const auto it = std::find_if(
                geometry.lines.begin(), geometry.lines.end(),
                [&phantom](const ImportedLine2D& line) {
                    return line.start.x == phantom.sx && line.start.y == phantom.sy &&
                           line.end.x == phantom.ex && line.end.y == phantom.ey;
                });
            if (it == geometry.lines.end()) continue;
            geometry.lines.erase(it);
            note(ImportSkipReason::InvalidGeometry, "LINE",
                 "the entity has a start point (codes 10/20) but no end point "
                 "(codes 11/21); it would have imported as a line to the sketch "
                 "origin, which is not what the file says");
        }
    }

    // Reports a fault the callbacks cannot see. See BlocksSectionState below.
    void noteUnterminatedBlocks() {
        note(ImportSkipReason::UnsupportedEntity, "BLOCK",
             "the BLOCKS section is not terminated by ENDBLK, so every entity "
             "after it was read as block content and skipped; the drawing "
             "cannot be recovered without repairing that section");
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
    void addBlock(const DRW_Block&) override {
        inBlock_ = true;
        // Reset per BLOCK. Setting blockReported_ once and never clearing it
        // made a drawing with forty dimension blocks report "skipped 1", while
        // ADR-M6-009 and the comment below both said "once per block".
        blockReported_ = false;
    }
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
        // std::fmod, NOT a while-loop.
        //
        // I wrote the while-loop version here after fixing exactly this bug in
        // M5 and recording the reason in ADR-M5-006: subtracting 2*pi in a loop
        // never terminates once |value| exceeds about 2^53 * 2*pi, because the
        // subtraction stops changing the double. libdxfrw applies no range check
        // to codes 50/51 and AllFinite accepts any finite value, so an ARC with
        // end angle 1e20 made ReadDxfFile never return -- on the Qt GUI thread,
        // synchronously, with no cancel. A hostile or merely corrupt file froze
        // the application permanently.
        //
        // This is also what ADR-M6-012 claimed was already true: "the reader
        // applies the same sweep test the model uses". The model
        // (IsValidSketchGeometry) uses fmod. Now so does this.
        double sweep = std::fmod(end - start, 2.0 * kPi);
        if (sweep < 0.0) sweep += 2.0 * kPi;
        // Written in POSITIVE form so it is NaN-safe by construction. The
        // negative form `sweep < lo || sweep > hi` is FALSE on both clauses for
        // a NaN, so a NaN sweep sailed straight through and the guard supplied
        // no defence in depth at all -- AllFinite above was a single point of
        // failure, and it is one line. fmod(inf - 0, 2*pi) is NaN, so that is
        // not hypothetical: it is what this path produces the moment the
        // finiteness check is weakened. For every finite input the two forms
        // are identical; both bounds match the model's inclusive comparisons.
        if (!(sweep >= kMinSweepRad && sweep <= 2.0 * kPi - kMinSweepRad)) {
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
    // LINEAR and ALIGNED dimensions are read as ANNOTATION (M7), not geometry.
    //
    // What is read is the pair of DEFINITION points (group codes 13 and 14) --
    // the extension-line origins, which sit on the geometry being measured --
    // plus the dimension's own direction, any explicit text, and the optional
    // stated measurement. The annotation LINES that draw the arrows and witness
    // lines live in an anonymous *D block and are still skipped by addBlock, as
    // ADR-M6-009 requires: reading a dimension deliberately is precisely what
    // stops those lines being imported as model geometry by accident.
    //
    // Nothing here becomes a Sketch entity. A dimension is not geometry, and
    // M7's reconstruction decides separately whether it resolves to any.
    virtual void addDimAlign(const DRW_DimAligned *data) override {
        collectLinearDimension(data, ImportedDimensionKind::Aligned);
    }
    virtual void addDimLinear(const DRW_DimLinear *data) override {
        collectLinearDimension(data, ImportedDimensionKind::Linear);
    }
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

    // The shared body of addDimLinear and addDimAlign.
    //
    // One function for both, because the two differ in exactly one thing --
    // whether the measurement is projected onto a stated direction -- and that
    // difference is carried by the KIND, which the reconstruction layer honours.
    // Two near-identical overrides is how the third one ends up subtly
    // different from the first two.
    template <typename DimType>
    void collectLinearDimension(const DimType* data, ImportedDimensionKind kind) {
        if (data == nullptr) return;
        // A dimension inside a block definition is part of that block, not of
        // model space -- the same rule its geometry follows (ADR-M6-009).
        if (skipBlockContent("DIMENSION")) return;

        const DRW_Coord def1 = data->getDef1Point();
        const DRW_Coord def2 = data->getDef2Point();
        if (!AllFinite({def1.x, def1.y, def2.x, def2.y})) {
            note(ImportSkipReason::NonFiniteValue, "DIMENSION",
                 "a definition point coordinate is not a finite number");
            return;
        }

        ImportedDimension2D dimension;
        dimension.kind = kind;
        dimension.measureFrom = Vec2{def1.x, def1.y};
        dimension.measureTo = Vec2{def2.x, def2.y};
        dimension.textOverride = data->getText();
        dimension.sourceHandle = data->handle == 0 ? std::string{} : ToHexHandle(data->handle);

        if constexpr (std::is_same_v<DimType, DRW_DimLinear>) {
            // Code 50, in DEGREES like every other DXF angle, converted here so
            // nothing downstream has to know the format's convention. An
            // unrotated linear dimension carries 0 and measures along +X.
            const double degrees = data->getAngle();
            if (!AllFinite({degrees})) {
                note(ImportSkipReason::NonFiniteValue, "DIMENSION",
                     "the dimension rotation angle is not a finite number");
                return;
            }
            dimension.directionRad = degrees * kPi / 180.0;
        } else {
            // Aligned measures along its own definition points, so its
            // direction IS that of def1 -> def2. Recorded rather than left at
            // zero, because a consumer reading `directionRad` must get the
            // truth for both kinds.
            dimension.directionRad =
                std::atan2(def2.y - def1.y, def2.x - def1.x);
        }

        // Code 42, the "actual measurement": optional, documented read-only,
        // and defaulted to 0 by the library when the file omits it -- which is
        // indistinguishable from a genuine zero. Zero is not a usable
        // measurement either way, so treating "absent" and "zero" alike loses
        // nothing and avoids inventing a stated value the file never carried.
        const double measured = data->getMeasureValue();
        if (std::isfinite(measured) && measured > 0.0) dimension.statedValueMm = measured;

        geometry.dimensions.push_back(dimension);
    }

    // An entity kind M6 does not import. Reported, never reinterpreted as
    // another kind (spec 4).
    void noteUnsupported(const char* kind) {
        // An unsupported entity INSIDE a block definition is not a model-level
        // entity. Reporting it as one told the user their drawing contained an
        // INSERT and a TEXT it does not contain, and inflated skippedCount by
        // the contents of every block.
        if (skipBlockContent(kind)) return;
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

    // Two questions the callbacks are structurally blind to, asked after the
    // read so they cost nothing on the failure path. Neither can refuse a file
    // the parser accepted: one removes an entity the file never defined, the
    // other only adds a diagnostic.
    const DxfStructureScan scan = ScanDxfStructure(path);
    collector.dropTruncatedLines(scan.truncatedLines);

    // Units applied ONCE, here, after the whole file has been seen -- so the
    // result cannot depend on whether HEADER preceded ENTITIES.
    collector.finalise();

    if (scan.blocks == BlocksSectionState::Unterminated)
        collector.noteUnterminatedBlocks();

    if (!ok) {
        result.error = DxfReadError::MalformedFile;
        result.message = "the file could not be parsed as DXF: " + path;
        return result;
    }

    result.geometry = std::move(collector.geometry);
    return result;
}

} // namespace paramcad
