#include "Core/Drawing/Sheet.h"

#include <cctype>
#include <cstdlib>

namespace paramcad {

namespace {

// PORTRAIT millimetres, ISO 216. One table, so nothing else carries a second
// copy of what A3 is.
struct PaperSize {
    double widthMm;
    double heightMm;
};

PaperSize PortraitOf(SheetSize size) noexcept {
    switch (size) {
        case SheetSize::A0: return PaperSize{841.0, 1189.0};
        case SheetSize::A1: return PaperSize{594.0, 841.0};
        case SheetSize::A2: return PaperSize{420.0, 594.0};
        case SheetSize::A3: return PaperSize{297.0, 420.0};
        case SheetSize::A4: return PaperSize{210.0, 297.0};
        case SheetSize::Custom: break;
    }
    return PaperSize{0.0, 0.0};
}

} // namespace

std::string_view toString(SheetSize size) noexcept {
    switch (size) {
        case SheetSize::A0: return "A0";
        case SheetSize::A1: return "A1";
        case SheetSize::A2: return "A2";
        case SheetSize::A3: return "A3";
        case SheetSize::A4: return "A4";
        case SheetSize::Custom: return "Custom";
    }
    return "A3";
}

std::string_view toString(ProjectionAngle angle) noexcept {
    return angle == ProjectionAngle::First ? "First" : "Third";
}

std::string_view toString(SheetOrientation orientation) noexcept {
    return orientation == SheetOrientation::Portrait ? "Portrait" : "Landscape";
}

double DrawingScale::factor() const noexcept {
    if (!valid()) return 1.0;
    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

std::string DrawingScale::toString() const {
    return std::to_string(numerator) + ":" + std::to_string(denominator);
}

bool operator==(const DrawingScale& a, const DrawingScale& b) noexcept {
    // BY WHAT WAS TYPED, not by the quotient. 1:3 and 2:6 print differently,
    // so they are different scales -- and a comparison that called them equal
    // would let a save quietly replace one with the other.
    return a.numerator == b.numerator && a.denominator == b.denominator;
}

bool ParseDrawingScale(std::string_view text, DrawingScale& into) noexcept {
    const auto readInt = [](std::string_view part, int& value) noexcept {
        if (part.empty()) return false;
        int result = 0;
        for (const char c : part) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) return false;
            // Bounded so a long run of digits cannot wrap into a negative.
            if (result > 100000) return false;
            result = result * 10 + (c - '0');
        }
        if (result <= 0) return false;
        value = result;
        return true;
    };

    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) {
        // "1" means 1:1. Accepted because it is what somebody types for a
        // full-size drawing, and refusing it would be pedantry.
        int whole = 0;
        if (!readInt(text, whole)) return false;
        into = DrawingScale{whole, 1};
        return true;
    }
    int numerator = 0;
    int denominator = 0;
    if (!readInt(text.substr(0, colon), numerator)) return false;
    if (!readInt(text.substr(colon + 1), denominator)) return false;
    into = DrawingScale{numerator, denominator};
    return true;
}

Sheet::Sheet(SheetSize size, SheetOrientation orientation)
    : size_(size), orientation_(orientation) {}

void Sheet::setSize(SheetSize size) noexcept { size_ = size; }

void Sheet::setOrientation(SheetOrientation orientation) noexcept {
    orientation_ = orientation;
}

void Sheet::setScale(const DrawingScale& scale) noexcept {
    if (scale.valid()) scale_ = scale;
}

bool Sheet::setCustomSize(double widthMm, double heightMm) noexcept {
    if (!(widthMm > 0.0) || !(heightMm > 0.0)) return false;
    customWidthMm_ = widthMm;
    customHeightMm_ = heightMm;
    size_ = SheetSize::Custom;
    return true;
}

// A CUSTOM SHEET IGNORES ORIENTATION, and that is not an oversight.
//
// Orientation exists to say which way round a NAMED size is used: A3 is 297 x
// 420 and everybody knows it, so "landscape" is the only way to ask for the
// other one. A custom sheet has no such name -- the two numbers the user typed
// ARE the width and the height. Orienting them would mean 500 x 250 typed as
// "landscape" came out 250 x 500, which is the opposite of what was asked for.
//
// Found by M32_UNDO_002, which measured the paper instead of trusting it.
double Sheet::widthMm() const noexcept {
    if (size_ == SheetSize::Custom) return customWidthMm_;
    const PaperSize paper = PortraitOf(size_);
    return orientation_ == SheetOrientation::Portrait ? paper.widthMm : paper.heightMm;
}

double Sheet::heightMm() const noexcept {
    if (size_ == SheetSize::Custom) return customHeightMm_;
    const PaperSize paper = PortraitOf(size_);
    return orientation_ == SheetOrientation::Portrait ? paper.heightMm : paper.widthMm;
}

} // namespace paramcad
