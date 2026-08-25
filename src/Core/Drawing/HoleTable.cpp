#include "Core/Drawing/HoleTable.h"

#include "Core/Document/PartDocument.h"
#include "Core/Feature/HoleFeature.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Profile.h"
#include "Core/Sketch/Sketch.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <variant>
#include <cstdio>
#include <vector>

namespace paramcad {

namespace {

// A hole as it comes out of the part, before it is tagged.
struct Found {
    Vec2 atMm{0.0, 0.0};
    std::string callout;
    double diameterMm = 0.0;
};

// THE SAME PAGE FRAME THE PROJECTOR USES (see OcctDrawingProjection.cpp).
//
// Written here in Core rather than asked of the kernel because a hole's
// position is not geometry -- it is two numbers about a point, and a table
// that had to build a solid to find out where a hole was could not be tested
// without one. What it MUST NOT do is differ: a table whose positions came
// from a different frame from the curves would put the tag beside the wrong
// hole, and every number in the row would still be right.
Vec2 ProjectOntoPage(const Vec3& point, const ViewCamera& camera) {
    const Vec3 sight{-camera.towards.x, -camera.towards.y, -camera.towards.z};
    const Vec3 pageX{camera.up.y * sight.z - camera.up.z * sight.y,
                     camera.up.z * sight.x - camera.up.x * sight.z,
                     camera.up.x * sight.y - camera.up.y * sight.x};
    const Vec3 pageY = camera.up;
    return Vec2{point.x * pageX.x + point.y * pageX.y + point.z * pageX.z,
                point.x * pageY.x + point.y * pageY.y + point.z * pageY.z};
}

const Parameter* ParameterNamed(const PartDocument& part, ObjectId id) {
    for (const std::unique_ptr<Parameter>& one : part.parameters().items())
        if (one->id() == id) return one.get();
    return nullptr;
}

// A1, A2, B1: the LETTER is the size and the NUMBER is which one of that size.
//
// Not one letter per hole, which is what a table generated in feature order
// gives and what makes a twenty-hole plate unreadable: the reader wants to
// know "these six are the same hole", and the tag is where that is said.
std::string TagOf(std::size_t group, std::size_t within) {
    std::string letter;
    std::size_t number = group + 1;
    while (number > 0) {
        const std::size_t remainder = (number - 1) % 26;
        letter.insert(letter.begin(), static_cast<char>('A' + remainder));
        number = (number - 1) / 26;
    }
    return letter + std::to_string(within + 1);
}

} // namespace

HoleTableContents HolesOfPart(const std::string& partPath, ViewDirection direction, Vec2 datumMm) {
    HoleTableContents out;

    LoadResult loaded = loadPartDocumentFromFile(partPath);
    if (!loaded) {
        // REFUSED, NOT EMPTY. An unreadable part that came back as "no holes"
        // is the one wrong answer that looks like a right one -- the same
        // trap the BOM's uncounted sub-assemblies are there for (M36).
        out.why = "the part this table is for could not be read: " + loaded.message;
        return out;
    }
    const PartDocument& part = *loaded.document;
    const ViewCamera camera = CameraFor(direction);

    std::vector<Found> found;
    for (const std::unique_ptr<Body>& body : part.bodies()) {
        for (const std::unique_ptr<Feature>& feature : body->features()) {
            const auto* hole = dynamic_cast<const HoleFeature*>(feature.get());
            if (hole == nullptr) continue;

            const Parameter* diameter = ParameterNamed(part, hole->diameterParameterId());
            const Parameter* depth = ParameterNamed(part, hole->depthParameterId());
            const Sketch* sketch = part.findSketch(hole->sketchId());
            if (diameter == nullptr || depth == nullptr || sketch == nullptr) {
                out.why = "hole '" + hole->name() +
                          "' is missing the sketch or the parameters it is built from";
                return out;
            }

            // THE SAME SIZING THE CUT USES. A table that worked its callouts
            // out for itself would be a second reading of one standard, and
            // the drawing would say M8 over a hole drilled to something else.
            const bool through = std::fabs(depth->value()) < 1e-9;
            const HoleSizes sizes =
                hole->sizes(diameter->value(), through ? 0.0 : std::fabs(depth->value()));
            if (!sizes.ok) {
                out.why = "hole '" + hole->name() + "' cannot be sized: " + sizes.why;
                return out;
            }

            const ProfilePlane plane =
                PlaneOfSketchFrame(part.effectiveSketchFrame(sketch->id()));
            for (const SketchEntity& entity : sketch->entities()) {
                const auto* point = std::get_if<SketchPoint>(&entity.geometry);
                if (point == nullptr) continue;
                const Vec3 inModel{
                    plane.origin.x + plane.uAxis.x * point->position.x +
                        plane.vAxis.x * point->position.y,
                    plane.origin.y + plane.uAxis.y * point->position.x +
                        plane.vAxis.y * point->position.y,
                    plane.origin.z + plane.uAxis.z * point->position.x +
                        plane.vAxis.z * point->position.y};
                const Vec2 onPage = ProjectOntoPage(inModel, camera);
                found.push_back(
                    Found{Vec2{onPage.x - datumMm.x, onPage.y - datumMm.y},
                          sizes.callout, sizes.drillDiameterMm});
            }
        }
    }

    // GROUPED BY WHAT THE HOLE IS, then ordered the way a reader scans: down
    // the page, then across. Ordered by feature order instead, two identical
    // holes at opposite corners would be A1 and A7 with five unrelated rows
    // between them.
    std::vector<std::string> groups;
    for (const Found& one : found)
        if (std::find(groups.begin(), groups.end(), one.callout) == groups.end())
            groups.push_back(one.callout);

    for (std::size_t group = 0; group < groups.size(); ++group) {
        std::vector<const Found*> members;
        for (const Found& one : found)
            if (one.callout == groups[group]) members.push_back(&one);
        std::stable_sort(members.begin(), members.end(), [](const Found* a, const Found* b) {
            if (std::fabs(a->atMm.y - b->atMm.y) > 1e-9) return a->atMm.y > b->atMm.y;
            return a->atMm.x < b->atMm.x;
        });
        for (std::size_t within = 0; within < members.size(); ++within)
            out.rows.push_back(HoleTableRow{TagOf(group, within), members[within]->atMm,
                                            members[within]->callout,
                                            members[within]->diameterMm});
    }

    // A part with no holes gives an empty table AND ok. That is a true answer
    // about the part, and it is a different thing from a file that would not
    // open -- which is why the two are not both an empty list.
    out.ok = true;
    return out;
}

std::string_view toString(HoleColumn column) noexcept {
    switch (column) {
        case HoleColumn::Tag: return "tag";
        case HoleColumn::X: return "x";
        case HoleColumn::Y: return "y";
        case HoleColumn::Description: return "description";
    }
    return "tag";
}

bool ParseHoleColumn(std::string_view text, HoleColumn& into) noexcept {
    // READ FROM THE SAME LIST IT IS WRITTEN FROM, so a column cannot be
    // written in one spelling and looked for in another.
    for (const HoleColumn column :
         {HoleColumn::Tag, HoleColumn::X, HoleColumn::Y, HoleColumn::Description})
        if (text == toString(column)) {
            into = column;
            return true;
        }
    return false;
}

std::string_view HeadingOf(HoleColumn column) noexcept {
    switch (column) {
        case HoleColumn::Tag: return "HOLE";
        case HoleColumn::X: return "X";
        case HoleColumn::Y: return "Y";
        case HoleColumn::Description: return "DESCRIPTION";
    }
    return "HOLE";
}

std::string CellOf(const HoleTableRow& row, HoleColumn column) {
    const auto number = [](double value) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", value);
        return std::string(text);
    };
    switch (column) {
        case HoleColumn::Tag: return row.tag;
        case HoleColumn::X: return number(row.atMm.x);
        case HoleColumn::Y: return number(row.atMm.y);
        case HoleColumn::Description: return row.callout;
    }
    return {};
}

