#pragma once

#include "Core/Drawing/DrawingDocument.h"
#include "Viewer/DocumentOutline.h"

#include <set>
#include <vector>

namespace paramcad {

// THE MODEL TREE FOR A DRAWING (M32.4).
//
// THE SAME OutlineNode TYPE, a different builder -- exactly as AssemblyOutline
// is. The widget, the state markers, the selection and the property panel are
// shared; only what goes in the tree differs. A second node type would mean a
// second tree widget, a second selection story and a second set of state
// colours, which is three things that must agree about one idea.
class DrawingOutline {
public:
    explicit DrawingOutline(const DrawingDocument& document) : document_(&document) {}

    // `hiddenIds` is presentation and never reaches Core (A02) -- the same
    // shape AssemblyOutline takes it in.
    OutlineNode build(const std::set<ObjectId>& hiddenIds = {}) const;
    std::vector<PropertyRow> propertiesOf(ObjectId id) const;

private:
    const DrawingDocument* document_;
};

} // namespace paramcad
