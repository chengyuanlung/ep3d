#include "Core/Library/CompressionSpring.h"

#include "Core/Document/PartDocument.h"
#include "Core/Feature/HelixFeature.h"
#include "Core/Text/NumberText.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace paramcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// THE SHEAR MODULUS OF SPRING STEEL, in newtons per square millimetre
// (EN 10270-1 patented cold-drawn wire). Written down rather than made a
// parameter because a spring generator that let a user type it would let them
// type the wrong one, and the rate would come out plausible and wrong. The day
// a second material is offered, this becomes a row in a table beside its name.
constexpr double kShearModulusMPa = 81500.0;

// WHERE A COIL SPRING CAN BE WOUND, as an index D/d.
//
// Below the lower limit the wire cracks on the mandrel as it is formed --
// the bend is tighter than drawn wire will take. Above the upper one the
// spring tangles with its neighbours in a bin and wanders under load. Every
// spring catalogue prints these two numbers and every spring in one sits
// between them.
constexpr double kLeastIndex = 4.0;
constexpr double kMostIndex = 12.0;

// L0/D above which an unguided spring folds sideways instead of shortening.
constexpr double kSlendernessToWatch = 2.6;

bool Same(double a, double b) noexcept { return std::fabs(a - b) < 1e-9; }

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

struct EndsRow {
    SpringEnds ends;
    std::string_view token; // what a saved file calls it
    char code;              // what a path calls it
    double deadCoils;       // how many coils do not deflect
    double solidCoils;      // how many wire thicknesses the solid height is
};

// ONE ROW PER END STYLE, because the two numbers that differ between them --
// how many coils are dead and how tall the spring is when shut -- are the two
// numbers everything else depends on, and a switch in each of four functions
// is four places to get them wrong.
constexpr EndsRow kEnds[] = {
    {SpringEnds::Plain, "Plain", 'p', 0.0, 1.0},
    {SpringEnds::Closed, "Closed", 'c', 2.0, 3.0},
    // GROUND IS THE SHORTER ONE WHEN SHUT: grinding takes the end coils' own
    // helical rise away, so nt thicknesses stack instead of nt + 1.
    {SpringEnds::ClosedGround, "ClosedGround", 'g', 2.0, 0.0},
};

const EndsRow& RowOf(SpringEnds ends) noexcept {
    for (const EndsRow& row : kEnds)
        if (row.ends == ends) return row;
    return kEnds[2];
}

} // namespace

const std::vector<double>& StandardWireDiameters() {
    static const std::vector<double> wires{0.5, 0.6, 0.8, 1.0, 1.2, 1.6, 2.0, 2.5,
                                           3.0, 3.5, 4.0, 5.0, 6.0, 8.0, 10.0};
    return wires;
}

std::string_view toString(SpringEnds ends) noexcept { return RowOf(ends).token; }

bool ParseSpringEnds(std::string_view text, SpringEnds& into) noexcept {
    for (const EndsRow& row : kEnds) {
        if (row.token != text) continue;
        into = row.ends;
        return true;
    }
    return false;
}

double CompressionSpring::outerDiameterMm() const noexcept { return meanDiameterMm + wireMm; }
double CompressionSpring::innerDiameterMm() const noexcept { return meanDiameterMm - wireMm; }

double CompressionSpring::springIndex() const noexcept {
    return wireMm > 0.0 ? meanDiameterMm / wireMm : 0.0;
}

double CompressionSpring::totalCoilsCount() const noexcept {
    return activeCoils + RowOf(ends).deadCoils;
}

double CompressionSpring::solidLengthMm() const noexcept {
    const EndsRow& row = RowOf(ends);
    return (activeCoils + row.deadCoils + row.solidCoils) * wireMm;
}

double CompressionSpring::pitchMm() const noexcept {
    if (!(activeCoils > 0.0)) return 0.0;
    // What is left of the free length once the dead coils have taken theirs,
    // shared out over the coils that actually rise.
    const EndsRow& row = RowOf(ends);
    return (freeLengthMm - (row.deadCoils + row.solidCoils) * wireMm) / activeCoils;
}

double CompressionSpring::maxDeflectionMm() const noexcept {
    return freeLengthMm - solidLengthMm();
}

double CompressionSpring::rateNPerMm() const noexcept {
    if (!(activeCoils > 0.0) || !(meanDiameterMm > 0.0)) return 0.0;
    const double d = wireMm;
    return kShearModulusMPa * d * d * d * d /
           (8.0 * meanDiameterMm * meanDiameterMm * meanDiameterMm * activeCoils);
}

