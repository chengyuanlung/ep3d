#include "Core/Library/StandardParts.h"
#include "Core/Text/NumberText.h"

#include "Core/Document/PartDocument.h"
#include "Core/Feature/HoleStandards.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/RevolveFeature.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace paramcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ONE ROW PER SIZE, so a size cannot be in the screw table and missing from
// the nut one. The same shape M39's thread table has, and for the same reason.
//
// A zero means the standard does not publish that number for this size, and it
// is refused rather than filled in.
struct Row {
    double threadMm;
    double headDiameterMm;  // ISO 4762 dk, max
    double headHeightMm;    // ISO 4762 k, max
    double acrossFlatsMm;   // ISO 4032 s
    double nutThicknessMm;  // ISO 4032 m, max
    double washerOuterMm;   // ISO 7089 d2
    double washerThickMm;   // ISO 7089 h
};

constexpr Row kCatalogue[] = {
    {3.0, 5.5, 3.0, 5.5, 2.4, 7.0, 0.5},
    {4.0, 7.0, 4.0, 7.0, 3.2, 9.0, 0.8},
    {5.0, 8.5, 5.0, 8.0, 4.7, 10.0, 1.0},
    {6.0, 10.0, 6.0, 10.0, 5.2, 12.0, 1.6},
    {8.0, 13.0, 8.0, 13.0, 6.8, 16.0, 1.6},
    {10.0, 16.0, 10.0, 16.0, 8.4, 20.0, 2.0},
    {12.0, 18.0, 12.0, 18.0, 10.8, 24.0, 2.5},
};

const Row* FindSize(double threadMm) noexcept {
    for (const Row& row : kCatalogue)
        if (std::fabs(row.threadMm - threadMm) < 1e-9) return &row;
    return nullptr;
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

void SkipSpace(std::string_view text, std::size_t& at) {
    while (at < text.size() && text[at] == ' ') ++at;
}

} // namespace

std::string_view toString(FastenerKind kind) noexcept {
    switch (kind) {
        case FastenerKind::SocketHeadCapScrew: return "socket-head-cap-screw";
        case FastenerKind::HexNut: return "hex-nut";
        case FastenerKind::PlainWasher: return "plain-washer";
    }
    return "socket-head-cap-screw";
}

bool ParseFastenerKind(std::string_view text, FastenerKind& into) noexcept {
    for (const FastenerKind kind : {FastenerKind::SocketHeadCapScrew, FastenerKind::HexNut,
                                    FastenerKind::PlainWasher})
        if (text == toString(kind)) {
            into = kind;
            return true;
        }
    return false;
}

std::string_view StandardNumberOf(FastenerKind kind) noexcept {
    switch (kind) {
        case FastenerKind::SocketHeadCapScrew: return "ISO 4762";
        case FastenerKind::HexNut: return "ISO 4032";
        case FastenerKind::PlainWasher: return "ISO 7089";
    }
    return "ISO 4762";
}

std::string FastenerSpec::designation() const {
    std::string out = std::string(StandardNumberOf(kind)) + " M" + ShortNumber(threadMm);
    // ONLY A SCREW CARRIES A LENGTH. A nut's designation with an "x30" on it
    // names nothing, and a parts list is where that would be noticed last.
    if (kind == FastenerKind::SocketHeadCapScrew) out += "x" + ShortNumber(lengthMm);
    return out;
}

const std::vector<double>& StandardScrewLengths() {
    // ISO 888's series, over the range these sizes are made in.
    static const std::vector<double> lengths{5.0,  6.0,  8.0,  10.0, 12.0, 16.0, 20.0,
                                             25.0, 30.0, 35.0, 40.0, 45.0, 50.0, 55.0,
                                             60.0, 65.0, 70.0, 80.0, 90.0, 100.0};
    return lengths;
}