namespace {

double DefaultWidthOf(HoleColumn column) noexcept {
    switch (column) {
        case HoleColumn::Tag: return 16.0;
        case HoleColumn::X: return 22.0;
        case HoleColumn::Y: return 22.0;
        case HoleColumn::Description: return 60.0;
    }
    return 20.0;
}

} // namespace

HoleTable::HoleTable(std::string name, ObjectId viewId, Vec2 positionMm)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)), viewId_(viewId),
      positionMm_(positionMm) {}

HoleTable::HoleTable(ObjectId id, std::string name, ObjectId viewId, Vec2 positionMm)
    : id_(RestoreObjectId(id)), name_(std::move(name)), viewId_(viewId),
      positionMm_(positionMm) {}

bool HoleTable::setColumns(std::vector<HoleColumn> columns) {
    if (columns.empty()) return false;
    columns_ = std::move(columns);
    return true;
}

bool HoleTable::setRowHeightMm(double heightMm) noexcept {
    if (!(heightMm > 0.0)) return false;
    rowHeightMm_ = heightMm;
    return true;
}

double HoleTable::columnWidthMm(HoleColumn column) const noexcept {
    for (const auto& pair : columnWidths_)
        if (pair.first == column) return pair.second;
    return DefaultWidthOf(column);
}

bool HoleTable::setColumnWidthMm(HoleColumn column, double widthMm) {
    if (!(widthMm > 0.0)) return false;
    for (auto& pair : columnWidths_)
        if (pair.first == column) {
            pair.second = widthMm;
            return true;
        }
    columnWidths_.emplace_back(column, widthMm);
    return true;
}

double HoleTable::widthMm() const noexcept {
    double total = 0.0;
    for (const HoleColumn column : columns_) total += columnWidthMm(column);
    return total;
}

double HoleTable::rowBottomMm(std::size_t index, std::size_t rowCount) const noexcept {
    // A HOLE TABLE GROWS DOWNWARD, always: it is read alongside the view, top
    // to bottom, and its heading belongs at the top where a reader starts.
    // That is the one difference from a parts list, which is anchored to the
    // title block and grows up out of it.
    (void)rowCount;
    return -(static_cast<double>(index) + 1.0) * rowHeightMm_;
}

} // namespace paramcad
