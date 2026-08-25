#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Drawing/Sheet.h"
#include "Core/Drawing/SheetFrame.h"

#include <string>
#include <string_view>
#include <utility>

namespace paramcad {

// ONE PAGE OF A DRAWING (M44).
//
// The paper and its frame. A drawing FILE holds several: the general
// arrangement, then the details, then the parts list page.
//
// THE TITLE BLOCK IS NOT HERE, and that is deliberate. A drawing set carries
// one title, one drawing number, one approval -- they are facts about the
// DRAWING and not about a page of it. What differs page to page is the Sheet
// row, and that is derived from where the page sits. A block per page would
// be several copies of one fact, and the copies would drift the first time
// somebody corrected the title on the page they happened to be looking at.
//
// UP TO HERE THE TITLE BLOCK COULD ONLY SAY 1 / 1. It had a Sheet row from
// M35 and there was nothing else it could put in it, which is the kind of
// half-truth a drawing carries into a workshop: a reader who sees 1 / 1
// believes there is no second page.
//
// WHAT A PAGE DOES NOT OWN is the objects on it. Views, dimensions, symbols
// and tables stay in the document's lists and each says which page it is on.
// Ownership would make "which page" impossible to get wrong, which is the
// shape this project prefers -- but it would rewrite every method that walks
// those lists. What is done instead is a boundary check: at save and at load,
// in ONE place, every object's page has to be a page that exists.
class SheetPage {
public:
    SheetPage(std::string name, Sheet paper);
    SheetPage(ObjectId id, std::string name, Sheet paper);

    ObjectId id() const noexcept { return id_; }
    static std::string_view typeName() noexcept { return "SheetPage"; }

    // What the tab says. Not the "Sheet 2 of 3" row -- that is DERIVED from
    // where this page sits in the file, and a stored copy of it is the first
    // thing to go stale when a page is inserted.
    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    const Sheet& paper() const noexcept { return paper_; }
    Sheet& paperForEdit() noexcept { return paper_; }

    const FrameMargins& frameMargins() const noexcept { return frameMargins_; }
    void setFrameMargins(const FrameMargins& margins) noexcept { frameMargins_ = margins; }
    FrameMargins& marginsForEdit() noexcept { return frameMargins_; }

    double frameZoneTargetMm() const noexcept { return zoneTargetMm_; }
    void setFrameZoneTargetMm(double target) noexcept { zoneTargetMm_ = target; }
    double& zoneForEdit() noexcept { return zoneTargetMm_; }

    bool isFrameVisible() const noexcept { return frameVisible_; }
    void setFrameVisible(bool visible) noexcept { frameVisible_ = visible; }
    bool& frameShownForEdit() noexcept { return frameVisible_; }

private:
    ObjectId id_;
    std::string name_;
    Sheet paper_;
    FrameMargins frameMargins_{FrameMargins::standard()};
    // 100 mm, which is what the document used before pages existed. A new
    // default here would have quietly re-zoned every drawing that was reopened.
    double zoneTargetMm_ = 100.0;
    bool frameVisible_ = true;
};

} // namespace paramcad
