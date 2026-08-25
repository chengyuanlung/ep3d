#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Electrical/Netlist.h"

#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

// WHAT A SCHEMATIC PUTS ON A SHEET (M36.2).
//
// Both of these live on a DrawingDocument beside its geometry, dimensions and
// parts lists -- a schematic is a drawing (see SymbolLibrary.h). What they add
// is the two things a drawing has no way to say: THIS IS A COMPONENT, and THIS
// IS A CONNECTION.

// A PLACED COMPONENT.
//
// It stores a SENTENCE -- which library, which symbol, where, which way round
// -- and not a copy of the geometry (ADR-M22-003). Copying the shapes in would
// mean a corrected symbol never reaches the drawings already made, which is
// the whole reason a library is a library.
class SymbolPlacement {
public:
    SymbolPlacement(std::string tag, std::string symbolName, Vec2 positionMm, ObjectId layerId);
    SymbolPlacement(ObjectId id, std::string tag, std::string symbolName, Vec2 positionMm,
                    double rotationRad, bool mirrored, ObjectId layerId);

    ObjectId id() const noexcept { return id_; }
    static std::string_view typeName() noexcept { return "SymbolPlacement"; }

    // THE TAG IS THE NAME. "-K1", "-R7" -- what the schematic calls this part
    // and what every cross-reference, wiring list and label points at. It goes
    // through the document's name walk like any other named object, so two
    // parts cannot end up sharing one.
    const std::string& tag() const noexcept { return tag_; }
    void setTag(std::string tag) { tag_ = std::move(tag); }

    const std::string& symbolName() const noexcept { return symbolName_; }
    void setSymbolName(std::string name) { symbolName_ = std::move(name); }

    Vec2 positionMm() const noexcept { return positionMm_; }
    void setPositionMm(Vec2 at) noexcept { positionMm_ = at; }
    double rotationRad() const noexcept { return rotationRad_; }
    void setRotationRad(double radians) noexcept { rotationRad_ = radians; }
    bool isMirrored() const noexcept { return mirrored_; }
    void setMirrored(bool mirrored) noexcept { mirrored_ = mirrored; }

    ObjectId layerId() const noexcept { return layerId_; }
    void setLayerId(ObjectId layerId) noexcept { layerId_ = layerId; }

    // The form the netlist wants. ONE conversion, so connectivity and drawing
    // cannot end up looking at different placements.
    PlacedSymbol asPlaced() const;

private:
    ObjectId id_;
    std::string tag_;
    std::string symbolName_;
    Vec2 positionMm_{};
    double rotationRad_ = 0.0;
    bool mirrored_ = false;
    ObjectId layerId_{kInvalidObjectId};
};

// A CONNECTION, drawn.
//
// ITS OWN KIND, not a polyline on a layer called WIRE. Layer-based wires mean
// moving a line to another layer silently changes the circuit while the
// drawing looks identical -- this project's recurring defect in its most
// expensive form, because what gets built is the netlist.
//
// THE LABEL LIVES HERE, and this is the design decision worth stating: a NET
// is derived and has nowhere to keep a name, so the name is kept on the wires
// that make it up. Numbering writes the name onto every wire in the net; the
// netlist reads it back off them. Two wires in one net carrying DIFFERENT
// labels is a contradiction the drawing must report rather than quietly
// picking one -- see DrawingDocument::netlist.
class WireEntity {
public:
    WireEntity(std::vector<Vec2> pointsMm, ObjectId layerId);
    WireEntity(ObjectId id, std::vector<Vec2> pointsMm, ObjectId layerId, std::string label);

    ObjectId id() const noexcept { return id_; }
    static std::string_view typeName() noexcept { return "Wire"; }

    const std::vector<Vec2>& pointsMm() const noexcept { return pointsMm_; }
    void setPointsMm(std::vector<Vec2> points) { pointsMm_ = std::move(points); }

    const std::string& label() const noexcept { return label_; }
    void setLabel(std::string label) { label_ = std::move(label); }

    ObjectId layerId() const noexcept { return layerId_; }
    void setLayerId(ObjectId layerId) noexcept { layerId_ = layerId; }

    WireRun asRun() const;
    // A wire needs two points to be a connection; one is a click somebody did
    // not finish.
    bool isDrawable() const noexcept { return pointsMm_.size() >= 2; }
    double lengthMm() const noexcept;

private:
    ObjectId id_;
    std::vector<Vec2> pointsMm_;
    std::string label_;
    ObjectId layerId_{kInvalidObjectId};
};

} // namespace paramcad
