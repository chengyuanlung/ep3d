#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Drawing/DrawingDimension.h"
#include "Core/Geometry/MathTypes.h"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace paramcad {

// M41 -- THE SYMBOLS: SURFACE FINISH, GEOMETRIC TOLERANCE, DATUM.
//
// Three things a reader thinks of as unrelated, built as ONE, because
// mechanically they are one: a symbol, a leader to the thing it is about, and
// a set of fields. Written as three features they would be three copies of the
// leader, three ideas of what "the thing it points at moved" means, and three
// answers to "is this annotation still attached" -- which is the shape of
// defect this project keeps closing.
//
// AND THE LEADER IS A DimensionAnchor, the one a dimension already uses. Not a
// second kind of attachment: the same three ways of naming a point (a place on
// the paper, a snap point on an entity, a point inside a view), the same
// reattachment tolerance, and the same loud failure when the thing it named
// has gone. A surface finish symbol that quietly stays put while its face
// moves is a drawing that says the wrong surface has to be ground.

// --- SURFACE FINISH (ISO 1302) ---------------------------------------------
//
// THE THREE SYMBOLS ARE THREE DIFFERENT INSTRUCTIONS, and this is the field
// that must not have a silent default:
//
//   Basic       the surface is specified, and how it is made is not.
//   Machined    material MUST be removed -- a cast face is not acceptable.
//   AsCast      material must NOT be removed -- the process face is the face.
//
// Getting Machined where AsCast was meant scraps a casting, and both draw a
// tick with a number next to it.
enum class SurfaceSymbol { Basic, Machined, AsCast };
std::string_view toString(SurfaceSymbol symbol) noexcept;
bool ParseSurfaceSymbol(std::string_view text, SurfaceSymbol& into) noexcept;

// WHICH WAY THE MARKS RUN (ISO 1302 lay). It matters on a sealing face and on
// anything that slides: marks across a seal groove leak.
enum class SurfaceLay { Unspecified, Parallel, Perpendicular, Crossed, Multi,
                        Circular, Radial, Particulate };
std::string_view toString(SurfaceLay lay) noexcept;
bool ParseSurfaceLay(std::string_view text, SurfaceLay& into) noexcept;
// The character a drawing writes for it: =, ⊥, X, M, C, R, P.
std::string SymbolOfLay(SurfaceLay lay);

struct SurfaceFinishSpec {
    SurfaceSymbol symbol = SurfaceSymbol::Machined;
    // Ra in micrometres. THE UPPER LIMIT, which is what a bare number means.
    double raMicrometres = 3.2;
    // An optional lower limit: "Ra 0.8 / Ra 3.2" is a band, not a maximum.
    // Zero means there is no lower limit, which is the ordinary case.
    double raLowerMicrometres = 0.0;
    // Free text: "milled", "ground", "as cast". Not an enum, because the list
    // of processes is a shop's, not a standard's.
    std::string process;
    SurfaceLay lay = SurfaceLay::Unspecified;
    // Millimetres left for machining, on a casting or forging drawing. Zero
    // means none is stated.
    double machiningAllowanceMm = 0.0;
    // ALL AROUND: the symbol applies to every surface of the outline it points
    // at, not just the edge it touches. A circle on the leader elbow.
    bool allAround = false;
};

// WHY THERE IS NO TABLE OF LEGAL Ra VALUES HERE.
//
// M37's fits and M39's threads refuse anything not in their tables, because an
// interpolated fit or an invented tap drill is a standard this program made
// up. A roughness is NOT that: Ra is a free number the designer chooses, and
// 1.2 is as real an instruction as 1.6. The R10 preferred series below is
// offered to a user interface as a list to pick from -- it is not a gate.
//
// Saying this out loud because the opposite mistake is easy to make twice: the
// last two milestones both ended with "refuse what is not in the table", and
// applying that rule here would refuse perfectly good drawings.
const std::vector<double>& PreferredRaSeries();

// What the drawing writes: "Ra 3.2", "Ra 0.8-3.2", with the process and lay
// where ISO 1302 puts them. Derived, never stored.
std::string SurfaceFinishText(const SurfaceFinishSpec& spec);
// Why this specification cannot be drawn, or empty when it can.
std::string WhySurfaceFinishRefused(const SurfaceFinishSpec& spec);