double CompressionSpring::forceAtLengthN(double lengthMm) const noexcept {
    // NEGATIVE MEANS NOTHING HERE. A compression spring longer than its free
    // length is simply not touching anything, and reporting a pulling force
    // would describe a spring welded to both faces.
    const double squash = freeLengthMm - lengthMm;
    if (!(squash > 0.0)) return 0.0;
    return rateNPerMm() * squash;
}

double CompressionSpring::shearStressAtLengthMPa(double lengthMm) const noexcept {
    const double force = forceAtLengthN(lengthMm);
    if (!(force > 0.0) || !(wireMm > 0.0)) return 0.0;
    const double index = springIndex();
    if (!(index > 1.0)) return 0.0;
    // WAHL'S CORRECTION, and it is not optional. The inside of a coil sees
    // more stress than the plain torsion formula gives -- the wire is both
    // twisted and sheared there, and it is tighter round the inside than the
    // outside. The correction is 1.2 at an index of 8 and 1.4 at 4, and a
    // spring designed without it is designed twenty to forty percent light
    // exactly where it breaks.
    const double wahl = (4.0 * index - 1.0) / (4.0 * index - 4.0) + 0.615 / index;
    return wahl * 8.0 * force * meanDiameterMm / (kPi * wireMm * wireMm * wireMm);
}

double CompressionSpring::slendernessRatio() const noexcept {
    return meanDiameterMm > 0.0 ? freeLengthMm / meanDiameterMm : 0.0;
}

std::string CompressionSpring::designation() const {
    std::string out = "d" + ShortNumber(wireMm) + " D" + ShortNumber(meanDiameterMm) + " n" +
                      ShortNumber(activeCoils) + " L" + ShortNumber(freeLengthMm);
    // Appended only when it is not the ordinary closed-and-ground, so two of
    // the common spring are one line on a parts list.
    if (ends != SpringEnds::ClosedGround) out += std::string(" e") + RowOf(ends).code;
    return out;
}

std::optional<CompressionSpring> ParseSpringDesignation(std::string_view designation) {
    CompressionSpring spring;
    std::size_t at = 0;
    SkipSpaces(designation, at);
    if (!Expect(designation, at, 'd')) return std::nullopt;
    if (!ReadNumber(designation, at, spring.wireMm)) return std::nullopt;
    SkipSpaces(designation, at);
    if (!Expect(designation, at, 'D')) return std::nullopt;
    if (!ReadNumber(designation, at, spring.meanDiameterMm)) return std::nullopt;
    SkipSpaces(designation, at);
    if (!Expect(designation, at, 'n')) return std::nullopt;
    if (!ReadNumber(designation, at, spring.activeCoils)) return std::nullopt;
    SkipSpaces(designation, at);
    if (!Expect(designation, at, 'L')) return std::nullopt;
    if (!ReadNumber(designation, at, spring.freeLengthMm)) return std::nullopt;
    SkipSpaces(designation, at);
    if (at != designation.size()) {
        if (!Expect(designation, at, 'e')) return std::nullopt;
        if (at >= designation.size()) return std::nullopt;
        const char code = designation[at++];
        bool known = false;
        for (const EndsRow& row : kEnds) {
            if (row.code != code) continue;
            spring.ends = row.ends;
            known = true;
        }
        if (!known) return std::nullopt;
        SkipSpaces(designation, at);
        if (at != designation.size()) return std::nullopt;
    }
    return spring;
}

std::optional<CompressionSpring> LookUpSpring(std::string_view designation) {
    const std::optional<CompressionSpring> read = ParseSpringDesignation(designation);
    if (!read || !WhySpringRefused(*read).empty()) return std::nullopt;
    return read;
}

