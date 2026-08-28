#include "Core/Library/SpurGear.h"

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Text/NumberText.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace paramcad {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDefaultPressureAngleDeg = 20.0;

// HOW MANY CHORDS PER FLANK. Twelve puts the deviation from the true involute
// below a micron for every module in the table -- a hobbed gear is held to
// hundredths -- and keeps the sketch small enough that a 100-tooth gear is a
// few thousand entities rather than tens of thousands.
constexpr int kFlankSteps = 12;

bool Same(double a, double b) noexcept { return std::fabs(a - b) < 1e-9; }

double Radians(double degrees) noexcept { return degrees * kPi / 180.0; }

// THE INVOLUTE FUNCTION, inv(a) = tan(a) - a. It appears twice below -- once
// for where a flank starts on the pitch circle and once for the contact ratio
// -- and it is written once for the reason every shared formula in this
// project is.
double Involute(double angleRad) noexcept { return std::tan(angleRad) - angleRad; }

// A point on the involute of a circle of radius `base`, at radius `at`.
// Undefined inside the base circle, where the involute does not exist -- the
// caller clamps rather than asking.
Vec2 InvolutePointAt(double base, double at, double phase) {
    const double alpha = std::acos(std::clamp(base / at, -1.0, 1.0));
    const double theta = phase + Involute(alpha);
    return Vec2{at * std::cos(theta), at * std::sin(theta)};
}

bool ReadNumber(std::string_view text, std::size_t& at, double& out) {
    const std::size_t start = at;
    bool seenPoint = false;
    while (at < text.size()) {
        const char c = text[at];
        if (c >= '0' && c <= '9') {
            ++at;
        } else if (c == '.' && !seenPoint) {
            seenPoint = true;
            ++at;
        } else {
            break;
        }
    }
    if (at == start) return false;
    out = std::stod(std::string(text.substr(start, at - start)));
    return true;
}

bool Expect(std::string_view text, std::size_t& at, char c) {
    if (at >= text.size() || text[at] != c) return false;
    ++at;
    return true;
}

void SkipSpaces(std::string_view text, std::size_t& at) {
    while (at < text.size() && text[at] == ' ') ++at;
}

} // namespace

const std::vector<double>& StandardModules() {
    // ISO 54 series I, the preferred one. Series II exists and is what you
    // reach for when series I will not give the centre distance you need; it
    // is left out until something asks, because every module here is a cutter
    // a shop is likely to own and the second series is not.
    static const std::vector<double> modules{0.5, 0.6,  0.8, 1.0, 1.25, 1.5, 2.0, 2.5,
                                             3.0, 4.0,  5.0, 6.0, 8.0,  10.0};
    return modules;
}

const std::vector<double>& StandardPressureAngles() {
    static const std::vector<double> angles{20.0, 14.5};
    return angles;
}

double SpurGear::pitchDiameterMm() const noexcept {
    return moduleMm * static_cast<double>(teeth);
}
double SpurGear::baseDiameterMm() const noexcept {
    return pitchDiameterMm() * std::cos(Radians(pressureAngleDeg));
}
double SpurGear::tipDiameterMm() const noexcept {
    return moduleMm * (static_cast<double>(teeth) + 2.0);
}
double SpurGear::rootDiameterMm() const noexcept {
    return moduleMm * (static_cast<double>(teeth) - 2.5);
}
double SpurGear::toothThicknessMm() const noexcept { return kPi * moduleMm / 2.0; }

std::string SpurGear::designation() const {
    std::string out = "m" + ShortNumber(moduleMm) + " z" + std::to_string(teeth) + " b" +
                      ShortNumber(faceWidthMm);
    if (!Same(pressureAngleDeg, kDefaultPressureAngleDeg))
        out += " a" + ShortNumber(pressureAngleDeg);
    return out;
}

std::optional<SpurGear> LookUpGear(std::string_view designation) {
    const std::optional<SpurGear> read = ParseGearDesignation(designation);
    if (!read || !WhyGearRefused(*read).empty()) return std::nullopt;
    return read;
}

