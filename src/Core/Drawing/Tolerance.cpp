#include "Core/Drawing/Tolerance.h"

#include <cctype>
#include <cmath>

namespace paramcad {

namespace {

// THE SIZE STEPS ISO 286 IS BUILT ON, in millimetres. Every tolerance is
// constant within a step -- so 18.1 and 29.9 get the same one, which is what
// makes a table possible at all.
//
// The steps are "over A up to and INCLUDING B", so 30 belongs to 18..30 and
// not to 30..50. Getting that boundary wrong shifts every tolerance on a 30 mm
// shaft by a whole band, and the drawing looks exactly the same.
constexpr double kSteps[] = {0.0,   3.0,   6.0,   10.0,  18.0,  30.0,  50.0,
                             80.0,  120.0, 180.0, 250.0, 315.0, 400.0, 500.0};
constexpr int kStepCount = 13; // the number of BANDS, one fewer than the bounds

// THIS IS A TABLE, NOT A FORMULA, AND THAT IS THE POINT.
//
// ISO 286 does give an expression -- 0.45*cbrt(D) + 0.001*D, times a
// multiplier per grade -- and a first draft used it. It comes within one
// micrometre of the published values and is wrong on four of the thirteen
// steps: IT7 at 6..10 computes 14.4 where the standard prints 15.
//
// ON A FINE FIT A MICROMETRE IS THE WHOLE ARGUMENT. An H7 bore one micrometre
// narrow than the standard says is a drawing that specifies something nobody
// else's H7 will match, and there is no way to see it: the number looks
// entirely reasonable, and nobody re-derives an H7 by hand.
//
// So the published values are written down. Rows are the size steps above,
// columns are IT5..IT13, micrometres.
constexpr double kIT[kStepCount][9] = {
    /* 1..3   */ {4, 6, 10, 14, 25, 40, 60, 100, 140},
    /* 3..6   */ {5, 8, 12, 18, 30, 48, 75, 120, 180},
    /* 6..10  */ {6, 9, 15, 22, 36, 58, 90, 150, 220},
    /* 10..18 */ {8, 11, 18, 27, 43, 70, 110, 180, 270},
    /* 18..30 */ {9, 13, 21, 33, 52, 84, 130, 210, 330},
    /* 30..50 */ {11, 16, 25, 39, 62, 100, 160, 250, 390},
    /* 50..80 */ {13, 19, 30, 46, 74, 120, 190, 300, 460},
    /* 80..120*/ {15, 22, 35, 54, 87, 140, 220, 350, 540},
    /*120..180*/ {18, 25, 40, 63, 100, 160, 250, 400, 630},
    /*180..250*/ {20, 29, 46, 72, 115, 185, 290, 460, 720},
    /*250..315*/ {23, 32, 52, 81, 130, 210, 320, 520, 810},
    /*315..400*/ {25, 36, 57, 89, 140, 230, 360, 570, 890},
    /*400..500*/ {27, 40, 63, 97, 155, 250, 400, 630, 970},
};

// THE FUNDAMENTAL DEVIATIONS, also published rather than computed, and for
// exactly the same reason -- d at 18..30 computes -63.9 where the standard
// prints -65.
//
// The fundamental deviation is the one NEAREST the zero line; its sign says
// which side of nominal the whole zone sits on, and that is the entire content
// of the letter. For the clearance letters it is the UPPER deviation; for the
// transition and interference letters it is the LOWER.
constexpr double kShaftD[kStepCount] = {-20, -30, -40, -50, -65, -80, -100,
                                        -120, -145, -170, -190, -210, -230};
constexpr double kShaftE[kStepCount] = {-14, -20, -25, -32, -40, -50, -60,
                                        -72, -85, -100, -110, -125, -135};
constexpr double kShaftF[kStepCount] = {-6, -10, -13, -16, -20, -25, -30,
                                        -36, -43, -50, -56, -62, -68};
constexpr double kShaftG[kStepCount] = {-2, -4, -5, -6, -7, -9, -10,
                                        -12, -14, -15, -17, -18, -20};
constexpr double kShaftK[kStepCount] = {0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4, 5};
constexpr double kShaftM[kStepCount] = {2, 4, 6, 7, 8, 9, 11, 13, 15, 17, 20, 21, 23};
constexpr double kShaftN[kStepCount] = {4, 8, 10, 12, 15, 17, 20, 23, 27, 31, 34, 37, 40};
constexpr double kShaftP[kStepCount] = {6, 12, 15, 18, 22, 26, 32, 37, 43, 50, 56, 62, 68};
// s is written down only to 50 mm. Above that ISO splits the steps again --
// 50..65 and 65..80 differ -- and one value for both halves would be wrong on
// half the sizes in the step.
constexpr int kShaftSSteps = 6;
constexpr double kShaftS[kShaftSSteps] = {14, 19, 23, 28, 35, 43};

// Which band `nominalMm` is in, or -1 when it is outside the table.
int StepIndex(double nominalMm) noexcept {
    if (!(nominalMm > 0.0) || nominalMm > kSteps[kStepCount]) return -1;
    for (int i = 1; i <= kStepCount; ++i)
        if (nominalMm <= kSteps[i]) return i - 1;
    return -1;
}

} // namespace

std::string_view toString(ToleranceKind kind) noexcept {
    switch (kind) {
        case ToleranceKind::None: return "None";
        case ToleranceKind::Symmetric: return "Symmetric";
        case ToleranceKind::Deviation: return "Deviation";
        case ToleranceKind::Limits: return "Limits";
        case ToleranceKind::Basic: return "Basic";
        case ToleranceKind::Fit: return "Fit";
    }
    return "None";
}

bool ParseToleranceKind(std::string_view text, ToleranceKind& into) noexcept {
    if (text == "None") { into = ToleranceKind::None; return true; }
    if (text == "Symmetric") { into = ToleranceKind::Symmetric; return true; }
    if (text == "Deviation") { into = ToleranceKind::Deviation; return true; }
    if (text == "Limits") { into = ToleranceKind::Limits; return true; }
    if (text == "Basic") { into = ToleranceKind::Basic; return true; }
    if (text == "Fit") { into = ToleranceKind::Fit; return true; }
    return false;
}

std::optional<double> StandardToleranceMm(double nominalMm, int grade) noexcept {
    const int step = StepIndex(nominalMm);
    if (step < 0) return std::nullopt;
    // IT5 to IT13. The finer grades have their own rules and the coarser ones
    // are not used on a general engineering drawing -- and answering for a
    // grade that is not in the table would be inventing one.
    if (grade < 5 || grade > 13) return std::nullopt;
    return kIT[step][grade - 5] / 1000.0;
}

std::optional<Deviations> FitDeviation(double nominalMm, std::string_view code) noexcept {
    if (code.size() < 2) return std::nullopt;
    const char letter = code.front();
    // The grade is the rest, and it must be ALL digits: "H7x" is not a fit, and
    // reading it as H7 would accept a typo as a specification.
    int grade = 0;
    for (std::size_t i = 1; i < code.size(); ++i) {
        const char digit = code[i];
        if (digit < '0' || digit > '9') return std::nullopt;
        grade = grade * 10 + (digit - '0');
    }
    const std::optional<double> width = StandardToleranceMm(nominalMm, grade);
    if (!width.has_value()) return std::nullopt;
    const int step = StepIndex(nominalMm);
    if (step < 0) return std::nullopt;

    const bool isHole = std::isupper(static_cast<unsigned char>(letter)) != 0;
    const char shaftLetter =
        static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));

    // --- The hole side ------------------------------------------------------
    //
    // ONLY H, and that is a deliberate limit rather than an oversight:
    // hole-basis fitting -- an H hole and a lettered shaft -- is what
    // essentially all mechanical design uses, because a reamer is a fixed size
    // and a shaft can be turned to anything. Another hole letter comes back as
    // nothing, so the drawing says it does not know instead of quietly
    // printing an H7 the designer never asked for.
    if (isHole) {
        if (shaftLetter != 'h') return std::nullopt;
        Deviations out;
        out.upperMm = *width;
        out.lowerMm = 0.0;
        return out;
    }

    // --- The shaft side -----------------------------------------------------
    Deviations out;
    const auto fromClearance = [&](double microns) {
        // The fundamental deviation is the UPPER one; the lower is it minus
        // the grade's width.
        out.upperMm = microns / 1000.0;
        out.lowerMm = out.upperMm - *width;
        return out;
    };
    const auto fromInterference = [&](double microns) {
        // ...and here it is the LOWER one.
        out.lowerMm = microns / 1000.0;
        out.upperMm = out.lowerMm + *width;
        return out;
    };

    switch (shaftLetter) {
        case 'h': return fromClearance(0.0);
        case 'g': return fromClearance(kShaftG[step]);
        case 'f': return fromClearance(kShaftF[step]);
        case 'e': return fromClearance(kShaftE[step]);
        case 'd': return fromClearance(kShaftD[step]);
        case 'k':
            // A TRANSITION FIT ONLY IN THE GRADES THE STANDARD SAYS SO.
            // Outside IT4..IT7, k is simply h -- and zero here means that,
            // not a missing value.
            return fromInterference((grade < 4 || grade > 7) ? 0.0 : kShaftK[step]);
        case 'm': return fromInterference(kShaftM[step]);
        case 'n': return fromInterference(kShaftN[step]);
        case 'p': return fromInterference(kShaftP[step]);
        case 's':
            if (step >= kShaftSSteps) return std::nullopt;
            return fromInterference(kShaftS[step]);
        default:
            // NOT GUESSED AT. An unimplemented letter is a fit this build
            // cannot state, and saying nothing is the only honest answer.
            return std::nullopt;
    }
}

