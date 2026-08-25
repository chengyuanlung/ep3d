#pragma once

#include "Core/Document/ObjectId.h"

#include <string>
#include <vector>

namespace paramcad {

// THE DXF TABLE MODEL, taken deliberately (M32).
//
// Layers and linetypes are not EP3D inventions and must not become them: a
// drawing's whole purpose is to leave the program, and it leaves as DXF or as
// paper. Modelling them any other way would mean a translation layer at the
// door, and a translation layer is a second place the truth lives.
//
// So the fields are DXF's fields, in DXF's units, with DXF's defaults -- and
// where DXF is ugly the ugliness is kept and explained rather than smoothed
// over into something that cannot round-trip.
//
// Ported from EasyCad (EasyCad.Core/Tables), which had already made these
// choices and proved them against netDxf. What is NOT taken is its `Handle`:
// EP3D has ObjectId, and a second identity per object is exactly the seam this
// project spends its milestones removing.

// AutoCAD Color Index. 256 = ByLayer, 0 = ByBlock, 1..255 = the palette.
//
// AN INDEX, NOT AN RGB. A drawing that stored RGB would print differently from
// the same drawing opened in AutoCAD, because ACI is what pen tables and plot
// styles are keyed on. The mapping to RGB for the screen lives in the viewer,
// once.
inline constexpr int kColorByLayer = 256;
inline constexpr int kColorByBlock = 0;

// Lineweight in HUNDREDTHS OF A MILLIMETRE, which is DXF group code 370.
// -1 = ByLayer, -2 = ByBlock, -3 = Default.
inline constexpr int kLineweightByLayer = -1;
inline constexpr int kLineweightByBlock = -2;
inline constexpr int kLineweightDefault = -3;

// A DASH PATTERN, as DXF writes it: lengths in drawing units, positive for a
// dash, negative for a gap, zero for a dot. An empty pattern is CONTINUOUS.
//
// Kept as the raw list rather than as a named enum of "dashed / centre /
// hidden", because a drawing may carry any pattern a supplier's template
// defined, and an enum would silently flatten one of those to the nearest
// name it knew.
class Linetype {
public:
    Linetype(std::string name, std::string description, std::vector<double> pattern);
    Linetype(ObjectId id, std::string name, std::string description,
             std::vector<double> pattern);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }
    const std::string& description() const noexcept { return description_; }
    void setDescription(std::string description) { description_ = std::move(description); }
    const std::vector<double>& pattern() const noexcept { return pattern_; }
    void setPattern(std::vector<double> pattern) { pattern_ = std::move(pattern); }

    bool isContinuous() const noexcept { return pattern_.empty(); }
    // Sum of the absolute segment lengths -- one period of the pattern. Zero
    // for CONTINUOUS, which callers read as "do not dash".
    double patternLength() const noexcept;

private:
    ObjectId id_;
    std::string name_;
    std::string description_;
    std::vector<double> pattern_;
};

// WHAT A LAYER CARRIES, and what each flag actually does:
//
//   on       off is invisible AND unplotted, but still regenerated
//   frozen   invisible, unplotted AND skipped by regeneration
//   locked   visible but unselectable
//
// Three flags rather than one visibility, because AutoCAD's three do
// different things and a drawing that collapsed them would come back from a
// round trip with layers behaving differently than they were left.
class Layer {
public:
    Layer(std::string name, int color, std::string linetype);
    Layer(ObjectId id, std::string name, int color, std::string linetype, bool on, bool frozen,
          bool locked, int lineweight);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    int color() const noexcept { return color_; }
    void setColor(int color) noexcept { color_ = color; }
    // The linetype BY NAME, because that is what DXF stores and what survives
    // a table being rewritten. An id would point at whatever now sits in that
    // slot after somebody else's file was merged in.
    const std::string& linetype() const noexcept { return linetype_; }
    void setLinetype(std::string linetype) { linetype_ = std::move(linetype); }

    bool isOn() const noexcept { return on_; }
    bool isFrozen() const noexcept { return frozen_; }
    bool isLocked() const noexcept { return locked_; }
    void setOn(bool on) noexcept { on_ = on; }
    void setFrozen(bool frozen) noexcept { frozen_ = frozen; }
    void setLocked(bool locked) noexcept { locked_ = locked; }

    int lineweight() const noexcept { return lineweight_; }
    void setLineweight(int lineweight) noexcept { lineweight_ = lineweight; }

    // Drawn at all? "On" and "not frozen" are two facts and this is the one
    // question every renderer actually asks, so it is answered here rather
    // than by each of them.
    bool isVisible() const noexcept { return on_ && !frozen_; }

private:
    ObjectId id_;
    std::string name_;
    int color_{7}; // 7 = white/black, the AutoCAD default
    std::string linetype_{"CONTINUOUS"};
    bool on_{true};
    bool frozen_{false};
    bool locked_{false};
    int lineweight_{kLineweightDefault};
};

// The layer every drawing has and none may delete or rename. AutoCAD's rule,
// kept because a DXF without layer "0" is a DXF other programs refuse.
inline constexpr const char* kDefaultLayerName = "0";
inline constexpr const char* kContinuousLinetypeName = "CONTINUOUS";

} // namespace paramcad