// --- GEOMETRIC TOLERANCE (ISO 1101) ----------------------------------------
//
// The fourteen characteristics, in the standard's own order and grouping.
enum class GeometricCharacteristic {
    Straightness, Flatness, Roundness, Cylindricity,      // form -- no datum
    LineProfile, SurfaceProfile,                          // profile -- either
    Parallelism, Perpendicularity, Angularity,            // orientation -- datum
    Position, Concentricity, Symmetry,                    // location -- datum
    CircularRunout, TotalRunout                           // runout -- datum
};
std::string_view toString(GeometricCharacteristic characteristic) noexcept;
bool ParseGeometricCharacteristic(std::string_view text,
                                  GeometricCharacteristic& into) noexcept;
// The ISO 1101 glyph, as UTF-8.
std::string SymbolOfCharacteristic(GeometricCharacteristic characteristic);

// WHETHER THIS CHARACTERISTIC IS ABOUT A DATUM.
//
// THE RULE THAT FAILS SILENTLY. A flatness frame with "A" in it is not a
// tighter specification, it is a meaningless one -- flatness is a property of
// one surface by itself. A position frame WITHOUT a datum is worse: it is
// unmeasurable, and it looks exactly like a complete frame.
//
//   Never     form: straightness, flatness, roundness, cylindricity
//   Always    orientation, location, runout
//   Either    the two profile characteristics, which are both
enum class DatumNeed { Never, Either, Always };
DatumNeed DatumNeedOf(GeometricCharacteristic characteristic) noexcept;

// Maximum and least material condition. A positional tolerance at MMC gets a
// bonus as the feature departs from its maximum material size, which is a real
// and commonly wanted thing -- and a frame that dropped the modifier would
// silently specify a tighter part than the designer allowed.
enum class MaterialCondition { RegardlessOfFeatureSize, Maximum, Least };
std::string_view toString(MaterialCondition condition) noexcept;
bool ParseMaterialCondition(std::string_view text, MaterialCondition& into) noexcept;
std::string SymbolOfCondition(MaterialCondition condition);

// ONE REFERENCED DATUM. It names the datum OBJECT, not a letter.
//
// The letter is derived from the order the datums were placed, exactly as a
// section's letter is (M38): the symbol on the face and every frame that
// refers to it have to carry the SAME letter, and two places that each store
// one is the classic pair that drifts.
struct DatumReference {
    ObjectId datumId = kInvalidObjectId;
    MaterialCondition condition = MaterialCondition::RegardlessOfFeatureSize;
};

struct FeatureControlFrameSpec {
    GeometricCharacteristic characteristic = GeometricCharacteristic::Position;
    double toleranceMm = 0.1;
    // A DIAMETRIC ZONE, written Ø before the value.
    //
    // "0.2" is a square zone 0.2 across; "Ø0.2" is a cylinder 0.2 across, and
    // the corner of the square is 1.4 times further out than the circle. A
    // frame that lost the symbol specifies a part 40% looser at the corners
    // and reads as an ordinary frame.
    bool diametricZone = true;
    MaterialCondition condition = MaterialCondition::RegardlessOfFeatureSize;
    // Primary, secondary, tertiary -- IN THAT ORDER, because the order IS the
    // specification: |A|B|C| and |B|A|C| constrain different things.
    std::vector<DatumReference> datums;
};

// WHY THIS FRAME CANNOT BE DRAWN, or empty when it can.
//
// Every rule here fails the same way: the frame draws perfectly and specifies
// something nobody can measure, or something looser than the designer meant.
// None of them is a crash and none of them is visible on the paper.
std::string WhyFrameRefused(const FeatureControlFrameSpec& spec);

// WHAT THE FRAME SAYS, given the letters its datums currently carry.
//
// The letters are passed IN rather than looked up here, because they are
// derived from the order the datums were placed -- the document owns that
// order. What this must not do is take a letter from the frame itself: the
// symbol on the face and the frame that refers to it are the classic pair
// that drifts apart, and M38's section letters are the same lesson.
std::string FrameText(const FeatureControlFrameSpec& spec,
                      const std::vector<std::string>& datumLetters);

