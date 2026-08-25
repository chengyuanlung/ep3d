#pragma once

#include <string>
#include <string_view>

namespace paramcad {

// THE PAPER (M32, roadmap §24).
//
// A drawing is the third document type, and the first one whose contents have
// a PHYSICAL SIZE. A part and an assembly are modelled in millimetres and
// looked at through whatever window is open; a drawing is a piece of paper of
// a stated size that somebody prints.
//
// So the sheet is document state, not presentation. It decides where a view
// can sit, what the title block frames, and what "1:2" means when it is
// printed. A drawing whose paper size lived in the viewer would print
// differently on two machines.
enum class SheetSize {
    A0, // 841 x 1189 mm
    A1, // 594 x 841
    A2, // 420 x 594
    A3, // 297 x 420
    A4, // 210 x 297
    Custom,
};

std::string_view toString(SheetSize size) noexcept;

enum class SheetOrientation { Portrait, Landscape };

std::string_view toString(SheetOrientation orientation) noexcept;

// A SCALE IS A RATIO, NOT A NUMBER.
//
// "1:3" is what a title block prints and what a reader measures against, and
// 0.333333 is not that -- it is a rounding of it. Stored as two integers so
// the sentence survives the file, the arithmetic and the print, and so 1:3 and
// 2:6 stay distinguishable if somebody types the second.
//
// The same decision Relation::ratio made for millimetres per turn, for the
// same reason: keep what the user said, convert in one place.
struct DrawingScale {
    int numerator{1};   // paper
    int denominator{1}; // model

    // Paper millimetres per model millimetre. THE one conversion site.
    double factor() const noexcept;
    bool valid() const noexcept { return numerator > 0 && denominator > 0; }
    // "1:2", "2:1", "1:1".
    std::string toString() const;
};

bool operator==(const DrawingScale& a, const DrawingScale& b) noexcept;
inline bool operator!=(const DrawingScale& a, const DrawingScale& b) noexcept {
    return !(a == b);
}

// Parses "1:2", "2:1", "1" -- or fails, rather than guessing. A scale nobody
// can read back is a title block that prints a lie.
bool ParseDrawingScale(std::string_view text, DrawingScale& into) noexcept;

class Sheet {
public:
    Sheet() = default;
    Sheet(SheetSize size, SheetOrientation orientation);

    SheetSize size() const noexcept { return size_; }
    SheetOrientation orientation() const noexcept { return orientation_; }
    const DrawingScale& scale() const noexcept { return scale_; }

    void setSize(SheetSize size) noexcept;
    void setOrientation(SheetOrientation orientation) noexcept;
    void setScale(const DrawingScale& scale) noexcept;

    // Custom paper. Refused at or below zero -- a sheet with no area is a
    // sheet no view can be placed on, and it would fail much later.
    bool setCustomSize(double widthMm, double heightMm) noexcept;

    // The paper, in millimetres, WITH the orientation already applied. One
    // reader, so nothing else has to remember to swap them.
    double widthMm() const noexcept;
    double heightMm() const noexcept;

    // The custom size AS TYPED -- portrait, unoriented. Only the file needs
    // these: writing the oriented pair would make a landscape custom sheet
    // come back rotated, because the reader orients them again.
    double customWidthMm() const noexcept { return customWidthMm_; }
    double customHeightMm() const noexcept { return customHeightMm_; }

private:
    SheetSize size_{SheetSize::A3};
    SheetOrientation orientation_{SheetOrientation::Landscape};
    DrawingScale scale_{1, 1};
    // Only read when size_ is Custom. Kept rather than folded into
    // width/height so switching to A3 and back does not lose what was typed.
    double customWidthMm_{420.0};
    double customHeightMm_{297.0};
};

} // namespace paramcad