std::string_view toString(GeneralToleranceClass klass) noexcept {
    switch (klass) {
        case GeneralToleranceClass::None: return "None";
        case GeneralToleranceClass::Fine: return "Fine";
        case GeneralToleranceClass::Medium: return "Medium";
        case GeneralToleranceClass::Coarse: return "Coarse";
        case GeneralToleranceClass::VeryCoarse: return "VeryCoarse";
    }
    return "None";
}

bool ParseGeneralToleranceClass(std::string_view text, GeneralToleranceClass& into) noexcept {
    if (text == "None") { into = GeneralToleranceClass::None; return true; }
    if (text == "Fine") { into = GeneralToleranceClass::Fine; return true; }
    if (text == "Medium") { into = GeneralToleranceClass::Medium; return true; }
    if (text == "Coarse") { into = GeneralToleranceClass::Coarse; return true; }
    if (text == "VeryCoarse") { into = GeneralToleranceClass::VeryCoarse; return true; }
    return false;
}

std::string GeneralToleranceNote(GeneralToleranceClass klass) {
    switch (klass) {
        case GeneralToleranceClass::None: return {};
        case GeneralToleranceClass::Fine: return "ISO 2768-f";
        case GeneralToleranceClass::Medium: return "ISO 2768-m";
        case GeneralToleranceClass::Coarse: return "ISO 2768-c";
        case GeneralToleranceClass::VeryCoarse: return "ISO 2768-v";
    }
    return {};
}

