#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace paramcad {

class PartDocument;

// M45 -- THE STANDARD PARTS LIBRARY.
//
// After M39 a hole knows what M8 means. There was still no M8 SCREW to put in
// it, and that is the gap this closes: an assembly could describe a bolted
// joint and could not contain a bolt.
//
// IT IS A DATA PROBLEM, NOT A GEOMETRY ONE. The shapes are trivial -- a
// revolved profile and a hexagonal prism. What makes a screw an ISO 4762 M8x30
// rather than a cylinder is a row of published numbers, and those are copied
// down here for the same reason M37's fits and M39's threads are: a formula
// that is nearly right is worse than no formula, because the sizes it gets
// wrong are the ones that get used.
//
// A PART IS DERIVED FROM ITS DESIGNATION AND NEVER STORED. An instance says
// "ISO 4762 M8x30" and the solid is built again on every rebuild, exactly as a
// part file is re-read and re-built rather than trusted as saved
// (ADR-M22-003). There is no file on disk and no copy to go stale.
//
// WHAT IS NOT MODELLED is the thread and the hex socket -- for M39's reason,
// which has not changed: modelled helices make files enormous, sections
// unreadable and booleans fragile, and the drawing says ISO 4762 M8x30 either
// way. The solid is the blank.

enum class FastenerKind {
    SocketHeadCapScrew, // ISO 4762
    HexNut,             // ISO 4032, style 1
    PlainWasher,        // ISO 7089, 200 HV
};
std::string_view toString(FastenerKind kind) noexcept;
bool ParseFastenerKind(std::string_view text, FastenerKind& into) noexcept;
// The standard's number, which is what a parts list prints.
std::string_view StandardNumberOf(FastenerKind kind) noexcept;

// ONE CATALOGUE ITEM, as published numbers.
//
// Zero in a field the kind does not use -- a washer has no head height -- and
// the builder never reads a field its kind does not fill.
struct FastenerSpec {
    FastenerKind kind = FastenerKind::SocketHeadCapScrew;
    double threadMm = 8.0; // the M size: 8 for M8
    // Screws only. The length UNDER THE HEAD, which is what the designation
    // means and what a joint is designed around.
    double lengthMm = 0.0;

    double headDiameterMm = 0.0;   // ISO 4762 dk
    double headHeightMm = 0.0;     // ISO 4762 k
    double acrossFlatsMm = 0.0;    // ISO 4032 s
    double thicknessMm = 0.0;      // ISO 4032 m, or ISO 7089 h
    double innerDiameterMm = 0.0;  // ISO 7089 d1
    double outerDiameterMm = 0.0;  // ISO 7089 d2

    // "ISO 4762 M8x30", "ISO 4032 M8". What the drawing and the parts list say.
    std::string designation() const;
};

// THE LENGTHS A CAP SCREW IS MADE IN (ISO 888).
//
// AND THIS ONE IS A GATE, unlike M41's roughness series -- which is the
// opposite call, made deliberately. A roughness of 1.2 is an instruction a
// designer chooses and a shop can meet. A 33 mm cap screw is not a number: it
// is a part that has to exist, and a library that invented one would put a
// line on a parts list nobody can order.
const std::vector<double>& StandardScrewLengths();

// The catalogue item a designation names, or nothing.
//
// "ISO 4762 M8x30", "ISO 4032 M8", "ISO 7089 M8". A size or a length the
// catalogue does not hold is refused rather than interpolated.
std::optional<FastenerSpec> LookUpFastener(std::string_view designation);
std::optional<FastenerSpec> LookUpFastener(FastenerKind kind, double threadMm,
                                           double lengthMm = 0.0);

// --- THE SOURCE PATH SCHEME -------------------------------------------------
//
// A library part reaches the rest of the program as a SOURCE PATH, the same
// way a file does: an instance names it, a drawing view projects it, a parts
// list counts it. Nothing downstream had to learn a new kind of thing -- which
// is why the scheme exists at all.
//
//   std:ISO 4762 M8x30
//
// The prefix is what tells the resolver this is a catalogue item and not a
// file it should try to open.
constexpr std::string_view kStandardPartScheme = "std:";
bool IsStandardPartPath(std::string_view path) noexcept;
std::string StandardPartPath(const FastenerSpec& spec);
// The item a std: path names, or nothing when the path is not one or names
// something the catalogue does not have.
std::optional<FastenerSpec> FastenerOfPath(std::string_view path);

// THE PART, BUILT FROM THE NUMBERS. A real PartDocument with a real feature
// tree, so everything that reads a part -- the recompute engine, mass
// properties, the projector, the parts list -- treats it as one.
std::unique_ptr<PartDocument> BuildStandardPart(const FastenerSpec& spec);

} // namespace paramcad