std::optional<FastenerSpec> LookUpFastener(FastenerKind kind, double threadMm,
                                           double lengthMm) {
    const Row* row = FindSize(threadMm);
    if (row == nullptr) return std::nullopt;

    FastenerSpec spec;
    spec.kind = kind;
    spec.threadMm = row->threadMm;
    switch (kind) {
        case FastenerKind::SocketHeadCapScrew: {
            bool made = false;
            for (const double each : StandardScrewLengths())
                if (std::fabs(each - lengthMm) < 1e-9) made = true;
            // A LENGTH NOBODY MAKES IS NOT A PART. This is the opposite call
            // from M41's roughness series, and deliberately: a roughness is a
            // number a shop can meet, while a 33 mm cap screw is a line on a
            // parts list nobody can order.
            if (!made) return std::nullopt;
            spec.lengthMm = lengthMm;
            spec.headDiameterMm = row->headDiameterMm;
            spec.headHeightMm = row->headHeightMm;
            break;
        }
        case FastenerKind::HexNut:
            spec.acrossFlatsMm = row->acrossFlatsMm;
            spec.thicknessMm = row->nutThicknessMm;
            break;
        case FastenerKind::PlainWasher:
            // THE BORE IS THE CLOSE CLEARANCE HOLE, and it is not copied here:
            // ISO 7089's d1 IS ISO 273's close series, and M39 already holds
            // that table. Two copies of one column is the thing this project
            // keeps taking out -- and a washer that stopped matching the hole
            // it goes on is a defect nobody would look for.
            if (const std::optional<double> bore =
                    ClearanceHoleMm(row->threadMm, ClearanceFit::Close))
                spec.innerDiameterMm = *bore;
            else
                return std::nullopt;
            spec.outerDiameterMm = row->washerOuterMm;
            spec.thicknessMm = row->washerThickMm;
            break;
    }
    return spec;
}

std::optional<FastenerSpec> LookUpFastener(std::string_view designation) {
    std::size_t at = 0;
    SkipSpace(designation, at);

    FastenerKind kind = FastenerKind::SocketHeadCapScrew;
    bool known = false;
    for (const FastenerKind each : {FastenerKind::SocketHeadCapScrew, FastenerKind::HexNut,
                                    FastenerKind::PlainWasher}) {
        const std::string_view number = StandardNumberOf(each);
        if (designation.substr(at, number.size()) == number) {
            kind = each;
            at += number.size();
            known = true;
            break;
        }
    }
    // A STANDARD THIS BUILD DOES NOT HOLD IS REFUSED. Guessing the kind from
    // the size would put a nut where a screw was asked for.
    if (!known) return std::nullopt;

    SkipSpace(designation, at);
    if (at >= designation.size()) return std::nullopt;
    if (designation[at] != 'M' && designation[at] != 'm') return std::nullopt;
    ++at;

    double thread = 0.0;
    if (!ReadNumber(designation, at, thread)) return std::nullopt;

    double length = 0.0;
    if (at < designation.size()) {
        if (designation[at] != 'x' && designation[at] != 'X') return std::nullopt;
        ++at;
        if (!ReadNumber(designation, at, length)) return std::nullopt;
    }
    SkipSpace(designation, at);
    if (at != designation.size()) return std::nullopt;

    // A LENGTH ON SOMETHING THAT HAS NONE, or none on something that needs
    // one, names nothing -- and both would otherwise fall through into a part
    // with a plausible size.
    if (kind == FastenerKind::SocketHeadCapScrew && length <= 0.0) return std::nullopt;
    if (kind != FastenerKind::SocketHeadCapScrew && length > 0.0) return std::nullopt;
    return LookUpFastener(kind, thread, length);
}

bool IsStandardPartPath(std::string_view path) noexcept {
    return path.substr(0, kStandardPartScheme.size()) == kStandardPartScheme;
}

std::string StandardPartPath(const FastenerSpec& spec) {
    return std::string(kStandardPartScheme) + spec.designation();
}

std::optional<FastenerSpec> FastenerOfPath(std::string_view path) {
    if (!IsStandardPartPath(path)) return std::nullopt;
    return LookUpFastener(path.substr(kStandardPartScheme.size()));
}

// --- the solids -------------------------------------------------------------

