#include "Core/Drawing/DimensionStyle.h"

#include <cstdio>
#include <utility>

namespace paramcad {

DimensionStyle::DimensionStyle(std::string name)
    : id_(ObjectIdGenerator::Next()), name_(std::move(name)) {}

DimensionStyle::DimensionStyle(ObjectId id, std::string name)
    : id_(RestoreObjectId(id)), name_(std::move(name)) {}

// EVERY SETTER REFUSES A NON-POSITIVE VALUE rather than storing it.
//
// A zero text height is text nobody can read, a zero arrow is no arrow, and
// both would be found at plot time -- a long way from whoever typed them. The
// old value stays, which is a style that still works.
void DimensionStyle::setTextHeightMm(double height) noexcept {
    if (height > 0.0) textHeightMm_ = height;
}
void DimensionStyle::setArrowSizeMm(double size) noexcept {
    if (size > 0.0) arrowSizeMm_ = size;
}
void DimensionStyle::setOverallScale(double scale) noexcept {
    if (scale > 0.0) overallScale_ = scale;
}

// A GAP AND AN OVERSHOOT MAY BE ZERO. Some house styles run the extension line
// right up to the feature, and refusing that would be this file having an
// opinion it is not entitled to. Negative is still refused: it would draw the
// line inside the object.
void DimensionStyle::setTextGapMm(double gap) noexcept {
    if (gap >= 0.0) textGapMm_ = gap;
}
void DimensionStyle::setExtensionGapMm(double gap) noexcept {
    if (gap >= 0.0) extensionGapMm_ = gap;
}
void DimensionStyle::setExtensionOvershootMm(double overshoot) noexcept {
    if (overshoot >= 0.0) extensionOvershootMm_ = overshoot;
}

void DimensionStyle::setDecimals(int decimals) noexcept {
    // Bounded at both ends: a negative count is meaningless and more than nine
    // is past what a double carries, so it would print digits that are not
    // measurements.
    if (decimals >= 0 && decimals <= 9) decimals_ = decimals;
}

std::string DimensionStyle::format(double valueMm) const {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals_, valueMm);
    std::string text = buffer;
    // NO NEGATIVE ZERO. "-0.00" beside "0.00" is two numbers where the part
    // has one -- the same rule the part and assembly panels follow, and the
    // same one that was found by looking at the screen in M26.
    if (text.size() > 1 && text[0] == '-' &&
        text.find_first_of("123456789") == std::string::npos)
        text.erase(0, 1);
    // TRAILING ZEROS ARE KEPT. "25.00" and "25" mean different things to an
    // inspector: the first claims two decimals of intent, and trimming them
    // would silently loosen every tolerance the drawing implies.
    return text + suffix_;
}

} // namespace paramcad
