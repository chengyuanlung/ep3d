#include "Core/Edit/FeatureEditSession.h"

#include "Core/Document/PartDocument.h"
#include "Core/Serialization/PartDocumentSerializer.h"

#include <sstream>
#include <utility>

namespace paramcad {

std::unique_ptr<FeatureEditSession> FeatureEditSession::open(PartDocument& document,
                                                             std::string label) {
    std::ostringstream out;
    if (!savePartDocument(document, out)) return nullptr;
    std::istringstream in(out.str());
    LoadResult loaded = loadPartDocument(in);
    if (!loaded) return nullptr;

    // The copy needs the same backends to build anything. They are borrowed,
    // not owned -- the same arrangement the real document has (ADR-M3-003), and
    // the session never outlives the document it was opened on.
    loaded.document->setGeometryKernel(document.geometryKernel());
    loaded.document->setSketchSolver(document.sketchSolver());
    return std::unique_ptr<FeatureEditSession>(
        new FeatureEditSession(document, std::move(loaded.document), std::move(label)));
}

FeatureEditSession::FeatureEditSession(PartDocument& document,
                                       std::unique_ptr<PartDocument> preview, std::string label)
    : document_(document), preview_(std::move(preview)), label_(std::move(label)) {}

FeatureEditSession::~FeatureEditSession() {
    // An abandoned session cancels. A destructor that silently COMMITTED would
    // turn every early return in a UI slot into an accidental edit.
    if (open_) cancel();
}

bool FeatureEditSession::setParameterValue(ObjectId parameterId, double value) {
    if (!open_) return false;
    if (!preview_->setParameterValue(parameterId, value)) return false;
    for (StagedEdit& edit : staged_) {
        if (edit.parameterId != parameterId) continue;
        edit.value = value; // collapse: the session holds the destination
        return true;
    }
    staged_.push_back(StagedEdit{parameterId, value});
    return true;
}

bool FeatureEditSession::recomputePreview() {
    if (!open_) return false;
    return preview_->recompute().success;
}

const MassProperties& FeatureEditSession::previewMassProperties() const {
    return preview_->massProperties();
}

bool FeatureEditSession::accept() {
    if (!open_) return false;
    open_ = false;
    // ONE transaction, so an edit that moved three values is one undo step --
    // and an edit that moved none records nothing at all, because an empty
    // transaction is not a step (M9.1).
    document_.beginTransaction(label_);
    for (const StagedEdit& edit : staged_) document_.setParameterValue(edit.parameterId, edit.value);
    document_.commitTransaction();
    preview_.reset();
    return true;
}

void FeatureEditSession::cancel() {
    open_ = false;
    staged_.clear();
    preview_.reset();
    // Nothing else to do, and that is the point: the real document was never
    // touched, so there is nothing to roll back and no way for a cancel to
    // leave a trace.
}

} // namespace paramcad