std::unique_ptr<PartDocument> BuildStandardPart(const FastenerSpec& spec) {
    auto part = std::make_unique<PartDocument>(spec.designation());
    Body& body = part->addBody(spec.designation());

    if (spec.kind == FastenerKind::HexNut) {
        // A HEXAGON ACROSS THE FLATS. `s` is the distance between opposite
        // flats, so the inradius is s/2 and the circumradius s/sqrt(3) -- the
        // one place that conversion is written, because getting it the other
        // way round makes a nut 15% too big and it still looks like a nut.
        const double circumradius = spec.acrossFlatsMm / std::sqrt(3.0);
        Sketch& outline = part->addSketch("Hex");
        Vec2 previous{circumradius, 0.0};
        for (int i = 1; i <= 6; ++i) {
            const double angle = kPi / 3.0 * static_cast<double>(i);
            const Vec2 next{circumradius * std::cos(angle), circumradius * std::sin(angle)};
            part->addSketchEntity(outline.id(), SketchLine{previous, next});
            previous = next;
        }
        Parameter& thick = part->addParameter("Thickness", spec.thicknessMm,
                                              UnitType::Millimeter);
        PadFeature& pad = part->addPadFeature(body, "Blank", outline.id(), thick.id());

        // THE HOLE IS THE TAP DRILL, which is what the blank has before it is
        // threaded -- and what M39 already knows the number for.
        const std::optional<MetricThread> thread = MetricCoarseThreadOfSize(spec.threadMm);
        if (thread) {
            Sketch& bore = part->addSketch("Bore");
            part->addSketchEntity(bore.id(), SketchCircle{Vec2{0.0, 0.0},
                                                          thread->tapDrillMm / 2.0});
            // ALL THE WAY THROUGH, AND A MILLIMETRE PAST. A pocket cuts by the
            // depth it is given -- zero is zero, not "through" (that reading
            // belongs to a Hole, M20) -- and a cut ending exactly ON the far
            // face leaves OCCT deciding whether the two are coincident, which
            // is the one case it is worst at.
            Parameter& deep = part->addParameter("Bore depth", spec.thicknessMm + 1.0,
                                                 UnitType::Millimeter);
            part->addPocketFeature(body, "Bore", pad.id(), bore.id(), deep.id());
        }
        return part;
    }

    // A SCREW AND A WASHER ARE BOTH REVOLVED, because both are round and a
    // revolve gives their volume exactly rather than to whatever a tessellated
    // pad would manage.
    Sketch& section = part->addSketch("Section");
    // The axis, as a line the revolve turns about.
    const SketchEntityId axis =
        part->addSketchEntity(section.id(), SketchLine{Vec2{0.0, -1000.0}, Vec2{0.0, 1000.0}});

    std::vector<Vec2> profile;
    if (spec.kind == FastenerKind::SocketHeadCapScrew) {
        // Head, then shank. The LENGTH is under the head, so the solid stands
        // k + L tall -- which is what a joint's stack-up has to add up with.
        profile = {Vec2{0.0, 0.0},
                   Vec2{spec.headDiameterMm / 2.0, 0.0},
                   Vec2{spec.headDiameterMm / 2.0, spec.headHeightMm},
                   Vec2{spec.threadMm / 2.0, spec.headHeightMm},
                   Vec2{spec.threadMm / 2.0, spec.headHeightMm + spec.lengthMm},
                   Vec2{0.0, spec.headHeightMm + spec.lengthMm}};
    } else {
        profile = {Vec2{spec.innerDiameterMm / 2.0, 0.0},
                   Vec2{spec.outerDiameterMm / 2.0, 0.0},
                   Vec2{spec.outerDiameterMm / 2.0, spec.thicknessMm},
                   Vec2{spec.innerDiameterMm / 2.0, spec.thicknessMm}};
    }
    for (std::size_t i = 0; i < profile.size(); ++i)
        part->addSketchEntity(section.id(),
                              SketchLine{profile[i], profile[(i + 1) % profile.size()]});

    // RADIANS, because that is the unit this program keeps angles in. A
    // revolve given 360 in a millimetre-shaped box would turn a fraction of a
    // degree and produce a sliver nobody could name.
    Parameter& turn = part->addParameter("Angle", 2.0 * kPi, UnitType::Radian);
    part->addRevolveFeature(body, "Body", section.id(), axis, turn.id());
    return part;
}

} // namespace paramcad
