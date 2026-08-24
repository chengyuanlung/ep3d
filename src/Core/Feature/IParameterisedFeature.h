#pragma once

#include "Core/Document/ObjectId.h"

#include <vector>

namespace paramcad {

// One number a feature exposes for editing (M26.9).
struct FeatureParameter {
    // What the panel calls it: "Length", "Diameter", "Depth", "Count". Short,
    // because the property table sizes its label column to content and only
    // bounded labels are safe there (the assumption the M6.14 fix rests on).
    const char* label{""};

    // The PARAMETER's id, never the feature's. The panel writes through
    // PartDocument's facade into the Parameter (UI spec 20); a row pointing at
    // a feature would be a cell that edits nothing.
    ObjectId parameterId{kInvalidObjectId};

    // Whether "the other way" is a meaningful thing to ask of this number.
    //
    // A pad's length and a pocket's depth carry their direction in their SIGN,
    // so a Reversed checkbox is the readable form of that fact (ADR-M17-031).
    // A diameter, a count and an angle have no other way to go, and offering
    // the box would be offering a control that means nothing.
    bool reversible{false};
};

// A feature with numbers a user can edit (M26.9).
//
// A CAPABILITY, not the list of dynamic_casts it replaces. The property panel
// had a hand-written branch per feature type -- Pad, Pocket, Fillet/Chamfer,
// Revolve -- and every feature added after those four arrived without one. By
// M26 that was Hole, Shell, Draft, Sweep, Loft and all three patterns: eleven
// kinds of feature, sixteen editable numbers, and the panel could show four of
// them. A hole's diameter and depth were stored, solved, saved and reloaded
// correctly, and there was nowhere in the shell to read or change them.
//
// This is the same trap ISketchConsuming was written to close, one panel over,
// and it went unnoticed for the same reason: nothing fails when a branch is
// missing. The rows simply are not there, and only somebody looking for a
// number they cannot find finds out.
//
// A feature that implements this is described everywhere the panel describes
// anything; one that does not is described nowhere -- and that is a decision
// its author makes at compile time, not a branch somebody forgets.
class IParameterisedFeature {
public:
    virtual ~IParameterisedFeature() = default;

    // Every editable number, in the order a user should read them.
    virtual std::vector<FeatureParameter> featureParameters() const = 0;
};

} // namespace paramcad