std::optional<SpurGear> ParseGearDesignation(std::string_view designation) {
    SpurGear gear;
    std::size_t at = 0;
    SkipSpaces(designation, at);
    if (!Expect(designation, at, 'm')) return std::nullopt;
    if (!ReadNumber(designation, at, gear.moduleMm)) return std::nullopt;
    SkipSpaces(designation, at);
    if (!Expect(designation, at, 'z')) return std::nullopt;
    double teeth = 0.0;
    if (!ReadNumber(designation, at, teeth)) return std::nullopt;
    gear.teeth = static_cast<int>(teeth);
    if (!Same(static_cast<double>(gear.teeth), teeth)) return std::nullopt;
    SkipSpaces(designation, at);
    if (!Expect(designation, at, 'b')) return std::nullopt;
    if (!ReadNumber(designation, at, gear.faceWidthMm)) return std::nullopt;
    SkipSpaces(designation, at);
    if (at != designation.size()) {
        if (!Expect(designation, at, 'a')) return std::nullopt;
        if (!ReadNumber(designation, at, gear.pressureAngleDeg)) return std::nullopt;
        SkipSpaces(designation, at);
        if (at != designation.size()) return std::nullopt;
    }
    return gear;
}

int MinimumTeethWithoutUndercut(double pressureAngleDeg) noexcept {
    // z_min = 2 / sin^2(alpha), rounded UP: the limit is where undercut
    // begins, so the first count that is clear of it is the next whole tooth.
    const double sine = std::sin(Radians(pressureAngleDeg));
    if (sine <= 0.0) return 0;
    return static_cast<int>(std::ceil(2.0 / (sine * sine)));
}

std::string WhyGearRefused(const SpurGear& gear) {
    const std::vector<double>& modules = StandardModules();
    if (std::none_of(modules.begin(), modules.end(),
                     [&](double m) { return Same(m, gear.moduleMm); }))
        return "module " + ShortNumber(gear.moduleMm) +
               " is not one this program cuts -- a module is which cutter the shop owns, "
               "not a number to choose";
    const std::vector<double>& angles = StandardPressureAngles();
    if (std::none_of(angles.begin(), angles.end(),
                     [&](double a) { return Same(a, gear.pressureAngleDeg); }))
        return "a pressure angle of " + ShortNumber(gear.pressureAngleDeg) +
               " degrees has no cutter; this program knows 20 and 14.5";
    if (!(gear.faceWidthMm > 0.0))
        return "a gear has to have a width, and " + ShortNumber(gear.faceWidthMm) + " is not one";
    // UNDERCUT. The tooth would still be there and would still look like a
    // tooth, with part of its flank cut away near the root by the generating
    // cutter -- weaker and rougher than the drawing says.
    const int minimum = MinimumTeethWithoutUndercut(gear.pressureAngleDeg);
    if (gear.teeth < minimum)
        return std::to_string(gear.teeth) + " teeth undercuts at " +
               ShortNumber(gear.pressureAngleDeg) + " degrees: the cutter would sweep into "
               "the flank near the root and take part of the involute with it. " +
               std::to_string(minimum) +
               " is the fewest that does not, and a profile shift -- which this program "
               "does not do -- is the other way round it";
    return {};
}

std::string WhyPairRefused(const SpurGear& driver, const SpurGear& driven) {
    const std::string first = WhyGearRefused(driver);
    if (!first.empty()) return "the driver cannot be cut: " + first;
    const std::string second = WhyGearRefused(driven);
    if (!second.empty()) return "the driven gear cannot be cut: " + second;
    if (!Same(driver.moduleMm, driven.moduleMm))
        return "module " + ShortNumber(driver.moduleMm) + " and module " +
               ShortNumber(driven.moduleMm) +
               " have teeth of different size, so no centre distance makes them run";
    if (!Same(driver.pressureAngleDeg, driven.pressureAngleDeg))
        return ShortNumber(driver.pressureAngleDeg) + " and " +
               ShortNumber(driven.pressureAngleDeg) +
               " degrees are different flank curves, so these two will not roll on each other";
    return {};
}