// --- DATUM FEATURE ----------------------------------------------------------
//
// The letter is NOT here. It is derived from document order (see
// DrawingDocument::datumLetterOf), so the symbol on the paper and every frame
// that refers to this datum cannot end up carrying different letters.
struct DatumFeatureSpec {
    // Free text a drafter may add beside the symbol, e.g. "2 places". Usually
    // empty.
    std::string note;
};

// --- ITEM BALLOON (M42) -----------------------------------------------------
//
// THE NUMBER IS NOT HERE, and that is the entire design.
//
// A balloon exists to tie a part on the picture to a row in the parts list. A
// balloon carrying its own number is a second copy of that row's item number:
// somebody inserts a part, the list renumbers, and the balloons go on pointing
// at rows that have moved. Every number on the sheet is still a number the
// list contains, which is what makes it unreadable rather than obviously
// wrong -- the reader orders the wrong part and nothing looked broken.
//
// So a balloon stores WHICH ROW, as the same sentence the parts list groups
// by: the file the part came from and the body inside it. The number is asked
// for at every repaint (ADR-M10-002, composed never stored).
//
// AND IT NAMES ITS TABLE. A sheet can carry two lists -- this assembly's parts
// and every part however deep -- and the same bolt is item 4 in one and item
// 11 in the other. A balloon that only said "the parts list" would be right on
// whichever one was drawn first.
struct BalloonSpec {
    ObjectId tableId = kInvalidObjectId;
    std::string sourceFile;
    std::string partName;
};

using AnnotationBody = std::variant<SurfaceFinishSpec, FeatureControlFrameSpec,
                                    DatumFeatureSpec, BalloonSpec>;

// THE ANNOTATION ITSELF: a body, a leader, and where the symbol sits.
class Annotation {
public:
    Annotation(AnnotationBody body, DimensionAnchor anchor, Vec2 positionMm, ObjectId layerId);
    Annotation(ObjectId id, AnnotationBody body, DimensionAnchor anchor, Vec2 positionMm,
               ObjectId layerId);

    ObjectId id() const noexcept { return id_; }
    static std::string_view typeName() noexcept { return "Annotation"; }

    const AnnotationBody& body() const noexcept { return body_; }
    void setBody(AnnotationBody body) { body_ = std::move(body); }

    // WHAT IT IS ABOUT. The same anchor a dimension uses, so an annotation
    // dangles the same way and for the same reasons.
    const DimensionAnchor& anchor() const noexcept { return anchor_; }
    void setAnchor(DimensionAnchor anchor) { anchor_ = anchor; }

    // Where the symbol sits, in sheet millimetres. Dragged, not derived --
    // where an annotation is readable is a judgement no rule makes well.
    Vec2 positionMm() const noexcept { return positionMm_; }
    void setPositionMm(Vec2 at) noexcept { positionMm_ = at; }

    ObjectId layerId() const noexcept { return layerId_; }
    void setLayerId(ObjectId layerId) noexcept { layerId_ = layerId; }

    // WHICH PAGE THIS SITS ON (M44). kInvalidObjectId means the drawing's
    // first page, which is what every object made before there was more than
    // one page belongs to.
    ObjectId sheetId() const noexcept { return sheetId_; }
    void setSheetId(ObjectId sheetId) noexcept { sheetId_ = sheetId; }


    bool isDatum() const noexcept {
        return std::holds_alternative<DatumFeatureSpec>(body_);
    }
    bool isFrame() const noexcept {
        return std::holds_alternative<FeatureControlFrameSpec>(body_);
    }
    bool isSurfaceFinish() const noexcept {
        return std::holds_alternative<SurfaceFinishSpec>(body_);
    }
    bool isBalloon() const noexcept { return std::holds_alternative<BalloonSpec>(body_); }

private:
    ObjectId id_;
    AnnotationBody body_;
    DimensionAnchor anchor_;
    Vec2 positionMm_{};
    // M44. Set when the object is added, from whichever page was current.
    ObjectId sheetId_ = kInvalidObjectId;
    ObjectId layerId_ = kInvalidObjectId;
};

} // namespace paramcad
