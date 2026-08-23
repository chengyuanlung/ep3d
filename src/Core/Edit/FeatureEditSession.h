#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Physics/MassProperties.h"

#include <memory>
#include <string>
#include <vector>

namespace paramcad {

class PartDocument;

// M9.2 -- the feature edit transaction: Select, Edit, Preview, then Accept or
// Cancel (roadmap section 10).
//
// ADR-M9-003's rule, stated where the type is: **preview state is not document
// state.** While a session is open, nothing in the real document, its registry
// or its dependency graph has changed, and Cancel is indistinguishable from
// never having started -- not "changed and changed back", which would leave a
// dirtied graph, a bumped id generator, or a stray undo record behind.
//
// HOW, and the cost, stated plainly: the preview is a SEMANTIC COPY of the
// document, made by saving and loading it. That reuses the most heavily tested
// path in the project instead of inventing a second way to duplicate a
// document, and it is honest about what a preview is -- a different document
// that answers "what would this look like". The cost is a save/load per open,
// which is proportional to document size and is why a session is opened per
// EDIT, not per keystroke.
//
// The alternative -- apply to the real document, compute, then revert -- was
// rejected. It mutates the thing the user is looking at, and every observer
// (the viewer, the mass properties, an autosave, a second session) would see a
// state the user never asked for. "Nobody looks in between" is an assumption
// this project has been wrong about before.
class FeatureEditSession {
public:
    // Returns nullptr if the document cannot be copied -- which is exactly when
    // it could not be saved either, so the caller learns the same thing it
    // would learn from a save, before the user has typed anything.
    static std::unique_ptr<FeatureEditSession> open(PartDocument& document, std::string label);

    ~FeatureEditSession();

    FeatureEditSession(const FeatureEditSession&) = delete;
    FeatureEditSession& operator=(const FeatureEditSession&) = delete;

    // Stages a parameter change. Applied to the PREVIEW only. Repeated edits to
    // one parameter collapse: the session holds the value the user wants, not
    // the path they took to it.
    bool setParameterValue(ObjectId parameterId, double value);

    // Recomputes the preview. False when the previewed state does not build --
    // which is information the user wants BEFORE committing, and is the reason
    // preview exists at all.
    bool recomputePreview();
    const MassProperties& previewMassProperties() const;
    const PartDocument& preview() const noexcept { return *preview_; }

    // Applies every staged edit to the real document inside ONE transaction, so
    // the whole edit is one undo step however many values it touched. False if
    // the session was already closed. Does NOT recompute the real document --
    // the caller decides when, exactly as `setParameterValue` leaves it to the
    // caller (ADR-M9-001).
    bool accept();

    // Discards. The real document was never touched, so this only has to drop
    // the copy.
    void cancel();

    bool isOpen() const noexcept { return open_; }

private:
    FeatureEditSession(PartDocument& document, std::unique_ptr<PartDocument> preview,
                       std::string label);

    struct StagedEdit {
        ObjectId parameterId = kInvalidObjectId;
        double value = 0.0;
    };

    PartDocument& document_;
    std::unique_ptr<PartDocument> preview_;
    std::string label_;
    std::vector<StagedEdit> staged_;
    bool open_ = true;
};

} // namespace paramcad
