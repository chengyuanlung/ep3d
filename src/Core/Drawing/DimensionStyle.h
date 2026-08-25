#pragma once

#include "Core/Document/ObjectId.h"

#include <string>

namespace paramcad {

// HOW A DIMENSION IS DRAWN (M34), ported from EasyCad's DimStyle.
//
// EVERY LENGTH HERE IS IN PAPER MILLIMETRES, not model millimetres. A 3.5 mm
// text height means 3.5 mm on the printed sheet whatever the view is drawn at
// -- that is what makes a 1:5 general arrangement and a 2:1 detail readable
// side by side, and it is the one thing about a dimension style that surprises
// people who have only met screen graphics.
//
// The MEASUREMENT is the opposite: it is in model millimetres, because it is
// the size of the part. The two units meet in exactly one place -- the
// dimension's own measure() -- and the whole style exists on the paper side of
// that line.
class DimensionStyle {
public:
    DimensionStyle(std::string name);
    DimensionStyle(ObjectId id, std::string name);

    ObjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    // --- Text -----------------------------------------------------------------
    double textHeightMm() const noexcept { return textHeightMm_; }
    void setTextHeightMm(double height) noexcept;
    // How far the text floats above the dimension line. ISO puts it above, on
    // the line; a gap of zero would have it sitting on the stroke.
    double textGapMm() const noexcept { return textGapMm_; }
    void setTextGapMm(double gap) noexcept;

    // --- Arrows ---------------------------------------------------------------
    double arrowSizeMm() const noexcept { return arrowSizeMm_; }
    void setArrowSizeMm(double size) noexcept;

    // --- Extension lines ------------------------------------------------------
    //
    // A GAP AT THE FEATURE and an OVERSHOOT past the dimension line. Both are
    // ISO 129 conventions and both matter: the gap stops the extension line
    // being mistaken for part of the object, and the overshoot makes the
    // corner readable when two dimensions stack.
    double extensionGapMm() const noexcept { return extensionGapMm_; }
    double extensionOvershootMm() const noexcept { return extensionOvershootMm_; }
    void setExtensionGapMm(double gap) noexcept;
    void setExtensionOvershootMm(double overshoot) noexcept;

    // --- The number -----------------------------------------------------------
    //
    // DECIMAL PLACES, and a suffix for the unit when the drawing needs one.
    // Trailing zeros are KEPT: "25.00" and "25" mean different things to an
    // inspector -- the first claims two decimals of intent.
    int decimals() const noexcept { return decimals_; }
    void setDecimals(int decimals) noexcept;
    const std::string& suffix() const noexcept { return suffix_; }
    void setSuffix(std::string suffix) { suffix_ = std::move(suffix); }

    // A SCALE OVER THE WHOLE STYLE, so a drawing plotted at half size can keep
    // its text readable without every field being retyped. Multiplies every
    // paper length above; the measurement is untouched.
    double overallScale() const noexcept { return overallScale_; }
    void setOverallScale(double scale) noexcept;

    // The paper lengths WITH the overall scale applied. One reader each, so
    // nothing forgets to apply it -- which would be a style that half works.
    double scaledTextHeightMm() const noexcept { return textHeightMm_ * overallScale_; }
    double scaledArrowSizeMm() const noexcept { return arrowSizeMm_ * overallScale_; }
    double scaledTextGapMm() const noexcept { return textGapMm_ * overallScale_; }
    double scaledExtensionGapMm() const noexcept { return extensionGapMm_ * overallScale_; }
    double scaledExtensionOvershootMm() const noexcept {
        return extensionOvershootMm_ * overallScale_;
    }

    // The measurement, formatted. THE one place a number becomes text, so a
    // dimension on screen, in a DXF and on a plot cannot read differently.
    std::string format(double valueMm) const;

private:
    ObjectId id_;
    std::string name_;
    // ISO 129-1 defaults, which is what a metric drawing expects to find.
    double textHeightMm_ = 3.5;
    double textGapMm_ = 0.8;
    double arrowSizeMm_ = 3.5;
    double extensionGapMm_ = 1.5;
    double extensionOvershootMm_ = 2.0;
    int decimals_ = 2;
    std::string suffix_;
    double overallScale_ = 1.0;
};

// The style every drawing has and none may delete -- the same rule layer "0"
// follows, and for the same reason: a dimension has to have a style.
inline constexpr const char* kDefaultDimensionStyleName = "ISO-25";

} // namespace paramcad