double CentreDistanceMm(const SpurGear& driver, const SpurGear& driven) {
    if (!WhyPairRefused(driver, driven).empty()) return 0.0;
    return driver.moduleMm * (static_cast<double>(driver.teeth) +
                              static_cast<double>(driven.teeth)) / 2.0;
}

double GearRatio(const SpurGear& driver, const SpurGear& driven) {
    if (!WhyPairRefused(driver, driven).empty()) return 0.0;
    if (driven.teeth == 0) return 0.0;
    // NEGATIVE, because two external gears in mesh turn opposite ways. That is
    // a fact about the machine, so it lives in the number rather than in a
    // flag somebody sets beside it.
    return -static_cast<double>(driver.teeth) / static_cast<double>(driven.teeth);
}

double ContactRatio(const SpurGear& driver, const SpurGear& driven) {
    if (!WhyPairRefused(driver, driven).empty()) return 0.0;
    const double alpha = Radians(driver.pressureAngleDeg);
    const double centres = CentreDistanceMm(driver, driven);
    const double basePitch = kPi * driver.moduleMm * std::cos(alpha);
    if (basePitch <= 0.0) return 0.0;

    // The length of the path of contact: how far along the line of action the
    // two flanks stay touching. Each gear contributes the part of its tip
    // circle that reaches past the pitch point.
    const auto reach = [](const SpurGear& gear) {
        const double tip = gear.tipDiameterMm() / 2.0;
        const double base = gear.baseDiameterMm() / 2.0;
        return std::sqrt(std::max(0.0, tip * tip - base * base));
    };
    const double contact = reach(driver) + reach(driven) - centres * std::sin(alpha);
    return contact / basePitch;
}

bool IsSpurGearPath(std::string_view path) noexcept {
    return path.size() > kSpurGearScheme.size() &&
           path.substr(0, kSpurGearScheme.size()) == kSpurGearScheme;
}

std::string SpurGearPath(const SpurGear& gear) {
    return std::string(kSpurGearScheme) + gear.designation();
}

std::optional<SpurGear> SpurGearOfPath(std::string_view path) {
    if (!IsSpurGearPath(path)) return std::nullopt;
    return LookUpGear(path.substr(kSpurGearScheme.size()));
}