std::string WhySpringRefused(const CompressionSpring& spring) {
    const std::vector<double>& wires = StandardWireDiameters();
    if (std::none_of(wires.begin(), wires.end(),
                     [&](double d) { return Same(d, spring.wireMm); }))
        return ShortNumber(spring.wireMm) +
               " mm is not a wire this program winds -- a wire diameter is what comes off "
               "the reel, not a number to choose";
    if (!(spring.meanDiameterMm > 0.0) || !(spring.freeLengthMm > 0.0))
        return "a spring needs a diameter and a length, and one of these is not a size";
    // FEWER THAN TWO ACTIVE COILS IS NOT A SPRING. One coil is a washer that
    // does not sit flat, and the rate formula -- which divides by the coil
    // count -- goes to infinity as it approaches zero.
    if (!(spring.activeCoils >= 2.0))
        return ShortNumber(spring.activeCoils) +
               " active coils is not a spring: below two the ends are the whole of it, and "
               "there is nothing left to deflect";

    // THE INDEX, which is the one that matters.
    const double index = spring.springIndex();
    if (index < kLeastIndex)
        return "an index of " + ShortNumber(index) + " (" + ShortNumber(spring.meanDiameterMm) +
               " over " + ShortNumber(spring.wireMm) +
               ") is tighter than wire will wind: the coil is a sharper bend than drawn "
               "spring steel takes, and it cracks on the mandrel. " +
               ShortNumber(kLeastIndex) + " is as tight as it goes";
    if (index > kMostIndex)
        return "an index of " + ShortNumber(index) +
               " is too slack to handle: springs that loose tangle with each other in a bin "
               "and wander under load. " + ShortNumber(kMostIndex) + " is as loose as it goes";

    // AND IT HAS TO HAVE SOMEWHERE TO GO. A free length at or below the solid
    // height is a spring already shut: it has a rate, a mass and a picture,
    // and it cannot be compressed by a single millimetre.
    if (!(spring.maxDeflectionMm() > 0.0))
        return "this spring is already shut: " + ShortNumber(spring.freeLengthMm) +
               " mm free against a solid height of " + ShortNumber(spring.solidLengthMm()) +
               " mm, so there is nothing to compress";
    return {};
}

std::string WhySpringMayBuckle(const CompressionSpring& spring) {
    if (!WhySpringRefused(spring).empty()) return {};
    const double slenderness = spring.slendernessRatio();
    if (slenderness <= kSlendernessToWatch) return {};
    return "this spring is " + ShortNumber(slenderness) +
           " times as long as it is wide, and above " + ShortNumber(kSlendernessToWatch) +
           " an unguided one folds sideways instead of shortening -- it needs a bore round "
           "it or a rod through it";
}

bool IsCompressionSpringPath(std::string_view path) noexcept {
    return path.size() > kCompressionSpringScheme.size() &&
           path.substr(0, kCompressionSpringScheme.size()) == kCompressionSpringScheme;
}

std::string CompressionSpringPath(const CompressionSpring& spring) {
    return std::string(kCompressionSpringScheme) + spring.designation();
}

std::optional<CompressionSpring> CompressionSpringOfPath(std::string_view path) {
    if (!IsCompressionSpringPath(path)) return std::nullopt;
    return LookUpSpring(path.substr(kCompressionSpringScheme.size()));
}

std::unique_ptr<PartDocument> BuildCompressionSpring(const CompressionSpring& spring,
                                                     IGeometryKernel& kernel) {
    if (!WhySpringRefused(spring).empty()) return nullptr;

    auto part = std::make_unique<PartDocument>(spring.designation());
    part->setGeometryKernel(&kernel);
    Body& body = part->addBody(spring.designation());

    // ONE HELIX AT A CONSTANT PITCH, and what that costs is stated rather than
    // hidden.
    //
    // A real closed-and-ground spring has two DEAD coils wound flat against
    // their neighbours and then ground off square. What is modelled here rises
    // evenly the whole way, so the envelope is right -- the same wire, the same
    // outside diameter, the same total coils, the same free length, which is
    // what a clearance check and a mass need -- and the ends are not. The wire
    // length differs by well under a percent, because a coil's length is
    // dominated by its circumference and hardly at all by its rise.
    //
    // The numbers that DEPEND on the end style -- solid height, dead coils,
    // rate -- come from the style regardless, because they are calculated and
    // not measured off this shape.
    Parameter& wire = part->addParameter("Wire", spring.wireMm, UnitType::Millimeter);
    Parameter& mean = part->addParameter("Mean diameter", spring.meanDiameterMm,
                                         UnitType::Millimeter);
    const double turns = spring.totalCoilsCount();
    Parameter& pitch = part->addParameter(
        "Pitch", turns > 0.0 ? (spring.freeLengthMm - spring.wireMm) / turns : 0.0,
        UnitType::Millimeter);
    // UNITLESS, because a turn count is a count. There is no Count in
    // UnitType and adding one for a single caller would be a unit nobody else
    // means; Unitless is what this project already uses for a number that is
    // a number, and M18's lesson was about a LENGTH printed as an area, not
    // about counts.
    Parameter& count = part->addParameter("Turns", turns, UnitType::Unitless);
    part->addHelixFeature(body, "Coil", wire.id(), mean.id(), pitch.id(), count.id());
    return part;
}

} // namespace paramcad
