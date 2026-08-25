#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace paramcad {

// TOLERANCES (M37).
//
// A dimension without one is incomplete on anything that has to fit. "25" on a
// bore tells the machinist the size and nothing about how close they have to
// be, so they telephone -- and on a drawing with no general tolerance table,
// every unmarked dimension is undefined rather than loose.
//
// This header is the VALUE side: what a tolerance is, and what ISO 286 says a
// fit code means. What carries one is DrawingDimension; what prints it is
// DimensionStyle::format, which is already the single reader the canvas, the
// plot and the DXF all go through.

enum class ToleranceKind {
    None,
    // 25 ±0.1 -- one number, both ways.
    Symmetric,
    // 25 +0.2 / -0.1 -- two numbers, and they may both be positive or both
    // negative. Kept apart from Symmetric because a reader treats them
    // differently: symmetric means "centred", deviation means "biased".
    Deviation,
    // 25.2 / 24.9 -- the two SIZES rather than the two deviations. The same
    // information, and drawings use both; which one is shown is a house style.
    Limits,
    // A boxed dimension: theoretically exact, tolerance comes from a geometric
    // control elsewhere. It has no numbers of its own by definition.
    Basic,
    // H7, g6 -- a FIT. The deviations are NOT stored (see FitDeviation).
    Fit,
};

std::string_view toString(ToleranceKind kind) noexcept;
bool ParseToleranceKind(std::string_view text, ToleranceKind& into) noexcept;

// THE DEVIATIONS, IN MILLIMETRES, upper first.
//
// Upper is always the larger: a fit whose upper came out below its lower would
// describe a hole nothing can be made to, and it is worth having a type that
// cannot say so.
struct Deviations {
    double upperMm = 0.0;
    double lowerMm = 0.0;
    bool ordered() const noexcept { return upperMm >= lowerMm; }
};

// WHAT A FIT CODE MEANS, at a nominal size (ISO 286-1).
//
// STORE THE CODE, DERIVE THE NUMBERS. A drawing keeps "H7"; the deviations are
// computed whenever they are needed. Storing them would put a second answer in
// the file -- and the day the table is corrected, every drawing already made
// would keep the old numbers and look right.
//
// REFUSED RATHER THAN GUESSED. A code outside what this implements comes back
// as nothing, and the dimension says so. Defaulting an unknown fit to zero
// deviation would turn a specified fit into an unspecified one, silently, on a
// feature somebody chose a fit for on purpose.
//
// WHAT IS IMPLEMENTED: grades IT5-IT13 over the size steps to 500 mm, hole H
// and shaft h g f e d k m n p s -- which is every fit in ordinary mechanical
// use. Other letters return nothing rather than an approximation.
std::optional<Deviations> FitDeviation(double nominalMm, std::string_view code) noexcept;

// The IT grade's tolerance width alone, in millimetres. Public because a
// general-tolerance table needs it too, and because it is the half of the
// calculation worth checking against a published table on its own.
std::optional<double> StandardToleranceMm(double nominalMm, int grade) noexcept;

// WHAT ONE DIMENSION CARRIES.
//
// A fit stores its CODE and nothing else -- see FitDeviation. The two numbers
// are meaningful only for the kinds that state numbers, and a Basic dimension
// has none by definition.
struct DimensionTolerance {
    ToleranceKind kind = ToleranceKind::None;
    double upperMm = 0.0;
    double lowerMm = 0.0;
    std::string fitCode;
    // -1 means "follow the dimension style". A tolerance is usually shown to
    // one more decimal than the size it qualifies, and a drawing where that
    // was typed per dimension is one where half of them drift.
    int decimals = -1;

    bool statesNumbers() const noexcept {
        return kind == ToleranceKind::Symmetric || kind == ToleranceKind::Deviation ||
               kind == ToleranceKind::Limits;
    }
};

// --- General tolerances (ISO 2768-1) -----------------------------------------
//
// WHAT AN UNMARKED DIMENSION MEANS. A PROPERTY OF THE SHEET, not of a
// dimension -- exactly like the projection angle, and for the same reason: a
// drawing where two dimensions answered to two different general classes is
// one no reader can use, and the class is printed once beside the title block.
enum class GeneralToleranceClass {
    None, // nothing stated -- and the drawing then says nothing about unmarked sizes
    Fine,   // f
    Medium, // m
    Coarse, // c
    VeryCoarse, // v
};

std::string_view toString(GeneralToleranceClass klass) noexcept;
bool ParseGeneralToleranceClass(std::string_view text, GeneralToleranceClass& into) noexcept;
// What ISO 2768 prints: "ISO 2768-m".
std::string GeneralToleranceNote(GeneralToleranceClass klass);

// The permitted deviation for a plain linear size under `klass`, in
// millimetres -- symmetric, so one number. Nothing when the class is None, or
// when the size is below 0.5 mm, which ISO 2768 leaves to be marked
// individually rather than covering.
std::optional<double> GeneralToleranceMm(double nominalMm, GeneralToleranceClass klass) noexcept;

} // namespace paramcad