std::vector<Vec2> SpurGearOutline(const SpurGear& gear) {
    std::vector<Vec2> loop;
    if (!WhyGearRefused(gear).empty()) return loop;

    const double alpha = Radians(gear.pressureAngleDeg);
    const double pitch = gear.pitchDiameterMm() / 2.0;
    const double base = gear.baseDiameterMm() / 2.0;
    const double tip = gear.tipDiameterMm() / 2.0;
    const double root = gear.rootDiameterMm() / 2.0;
    const double pitchStep = 2.0 * kPi / static_cast<double>(gear.teeth);

    // WHERE THE FLANK CROSSES THE PITCH CIRCLE decides the tooth's thickness,
    // and that is the whole of meshing: half the circular pitch of tooth and
    // half of space. Everything else about the profile follows from the
    // involute.
    const double halfToothAtPitch = pitchStep / 4.0;
    // Walking the involute out from the base circle turns it by inv(alpha) by
    // the time it reaches the pitch circle, so the flank has to START that far
    // back for it to arrive in the right place.
    const double phase = halfToothAtPitch + Involute(alpha);

    // The flank never starts below the base circle -- the involute does not
    // exist there. On a gear whose root is inside the base circle the profile
    // drops radially, which is what a generated tooth does too.
    const double from = std::max(root, base);

    for (int tooth = 0; tooth < gear.teeth; ++tooth) {
        const double centre = pitchStep * static_cast<double>(tooth);

        // Up the leading flank...
        for (int step = 0; step <= kFlankSteps; ++step) {
            const double at =
                from + (tip - from) * static_cast<double>(step) / static_cast<double>(kFlankSteps);
            const Vec2 point = InvolutePointAt(base, std::max(at, base), -phase);
            loop.push_back(Vec2{point.x * std::cos(centre) - point.y * std::sin(centre),
                                point.x * std::sin(centre) + point.y * std::cos(centre)});
        }
        // ACROSS THE TIP LAND, which is an ARC and not a chord.
        //
        // Found by the kernel tests: a gear whose tip was one straight chord
        // came out consistently narrower than its own tip diameter -- by the
        // sagitta of that chord, twice, which is 0.022 mm on a 44 mm gear. Not
        // an error worth a tolerance: a real tooth's tip IS a circular land,
        // and drawing it as one costs three points and makes the outside
        // diameter -- the number a gear is measured over -- the number it says.
        {
            const Vec2 top = InvolutePointAt(base, tip, -phase);
            const double from_ = std::atan2(top.y, top.x) + centre;
            const double to_ = std::atan2(-top.y, top.x) + centre;
            constexpr int kTipSteps = 4;
            for (int step = 1; step < kTipSteps; ++step) {
                const double sweep = from_ + (to_ - from_) * static_cast<double>(step) /
                                                 static_cast<double>(kTipSteps);
                loop.push_back(Vec2{tip * std::cos(sweep), tip * std::sin(sweep)});
            }
        }

        // ...and down the trailing one, which is the leading flank mirrored in
        // the tooth's own centre line. Mirrored rather than generated a second
        // time: two derivations of one curve is how the two flanks of a tooth
        // come to be different shapes.
        for (int step = kFlankSteps; step >= 0; --step) {
            const double at =
                from + (tip - from) * static_cast<double>(step) / static_cast<double>(kFlankSteps);
            const Vec2 point = InvolutePointAt(base, std::max(at, base), -phase);
            const Vec2 mirrored{point.x, -point.y};
            loop.push_back(Vec2{mirrored.x * std::cos(centre) - mirrored.y * std::sin(centre),
                                mirrored.x * std::sin(centre) + mirrored.y * std::cos(centre)});
        }
        // Across the root, to the next tooth.
        const double nextCentre = centre + pitchStep;
        const Vec2 rootStart = InvolutePointAt(base, std::max(from, base), -phase);
        const double rootAngle = std::atan2(-rootStart.y, rootStart.x) + centre;
        const double nextRootAngle = std::atan2(rootStart.y, rootStart.x) + nextCentre;
        // A few points round the root arc, so the gap between two teeth is a
        // circle rather than a chord that cuts into the next tooth.
        constexpr int kRootSteps = 4;
        for (int step = 1; step < kRootSteps; ++step) {
            const double sweep = rootAngle + (nextRootAngle - rootAngle) *
                                                 static_cast<double>(step) /
                                                 static_cast<double>(kRootSteps);
            loop.push_back(Vec2{root * std::cos(sweep), root * std::sin(sweep)});
        }
    }
    return loop;
}

std::unique_ptr<PartDocument> BuildSpurGear(const SpurGear& gear) {
    const std::vector<Vec2> outline = SpurGearOutline(gear);
    if (outline.size() < 3) return nullptr;

    auto part = std::make_unique<PartDocument>(gear.designation());
    Body& body = part->addBody(gear.designation());
    Sketch& teeth = part->addSketch("Teeth");
    for (std::size_t i = 0; i < outline.size(); ++i)
        part->addSketchEntity(teeth.id(),
                              SketchLine{outline[i], outline[(i + 1) % outline.size()]});

    Parameter& width = part->addParameter("Face width", gear.faceWidthMm, UnitType::Millimeter);
    part->addPadFeature(body, "Blank", teeth.id(), width.id());
    return part;
}

} // namespace paramcad