std::optional<double> GeneralToleranceMm(double nominalMm,
                                         GeneralToleranceClass klass) noexcept {
    if (klass == GeneralToleranceClass::None) return std::nullopt;
    // ISO 2768-1's own table, in millimetres. A table again, and here there
    // was never a formula to be tempted by.
    static const double kSteps2768[] = {0.5, 3.0, 6.0, 30.0, 120.0, 400.0, 1000.0, 2000.0,
                                        4000.0};
    static const double kFine[] = {0.05, 0.05, 0.1, 0.15, 0.2, 0.3, 0.5, 0.0};
    static const double kMedium[] = {0.1, 0.1, 0.2, 0.3, 0.5, 0.8, 1.2, 2.0};
    static const double kCoarse[] = {0.2, 0.3, 0.5, 0.8, 1.2, 2.0, 3.0, 4.0};
    static const double kVeryCoarse[] = {0.0, 0.5, 1.0, 1.5, 2.5, 4.0, 6.0, 8.0};

    // BELOW 0.5 mm ISO 2768 SAYS NOTHING, and neither does this: the standard
    // asks for those to be marked individually, so answering would be putting
    // a tolerance on a size it deliberately leaves open.
    if (nominalMm < kSteps2768[0]) return std::nullopt;
    int row = -1;
    for (int i = 1; i < 9; ++i) {
        if (nominalMm > kSteps2768[i]) continue;
        row = i - 1;
        break;
    }
    if (row < 0) return std::nullopt;

    double value = 0.0;
    switch (klass) {
        case GeneralToleranceClass::Fine: value = kFine[row]; break;
        case GeneralToleranceClass::Medium: value = kMedium[row]; break;
        case GeneralToleranceClass::Coarse: value = kCoarse[row]; break;
        case GeneralToleranceClass::VeryCoarse: value = kVeryCoarse[row]; break;
        case GeneralToleranceClass::None: return std::nullopt;
    }
    // A zero in the table means the standard does not cover that class at that
    // size -- f above 2000 mm, v below 3 mm -- and a zero tolerance would be
    // an impossible requirement rather than a missing one.
    if (!(value > 0.0)) return std::nullopt;
    return value;
}

} // namespace paramcad
