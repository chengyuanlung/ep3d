#include "Cli/SketchScript.h"

#include "Core/Assembly/Mate.h"
#include "Core/Geometry/Transform.h"
#include "Core/Assembly/AssemblyDocument.h"
#include "Core/Serialization/AssemblyDocumentSerializer.h"
#include "Core/Feature/BooleanFeature.h"
#include "Core/Kernel/IGeometryKernel.h"
#include "Core/Feature/ISolidFeature.h"
#include "Core/Measure/SketchMeasure.h"

#include <iomanip>

#include "Core/Body/Body.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Core/Sketch/Sketch.h"
#include "Viewer/SketchCanvas.h"
#include "Viewer/SketchCommands.h"

#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace paramcad {

namespace {

// --- The three vocabularies -------------------------------------------------
//
// Tables rather than switches, because each one is ALSO the help text and the
// "did you mean" list in the error message. Three copies of a vocabulary --
// one to parse, one to print, one to explain -- is the shape of defect this
// project keeps paying for, so there is one of each.
//
// A test walks the SketchTool enum's full range and fails if a tool has no
// script name, which is what stops this table drifting behind the program.

struct ToolName {
    const char* text;
    SketchTool tool;
};

constexpr ToolName kTools[] = {
    {"select", SketchTool::Select},
    {"point", SketchTool::Point},
    {"line", SketchTool::Line},
    {"rect", SketchTool::Rectangle},
    {"centerrect", SketchTool::CenterRectangle},
    {"circle", SketchTool::Circle},
    {"circle3", SketchTool::ThreePointCircle},
    {"arc", SketchTool::Arc},
    {"arc3", SketchTool::ThreePointArc},
    {"tangentarc", SketchTool::TangentArc},
    {"ellipse", SketchTool::Ellipse},
    {"ellipsearc", SketchTool::EllipticalArc},
    {"spline", SketchTool::Spline},
    {"polygon", SketchTool::Polygon},
    {"slot", SketchTool::Slot},
};

struct KindName {
    const char* text;
    SketchEditKind kind;
};

constexpr KindName kConstraints[] = {
    {"coincident", SketchEditKind::AddCoincident},
    {"horizontal", SketchEditKind::AddHorizontal},
    {"vertical", SketchEditKind::AddVertical},
    {"fix", SketchEditKind::AddFix},
    {"parallel", SketchEditKind::AddParallel},
    {"perpendicular", SketchEditKind::AddPerpendicular},
    {"equal", SketchEditKind::AddEqual},
    {"concentric", SketchEditKind::AddConcentric},
    {"midpoint", SketchEditKind::AddMidpoint},
    {"pointonobject", SketchEditKind::AddPointOnObject},
    {"tangent", SketchEditKind::AddTangent},
    {"symmetric", SketchEditKind::AddSymmetric},
};

constexpr KindName kDimensions[] = {
    {"length", SketchEditKind::AddLength},
    {"distance", SketchEditKind::AddDistance},
    {"hdistance", SketchEditKind::AddHorizontalDistance},
    {"vdistance", SketchEditKind::AddVerticalDistance},
    {"pointlinedistance", SketchEditKind::AddPointLineDistance},
    {"radius", SketchEditKind::AddRadius},
    {"diameter", SketchEditKind::AddDiameter},
    {"angle", SketchEditKind::AddAngle},
    {"majoraxis", SketchEditKind::AddMajorAxis},
    {"minoraxis", SketchEditKind::AddMinorAxis},
    {"ellipseangle", SketchEditKind::AddEllipseRotation},
};

// A number a person reads. Six decimals is more than any sketch tolerance in
// this program, and the trailing zeros come off so that 40 reads as 40 rather
// than as 40.000000 -- a measurement that looks computed to six places invites
// a precision it does not have.
std::string FormatNumber(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    std::string text = out.str();
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    if (text == "-0") text = "0";
    return text;
}

std::string Join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        if (!out.empty()) out += ", ";
        out += item;
    }
    return out;
}

// The script's own name for a piece of geometry's KIND. Not SketchToolName:
// a rectangle tool makes lines, and what a reference has to name is the line.
const char* GeometryKindName(const SketchGeometry& geometry) {
    if (std::holds_alternative<SketchPoint>(geometry)) return "Point";
    if (std::holds_alternative<SketchLine>(geometry)) return "Line";
    if (std::holds_alternative<SketchCircle>(geometry)) return "Circle";
    if (std::holds_alternative<SketchArc>(geometry)) return "Arc";
    if (std::holds_alternative<SketchEllipse>(geometry)) return "Ellipse";
    if (std::holds_alternative<SketchEllipticalArc>(geometry)) return "EllipticalArc";
    return "Spline";
}

std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream stream(line);
    std::string token;
    while (stream >> token) {
        // A '#' STARTS A COMMENT wherever it appears, so a script can annotate
        // the end of a line rather than only whole lines.
        if (!token.empty() && token.front() == '#') break;
        tokens.push_back(token);
    }
    return tokens;
}

bool ParseNumber(const std::string& text, double* out) {
    if (text.empty()) return false;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == nullptr || *end != '\0') return false;
    if (!std::isfinite(value)) return false;
    *out = value;
    return true;
}

// --- The interpreter --------------------------------------------------------

class Runner {
public:
    explicit Runner(PartDocument& document) : document_(document) {}

    // The outcome is per CALL; everything else below survives between calls,
    // which is what makes a connection work one line at a time.
    ScriptOutcome run(const std::string& text) {
        ScriptOutcome outcome;
        outcome_ = &outcome;
        std::istringstream stream(text);
        std::string line;
        int number = 0;
        while (std::getline(stream, line)) {
            ++number;
            // A LONE CARRIAGE RETURN at the end of a line, which is what a
            // socket carries when the other end sends CRLF. Left on, it makes
            // every token's last character a stray \r and every name unknown.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // ...and a UTF-8 BYTE ORDER MARK at the front of the first one.
            //
            // Windows tooling adds one freely -- PowerShell's `|` does, and so
            // does Notepad's "UTF-8" -- and it is INVISIBLE. The first thing
            // ever piped into the socket came back as
            // `unknown command '<BOM>save'`, which names the problem in
            // characters the reader cannot see.
            if (number == 1 && line.size() >= 3 &&
                static_cast<unsigned char>(line[0]) == 0xEF &&
                static_cast<unsigned char>(line[1]) == 0xBB &&
                static_cast<unsigned char>(line[2]) == 0xBF)
                line.erase(0, 3);
            const std::vector<std::string> tokens = Tokenize(line);
            if (tokens.empty()) continue;
            line_ = number;
            // A DOCUMENT INVARIANT THROWS, and an uncaught throw here is not a
            // clean crash: on Windows the Debug CRT's abort handler waits for a
            // dialog nobody is there to click, so the whole script -- and a
            // socket connection driving it -- simply stops answering. That is
            // how this was found: an earlier version of the boolean verb passed
            // a base from another body, the facade refused correctly, and the
            // symptom was ep3d.exe sitting at 100% of nothing.
            //
            // NO SCRIPT COMMAND CAN REACH IT TODAY. Every verb picks its base
            // from the body it is dressing, so the facade's rules are satisfied
            // before they are checked -- which is why there is no test below
            // this line rather than a test that exercises nothing.
            //
            // It stays because the facade's contract SAYS it throws, and the
            // cost of being wrong about "nothing can reach it" is a connection
            // that stops answering rather than an error message.
            bool ok = false;
            try {
                ok = command(tokens);
            } catch (const std::exception& error) {
                ok = fail(std::string("the document refused that: ") + error.what());
            }
            if (!ok) {
                outcome_ = nullptr;
                return outcome;
            }
        }
        outcome.ok = true;
        outcome.message = "ran " + std::to_string(number) + " line(s)";
        outcome_ = nullptr;
        return outcome;
    }

private:
    PartDocument& document_;
    // THE ASSEMBLY THIS SCRIPT IS BUILDING, if it is building one (M23).
    // Owned here rather than passed in, because a script that never says
    // `assembly` must behave exactly as it did before this existed.
    std::unique_ptr<AssemblyDocument> assembly_;
    ScriptOutcome* outcome_{nullptr};
    int line_{0};

    SketchCanvasModel model_;
    ObjectId sketchId_{kInvalidObjectId};
    // Names are per SKETCH: a script that starts a second sketch gets Line1
    // again, which is what a reader expects and what makes each sketch's
    // section of a script readable on its own.
    std::map<std::string, SketchEntityId> names_;
    std::map<std::string, int> counts_;

    bool fail(const std::string& why) {
        outcome_->ok = false;
        outcome_->failedLine = line_;
        outcome_->message = why;
        return false;
    }

    void note(const std::string& what) {
        outcome_->log.push_back(ScriptLogEntry{line_, what});
    }

    const Sketch* sketch() const {
        return sketchId_ == kInvalidObjectId ? nullptr : document_.findSketch(sketchId_);
    }

    // Gives every entity a command created a name, and reports them.
    void nameCreated(const std::vector<SketchEntityId>& created, const std::string& what) {
        const Sketch* current = sketch();
        if (current == nullptr) return;
        std::string made;
        for (const SketchEntityId id : created) {
            const SketchEntity* entity = current->findEntity(id);
            if (entity == nullptr) continue;
            const std::string kind = GeometryKindName(entity->geometry);
            const std::string name = kind + std::to_string(++counts_[kind]);
            names_[name] = id;
            if (!made.empty()) made += " ";
            made += name;
        }
        note(what + (made.empty() ? std::string(" -> (nothing)") : " -> " + made));
    }

    // "Line1" or "Line1.start". Unknown names are an error naming what IS
    // known, because the alternative is a script author guessing.
    bool parseRef(const std::string& text, SketchElementRef* out) {
        std::string name = text;
        SketchSubElement part = SketchSubElement::Whole;
        int index = 0;
        const std::size_t dot = text.find('.');
        if (dot != std::string::npos) {
            name = text.substr(0, dot);
            const std::string suffix = text.substr(dot + 1);
            if (suffix == "start") part = SketchSubElement::StartPoint;
            else if (suffix == "end") part = SketchSubElement::EndPoint;
            else if (suffix == "center" || suffix == "centre")
                part = SketchSubElement::CenterPoint;
            else if (suffix == "whole") part = SketchSubElement::Whole;
            else if (suffix.size() > 1 && suffix[0] == 'h' &&
                       suffix.find_first_not_of("0123456789", 1) == std::string::npos) {
                // `Spline1.h3` -- the TIP of the handle on point 3.
                part = SketchSubElement::SplineHandle;
                index = std::atoi(suffix.c_str() + 1);
            } else if (suffix.size() > 1 && suffix[0] == 'p' &&
                     suffix.find_first_not_of("0123456789", 1) == std::string::npos) {
                // `Spline1.p3` -- ONE OF A SPLINE'S POINTS, counted from 0.
                part = SketchSubElement::SplinePoint;
                index = std::atoi(suffix.c_str() + 1);
            } else
                return fail("'" + suffix +
                            "' is not a sub-element; use start, end, center, pN for a "
                            "spline's Nth point, or hN for that point's handle");
        }
        const auto found = names_.find(name);
        if (found == names_.end()) {
            std::vector<std::string> known;
            for (const auto& pair : names_) known.push_back(pair.first);
            return fail("nothing here is called '" + name + "'" +
                        (known.empty() ? "" : "; this sketch has " + Join(known)));
        }
        if (part == SketchSubElement::SplineHandle) {
            const Sketch* current = sketch();
            const SketchEntity* entity =
                current == nullptr ? nullptr : current->findEntity(found->second);
            const auto* spline =
                entity == nullptr ? nullptr : std::get_if<SketchSpline>(&entity->geometry);
            if (spline == nullptr)
                return fail("'" + name + "' is not a spline, so it has no handles");
            if (spline->handles.find(index) == spline->handles.end())
                return fail("point " + std::to_string(index) + " of '" + name +
                            "' has no handle; give it one with `handle " + name + ".p" +
                            std::to_string(index) + " DU DV`");
            *out = SketchElementRef{found->second, SketchSubElement::SplineHandle, index};
            return true;
        }
        // THROUGH THE ONE SPELLING RULE, so `Spline1.p0` and `Spline1.start`
        // produce the SAME reference rather than two that compare unequal.
        if (part == SketchSubElement::SplinePoint) {
            const Sketch* current = sketch();
            const SketchEntity* entity =
                current == nullptr ? nullptr : current->findEntity(found->second);
            const auto* spline =
                entity == nullptr ? nullptr : std::get_if<SketchSpline>(&entity->geometry);
            if (spline == nullptr) return fail("'" + name + "' is not a spline, so it has no pN");
            if (index < 0 || index >= static_cast<int>(spline->points.size()))
                return fail("'" + name + "' has " + std::to_string(spline->points.size()) +
                            " points, numbered 0 to " +
                            std::to_string(spline->points.size() - 1));
            *out = SplineRefFor(found->second, *spline, index);
            return true;
        }
        *out = SketchElementRef{found->second, part};
        return true;
    }

    // A number, or the name of a Parameter that already exists. Returns the
    // TEXT to hand the dimension editor, which is where numbers and expressions
    // are already both understood -- so `dimension length L1 #Width/2` works
    // for free.
    bool parseValue(const std::string& text, std::string* out) {
        double ignored = 0.0;
        if (ParseNumber(text, &ignored)) {
            *out = text;
            return true;
        }
        *out = text; // an expression; CommitDimensionValue decides if it parses
        return true;
    }

    bool command(const std::vector<std::string>& tokens) {
        const std::string& verb = tokens[0];
        if (verb == "sketch") return doSketch(tokens);
        if (verb == "tool") return doTool(tokens);
        if (verb == "click") return doClick(tokens);
        if (verb == "finish") return doFinish();
        if (verb == "constrain") return doConstrain(tokens);
        if (verb == "dimension") return doDimension(tokens);
        if (verb == "pad") return doPad(tokens);
        if (verb == "sweep") return doSweep(tokens);
        if (verb == "loft") return doLoft(tokens);
        if (verb == "shell") return doShell(tokens);
        if (verb == "draft") return doDraft(tokens);
        if (verb == "hole") return doHole(tokens);
        if (verb == "union" || verb == "subtract" || verb == "intersect")
            return doBoolean(tokens);
        if (verb == "ring") return doRing(tokens);
        if (verb == "along") return doAlong(tokens);
        if (verb == "export") return doExport(tokens);
        if (verb == "import") return doImport(tokens);
        if (verb == "connector") return doConnector(tokens);
        if (verb == "assembly") return doAssembly(tokens);
        if (verb == "ground") return doGround(tokens);
        if (verb == "mate") return doMate(tokens);
        if (verb == "drive") return doDrive(tokens);
        if (verb == "insert") return doInsert(tokens);
        if (verb == "place") return doPlace(tokens);
        if (verb == "solve") return doSolve();
        if (verb == "save") return doSave(tokens);
        if (verb == "help") {
            // OVER A CONNECTION there is no --help to run, and the three
            // vocabularies are the same three lists the CLI's help prints --
            // asked for here rather than copied.
            note("tools: " + Join(ScriptToolNames()));
            note("constraints: " + Join(ScriptConstraintNames()));
            note("dimensions: " + Join(ScriptDimensionNames()));
            note("commands: sketch, tool, click, finish, constrain, dimension, pad, sweep, "
                 "loft, shell, draft, hole, union, subtract, intersect, ring, along, "
                 "export, import, connector, assembly, insert, place, ground, mate, "
                 "drive, solve, save, measure, "
                 "handle, echo, help");
            return true;
        }
        if (verb == "handle") {
            // `handle Spline1.p2 30 10`  -- give point 2 that tangent
            // `handle Spline1.p2 off`    -- take its handle away
            //
            // A handle is GEOMETRY, not a constraint: it changes what curve the
            // points describe. So this reshapes the entity in place, keeping its
            // id and every constraint already on it.
            if (tokens.size() != 3 && tokens.size() != 4)
                return fail("handle needs `handle REF DU DV` or `handle REF off`");
            SketchElementRef ref{};
            if (!parseRef(tokens[1], &ref)) return false;
            const Sketch* current = sketch();
            const SketchEntity* entity =
                current == nullptr ? nullptr : current->findEntity(ref.entityId);
            const auto* spline =
                entity == nullptr ? nullptr : std::get_if<SketchSpline>(&entity->geometry);
            if (spline == nullptr) return fail("'" + tokens[1] + "' is not a spline");
            // WHICH POINT, through the same rule that names one: `.start` is
            // point 0 and `.end` is the last, so `handle Spline1.start` works
            // and means what it looks like.
            int which = -1;
            const int count = static_cast<int>(spline->points.size());
            if (ref.subElement == SketchSubElement::StartPoint) which = 0;
            else if (ref.subElement == SketchSubElement::EndPoint) which = count - 1;
            else if (ref.subElement == SketchSubElement::SplinePoint ||
                     ref.subElement == SketchSubElement::SplineHandle) which = ref.index;
            if (which < 0 || which >= count)
                return fail("'" + tokens[1] + "' does not name one of that spline's points");

            SketchSpline reshaped = *spline;
            if (tokens.size() == 3) {
                if (tokens[2] != "off")
                    return fail("expected `off`, or a DU and a DV, after '" + tokens[1] + "'");
                if (reshaped.handles.erase(which) == 0)
                    return fail("point " + std::to_string(which) + " has no handle to remove");
            } else {
                double du = 0.0;
                double dv = 0.0;
                if (!ParseNumber(tokens[2], &du) || !ParseNumber(tokens[3], &dv)) return false;
                reshaped.handles[which] = Vec2{du, dv};
            }
            if (!document_.setSketchEntityGeometry(sketchId_, ref.entityId, std::move(reshaped)))
                return fail("the sketch refused that handle");
            note(tokens.size() == 3 ? "handle removed from point " + std::to_string(which)
                                    : "handle on point " + std::to_string(which) + " = " +
                                          tokens[2] + ", " + tokens[3]);
            return true;
        }
        if (verb == "measure") {
            // A QUERY, not an edit. Nothing is added to the document and
            // nothing is undoable -- which is exactly why it is here: the way
            // to check a script built what it meant to is to ask, and until
            // now the only way to ask was to open the file and read numbers
            // out of it by hand.
            // `measure` ON ITS OWN is the mass properties of the solid this
            // document's MassPropertiesNode is pointed at (roadmap 50.1).
            //
            // That is ONE feature, not the whole document: the node tracks a
            // single source, rewired to whichever solid feature was added last.
            // So a document with two bodies reports the second, and saying
            // "the document's volume" here would be a plausible number about
            // something else. Per-body and per-selection mass properties are
            // roadmap 50.1's `適用對象` line and are not done.
            //
            // Even so it is the number a script needs: it is the evidence that
            // a pad, a sweep or a loft actually produced a solid, and without
            // it the only evidence was that nothing complained.
            if (tokens.size() == 1 && assembly_ != nullptr) return measureAssembly();
            if (tokens.size() == 1) {
                const MassProperties& mass = document_.massProperties();
                if (!mass.valid)
                    return fail("no solid to measure yet: nothing has been built, or its "
                                "material has no density");
                note("  (the last solid feature built, not the whole document)");
                note("  volume = " + FormatNumber(mass.volumeMm3) + " mm^3");
                note("  mass = " + FormatNumber(mass.massKg) + " kg");
                note("  centre = " + FormatNumber(mass.centerOfMassMm.x) + ", " +
                     FormatNumber(mass.centerOfMassMm.y) + ", " +
                     FormatNumber(mass.centerOfMassMm.z) + " mm");
                return true;
            }
            const Sketch* current = sketch();
            if (current == nullptr) return fail("no sketch yet; use `sketch NAME` first");
            std::vector<SketchElementRef> selection;
            for (std::size_t i = 1; i < tokens.size(); ++i) {
                SketchElementRef ref{};
                if (!parseRef(tokens[i], &ref)) return false;
                selection.push_back(ref);
            }
            const MeasureResult measured = MeasureSketch(*current, selection);
            if (!measured.ok) return fail(measured.message);
            for (const MeasureItem& item : measured.items) {
                std::string line = "  " + item.label + " = " + FormatNumber(item.value);
                // THROUGH THE ONE table, so a unit added to the measure model
                // cannot arrive here printed as something else.
                const std::string suffix = MeasureUnitSuffix(item.unit);
                if (!suffix.empty()) line += " " + suffix;
                // SAID, not hidden. A spline's length has no closed form, and a
                // reader told 128.4 mm without being told it is approximate
                // will use it as though it were not.
                if (item.approximate) line += " (approx)";
                note(line);
            }
            return true;
        }
        if (verb == "echo") {
            std::string rest;
            for (std::size_t i = 1; i < tokens.size(); ++i) {
                if (!rest.empty()) rest += " ";
                rest += tokens[i];
            }
            note(rest);
            return true;
        }
        return fail("unknown command '" + verb +
                    "'; known: sketch, tool, click, finish, constrain, dimension, "
                    "pad, solve, save, echo, help");
    }

    bool doSketch(const std::vector<std::string>& tokens) {
        // `sketch NAME` | `sketch NAME xz` | `sketch NAME xy 40`
        //
        // A PLANE, because M19 needs sketches that are not all on top of each
        // other: a section swept along a spine drawn on its own plane has no
        // volume, and a loft between two sections in the same place has nothing
        // to run through. Until now every script sketch was world XY, which
        // made both of those impossible to write.
        if (tokens.size() < 2 || tokens.size() > 4) return fail("sketch needs a name");
        SketchFrame frame = SketchFrame::WorldXY();
        if (tokens.size() >= 3) {
            double offset = 0.0;
            if (tokens.size() == 4 && !ParseNumber(tokens[3], &offset))
                return fail("a sketch's offset is a distance in mm along its normal");
            // u AND the normal; v follows from them, so nothing here chooses a
            // handedness the rest of the program might choose differently.
            Vec3 uAxis{1, 0, 0};
            Vec3 normal{0, 0, 1};
            if (tokens[2] == "xy") { uAxis = Vec3{1, 0, 0}; normal = Vec3{0, 0, 1}; }
            else if (tokens[2] == "xz") { uAxis = Vec3{1, 0, 0}; normal = Vec3{0, -1, 0}; }
            else if (tokens[2] == "yz") { uAxis = Vec3{0, 1, 0}; normal = Vec3{1, 0, 0}; }
            else return fail("'" + tokens[2] + "' is not a plane; use xy, xz or yz");
            const Vec3 origin{normal.x * offset, normal.y * offset, normal.z * offset};
            const std::optional<SketchFrame> placed =
                SketchFrame::FromBasis(origin, uAxis, normal);
            if (!placed) return fail("that plane is degenerate");
            frame = *placed;
        }
        Sketch& made = document_.addSketch(tokens[1], frame);
        sketchId_ = made.id();
        names_.clear();
        counts_.clear();
        model_.setTool(SketchTool::Select);
        model_.clearSelection();
        note("sketch " + tokens[1]);
        return true;
    }

    bool doTool(const std::vector<std::string>& tokens) {
        if (tokens.size() != 2) return fail("tool needs a name");
        if (sketch() == nullptr) return fail("there is no sketch yet; start one with `sketch`");
        for (const ToolName& entry : kTools) {
            if (tokens[1] != entry.text) continue;
            model_.setTool(entry.tool);
            note("tool " + tokens[1]);
            return true;
        }
        return fail("'" + tokens[1] + "' is not a tool; known: " + Join(ScriptToolNames()));
    }

    bool doClick(const std::vector<std::string>& tokens) {
        if (tokens.size() != 3) return fail("click needs a u and a v, in mm");
        const Sketch* current = sketch();
        if (current == nullptr) return fail("there is no sketch yet");
        double u = 0.0;
        double v = 0.0;
        if (!ParseNumber(tokens[1], &u) || !ParseNumber(tokens[2], &v))
            return fail("click needs two numbers");

        // THROUGH THE REAL SNAPPER, so a click that lands on existing geometry
        // infers the same coincidence a mouse click would. That is the whole
        // point of driving the model rather than building entities directly.
        const SnapResult snap = SnapCursor(*current, Vec2{u, v}, 0.5, 0.0, false);
        const SketchEdit edit = model_.click(snap);
        if (!edit.valid()) {
            note("click " + tokens[1] + " " + tokens[2]);
            return true; // a tool mid-sequence: nothing to apply yet
        }
        const SketchEditOutcome outcome = ApplySketchEdit(document_, sketchId_, edit);
        model_.afterApply(outcome.createdEntities);
        if (!outcome.applied)
            return fail(outcome.status.empty() ? "that click was refused" : outcome.status);
        nameCreated(outcome.createdEntities, "click " + tokens[1] + " " + tokens[2]);
        return true;
    }

    bool doFinish() {
        if (sketch() == nullptr) return fail("there is no sketch yet");
        // THERE IS NO `close` VERB, deliberately: a spline is closed by clicking
        // its first point again, and `click` already does that -- the same
        // gesture the mouse makes. A second way to say it would be a second path
        // into the model, and the second path is the one that keeps working
        // while the first one breaks.
        const SketchEdit edit = model_.finishPendingSpline();
        if (!edit.valid())
            return fail("there is nothing to finish; only the spline tool takes an open-ended "
                        "run of points");
        const SketchEditOutcome outcome = ApplySketchEdit(document_, sketchId_, edit);
        model_.afterApply(outcome.createdEntities);
        if (!outcome.applied)
            return fail(outcome.status.empty() ? "that shape was refused" : outcome.status);
        nameCreated(outcome.createdEntities, "finish");
        return true;
    }

    bool doConstrain(const std::vector<std::string>& tokens) {
        if (tokens.size() < 3) return fail("constrain needs a kind and at least one reference");
        const Sketch* current = sketch();
        if (current == nullptr) return fail("there is no sketch yet");
        SketchEditKind kind = SketchEditKind::None;
        for (const KindName& entry : kConstraints)
            if (tokens[1] == entry.text) kind = entry.kind;
        if (kind == SketchEditKind::None)
            return fail("'" + tokens[1] + "' is not a constraint; known: " +
                        Join(ScriptConstraintNames()));

        std::vector<SketchElementRef> refs;
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            SketchElementRef ref;
            if (!parseRef(tokens[i], &ref)) return false;
            refs.push_back(ref);
        }
        model_.setSelection(refs);
        std::string whyNot;
        const SketchEdit edit = model_.requestConstraint(*current, kind, &whyNot);
        if (!edit.valid()) return fail(whyNot.empty() ? "that selection was refused" : whyNot);
        const SketchEditOutcome outcome = ApplySketchEdit(document_, sketchId_, edit);
        if (!outcome.applied)
            return fail(outcome.status.empty() ? "the sketch refused it" : outcome.status);
        model_.clearSelection();
        note("constrain " + tokens[1]);
        return true;
    }

    bool doDimension(const std::vector<std::string>& tokens) {
        // dimension KIND REF... VALUE [as NAME]
        if (tokens.size() < 4) return fail("dimension needs a kind, a reference and a value");
        const Sketch* current = sketch();
        if (current == nullptr) return fail("there is no sketch yet");
        SketchEditKind kind = SketchEditKind::None;
        for (const KindName& entry : kDimensions)
            if (tokens[1] == entry.text) kind = entry.kind;
        if (kind == SketchEditKind::None)
            return fail("'" + tokens[1] + "' is not a dimension; known: " +
                        Join(ScriptDimensionNames()));

        std::size_t last = tokens.size();
        std::string rename;
        if (tokens.size() >= 3 && tokens[tokens.size() - 2] == "as") {
            rename = tokens.back();
            last = tokens.size() - 2;
        }
        if (last < 4) return fail("dimension needs a value before `as`");
        const std::string valueText = tokens[last - 1];

        std::vector<SketchElementRef> refs;
        for (std::size_t i = 2; i + 1 < last; ++i) {
            SketchElementRef ref;
            if (!parseRef(tokens[i], &ref)) return false;
            refs.push_back(ref);
        }
        if (refs.empty()) return fail("dimension needs at least one reference");

        model_.setSelection(refs);
        std::string whyNot;
        const SketchEdit edit = model_.requestDimension(*current, kind, &whyNot);
        if (!edit.valid()) return fail(whyNot.empty() ? "that selection was refused" : whyNot);
        const SketchEditOutcome outcome = ApplySketchEdit(document_, sketchId_, edit);
        if (!outcome.applied)
            return fail(outcome.status.empty() ? "the sketch refused it" : outcome.status);
        model_.clearSelection();
        if (outcome.createdConstraints.empty())
            return fail("that dimension created no constraint");

        // THE VALUE, through the dimension editor -- the same call the panel
        // makes, so an expression works here exactly as it does there and
        // degrees are converted in the one place that knows to.
        std::string text;
        if (!parseValue(valueText, &text)) return false;
        const Sketch* afterAdd = sketch();
        const SketchEditOutcome committed = CommitDimensionValue(
            document_, *afterAdd, outcome.createdConstraints.front(), text);
        if (!committed.applied)
            return fail(committed.status.empty() ? "that value was refused" : committed.status);

        std::string made = "dimension " + tokens[1] + " = " + valueText;
        if (!rename.empty()) {
            const ObjectId parameter = outcome.createdParameter;
            if (parameter == kInvalidObjectId)
                return fail("that dimension created no Parameter to name");
            const PartDocument::RenameResult renamed = document_.renameObject(parameter, rename);
            if (!renamed.ok)
                return fail(renamed.message.empty()
                                ? "the document refused the name '" + rename + "'"
                                : renamed.message);
            made += " as " + rename;
        }
        note(made);
        return true;
    }

    bool doPad(const std::vector<std::string>& tokens) {
        // pad BODY VALUE [as NAME]
        if (tokens.size() < 3) return fail("pad needs a body name and a length");
        if (sketch() == nullptr) return fail("there is no sketch to pad");
        std::size_t last = tokens.size();
        std::string rename;
        if (tokens.size() >= 4 && tokens[tokens.size() - 2] == "as") {
            rename = tokens.back();
            last = tokens.size() - 2;
        }
        if (last < 3) return fail("pad needs a length before `as`");
        double length = 0.0;
        if (!ParseNumber(tokens[last - 1], &length))
            return fail("pad needs a length in mm");

        const std::string parameterName =
            rename.empty() ? std::string("PadLength") + std::to_string(++padCount_) : rename;
        if (document_.parameters().findByName(parameterName) != nullptr)
            return fail("a Parameter is already called '" + parameterName + "'");
        Parameter& thickness =
            document_.addParameter(parameterName, length, UnitType::Millimeter);
        Body& body = bodyNamed(tokens[1]);
        document_.addPadFeature(body, tokens[1] + "Pad", sketchId_, thickness.id());
        note("pad " + tokens[1] + " " + tokens[last - 1] + " (" + parameterName + ")");
        return true;
    }

    int padCount_{0};

    // A sketch BY NAME, which is how a script refers to one it made earlier.
    // Names are the script's own handle on things -- the same way entities get
    // `Line1` -- so a sweep says which section and which spine in the words the
    // author already used.
    const Sketch* sketchNamed(const std::string& name) const {
        for (const Sketch* one : document_.sketches())
            if (one->name() == name) return one;
        return nullptr;
    }

    // A FACE, said the way a script can say it: `top`, `bottom`, `+x`, `-y`,
    // or `made-by BODY` for everything a body's last feature created.
    //
    // The vocabulary is deliberately small. A face query is a conjunction of
    // conditions and the useful ones on a prismatic part are "the extreme face
    // that way" and "what that feature made"; anything richer is a picking
    // problem, not a typing one, and belongs in the canvas.
    bool parseFace(const std::string& text, FaceQuery* out) {
        static const struct { const char* name; Vec3 direction; } kDirections[] = {
            {"top", Vec3{0, 0, 1}},  {"bottom", Vec3{0, 0, -1}}, {"+x", Vec3{1, 0, 0}},
            {"-x", Vec3{-1, 0, 0}},  {"+y", Vec3{0, 1, 0}},      {"-y", Vec3{0, -1, 0}},
            {"+z", Vec3{0, 0, 1}},   {"-z", Vec3{0, 0, -1}},
        };
        for (const auto& entry : kDirections) {
            if (text != entry.name) continue;
            out->extremeTowards = entry.direction;
            return true;
        }
        // `facing:+y` -- every face pointing that way, however far out it is.
        // Different from `+y`, which is the OUTERMOST one, and the difference
        // matters the moment a part has a step in it.
        if (text.rfind("facing:", 0) == 0) {
            for (const auto& entry : kDirections) {
                if (text.substr(7) != entry.name) continue;
                out->facing = entry.direction;
                return true;
            }
        }
        return fail("'" + text +
                    "' is not a face; use top, bottom, +x, -x, +y, -y, +z, -z, or facing:DIR");
    }

    bool doExport(const std::vector<std::string>& tokens) {
        // export BODY FILE [DEFLECTION]
        //
        // The format is chosen by the EXTENSION, because that is what the file
        // is going to be read as at the far end. Asking for it separately would
        // let a .step be written as STL, which every reader would then refuse
        // for a reason that names neither this program nor the choice.
        if (tokens.size() != 3 && tokens.size() != 4)
            return fail("export needs a body and a file name");
        Body* body = document_.findBodyNamed(tokens[1]);
        if (body == nullptr) return fail("there is no body called '" + tokens[1] + "'");
        const ObjectId tip = lastSolidIn(*body);
        if (tip == kInvalidObjectId) return fail("'" + tokens[1] + "' has no solid to export");

        const ISolidFeature* solid = solidFeature(tip);
        if (solid == nullptr) return fail("'" + tokens[1] + "' has no solid to export");
        if (solid->currentState() != ComputeState::Valid || !solid->currentShape().isValid())
            return fail("'" + tokens[1] +
                        "' has not been built yet -- `solve` before exporting");

        IGeometryKernel* kernel = document_.geometryKernel();
        if (kernel == nullptr) return fail("no geometry kernel configured");

        const std::string& path = tokens[2];
        const std::string suffix = LowerSuffix(path);
        IoResult written;
        if (suffix == "stl") {
            // A DEFAULT DEFLECTION, said out loud in the log rather than
            // silent: STL is triangles and the number decides how many, so a
            // file written at a deflection nobody chose is a file nobody can
            // reproduce.
            double deflection = 0.05;
            if (tokens.size() == 4 && !ParseNumber(tokens[3], &deflection))
                return fail("an STL deflection is a distance in mm");
            written = kernel->exportStl(solid->currentShape(), path, deflection);
            if (written) note("export " + tokens[1] + " -> " + path + " (STL, deflection " +
                              FormatNumber(deflection) + " mm)");
        } else if (suffix == "step" || suffix == "stp") {
            if (tokens.size() == 4)
                return fail("only an STL export takes a deflection");
            written = kernel->exportStep(solid->currentShape(), path);
            if (written) note("export " + tokens[1] + " -> " + path + " (STEP AP214, mm)");
        } else {
            return fail("'" + path + "' has no extension this can write; use .step or .stl");
        }
        if (!written) return fail(written.message);
        return true;
    }

    bool doImport(const std::vector<std::string>& tokens) {
        // import FILE as BODY
        if (tokens.size() != 4 || tokens[2] != "as")
            return fail("import needs `import FILE as BODY`");
        const std::string suffix = LowerSuffix(tokens[1]);
        if (suffix != "step" && suffix != "stp")
            return fail("'" + tokens[1] + "' is not a STEP file; only STEP can be imported");
        Body& body = bodyNamed(tokens[3]);
        // THE PATH IS STORED, not the geometry: the file is the source of
        // truth and is read again on every rebuild.
        document_.addImportFeature(body, tokens[3] + "Import", tokens[1]);
        note("import " + tokens[1] + " as " + tokens[3]);
        return true;
    }

    bool doBoolean(const std::vector<std::string>& tokens) {
        // union BODY | subtract BODY | intersect BODY
        //
        // ONE BODY, and that is the model rather than a limitation of the
        // script. A Body here is a feature chain, and two features in it that
        // nothing consumes are two disjoint solids -- which is what multi-body
        // means: `pad Case 40` twice with two sketches gives one body holding
        // two parts. A boolean joins two of them into one.
        //
        // It cannot span two bodies, because a consumer's base must be an
        // EARLIER feature of the same body: restore replays features in array
        // order, and a consumer restored before its operand would wire an edge
        // to a node that does not exist yet.
        //
        // The two it takes are the body's two unconsumed solids, IN THE ORDER
        // THEY WERE DRAWN -- so `subtract` removes the second from the first,
        // which is the order the script reads in.
        if (tokens.size() != 2) return fail(tokens[0] + " needs a body");
        Body* body = document_.findBodyNamed(tokens[1]);
        if (body == nullptr) return fail("there is no body called '" + tokens[1] + "'");

        const std::vector<ObjectId> loose = unconsumedSolidsIn(*body);
        if (loose.size() < 2)
            return fail("'" + tokens[1] + "' holds " + std::to_string(loose.size()) +
                        " separate solid(s); a boolean needs two -- pad into the same body "
                        "twice to make them");

        BooleanOperation operation = BooleanOperation::Union;
        if (tokens[0] == "subtract") operation = BooleanOperation::Subtract;
        else if (tokens[0] == "intersect") operation = BooleanOperation::Intersect;

        document_.addBooleanFeature(*body, tokens[1] + BooleanOperationName(operation),
                                    operation, loose[loose.size() - 2], loose.back());
        note(tokens[0] + " " + tokens[1]);
        return true;
    }

    bool doRing(const std::vector<std::string>& tokens) {
        // ring BODY COUNT DEGREES
        if (tokens.size() != 4)
            return fail("ring needs a body, a count and a step in degrees");
        Body* body = document_.findBodyNamed(tokens[1]);
        if (body == nullptr) return fail("there is no body called '" + tokens[1] + "'");
        double howMany = 0.0;
        double degrees = 0.0;
        if (!ParseNumber(tokens[2], &howMany)) return fail("a ring's count is a whole number");
        if (!ParseNumber(tokens[3], &degrees)) return fail("a ring's step is in degrees");
        const ObjectId base = lastSolidIn(*body);
        if (base == kInvalidObjectId) return fail("'" + tokens[1] + "' has nothing to pattern");

        // THE AXIS IS THE WORLD Z through the origin, which is what a frame at
        // the origin gives -- the same +Z convention a mirror's plane normal
        // and a linear pattern's direction already follow.
        ReferenceFrame& axis = document_.addFrame("RingAxis" + std::to_string(++ringCount_));
        Parameter& count =
            document_.addParameter("RingCount" + std::to_string(ringCount_), howMany,
                                   UnitType::Unitless);
        Parameter& step = document_.addParameter(
            "RingStep" + std::to_string(ringCount_),
            degrees * 3.14159265358979323846 / 180.0, UnitType::Radian);
        document_.addCircularPatternFeature(*body, tokens[1] + "Ring", base, axis.id(),
                                            count.id(), step.id());
        note("ring " + tokens[1] + " x" + tokens[2] + " at " + tokens[3] + " deg");
        return true;
    }

    bool doAlong(const std::vector<std::string>& tokens) {
        // along BODY SKETCH COUNT
        if (tokens.size() != 4) return fail("along needs a body, a path sketch and a count");
        Body* body = document_.findBodyNamed(tokens[1]);
        if (body == nullptr) return fail("there is no body called '" + tokens[1] + "'");
        const Sketch* path = sketchNamed(tokens[2]);
        if (path == nullptr) return fail("there is no sketch called '" + tokens[2] + "'");
        double howMany = 0.0;
        if (!ParseNumber(tokens[3], &howMany)) return fail("a count is a whole number");
        const ObjectId base = lastSolidIn(*body);
        if (base == kInvalidObjectId) return fail("'" + tokens[1] + "' has nothing to pattern");

        Parameter& count = document_.addParameter(
            "AlongCount" + std::to_string(++alongCount_), howMany, UnitType::Unitless);
        document_.addCurvePatternFeature(*body, tokens[1] + "Along", base, path->id(),
                                         count.id());
        note("along " + tokens[1] + " " + tokens[2] + " x" + tokens[3]);
        return true;
    }

    int ringCount_{0};
    int alongCount_{0};

    bool doShell(const std::vector<std::string>& tokens) {
        // shell BODY THICKNESS FACE [FACE...]
        if (tokens.size() < 4) return fail("shell needs a body, a thickness and a face to open");
        Body* body = document_.findBodyNamed(tokens[1]);
        if (body == nullptr) return fail("there is no body called '" + tokens[1] + "'");
        double thickness = 0.0;
        if (!ParseNumber(tokens[2], &thickness)) return fail("a shell's thickness is in mm");
        FaceSelection faces;
        for (std::size_t i = 3; i < tokens.size(); ++i) {
            FaceQuery query;
            if (!parseFace(tokens[i], &query)) return false;
            faces.push_back(std::move(query));
        }
        const ObjectId base = lastSolidIn(*body);
        if (base == kInvalidObjectId)
            return fail("'" + tokens[1] + "' has nothing to hollow yet");
        const std::string parameterName = "ShellWall" + std::to_string(++shellCount_);
        Parameter& wall = document_.addParameter(parameterName, thickness, UnitType::Millimeter);
        document_.addShellFeature(*body, tokens[1] + "Shell", base, std::move(faces), wall.id());
        note("shell " + tokens[1] + " " + tokens[2] + " (" + parameterName + ")");
        return true;
    }

    bool doDraft(const std::vector<std::string>& tokens) {
        // draft BODY DEGREES NEUTRAL FACE [FACE...]
        if (tokens.size() < 5)
            return fail("draft needs a body, an angle in degrees, a neutral face and a face to "
                        "taper");
        Body* body = document_.findBodyNamed(tokens[1]);
        if (body == nullptr) return fail("there is no body called '" + tokens[1] + "'");
        double degrees = 0.0;
        if (!ParseNumber(tokens[2], &degrees)) return fail("a draft's angle is in degrees");
        FaceQuery neutral;
        if (!parseFace(tokens[3], &neutral)) return false;
        FaceSelection faces;
        for (std::size_t i = 4; i < tokens.size(); ++i) {
            FaceQuery query;
            if (!parseFace(tokens[i], &query)) return false;
            faces.push_back(std::move(query));
        }
        const ObjectId base = lastSolidIn(*body);
        if (base == kInvalidObjectId)
            return fail("'" + tokens[1] + "' has nothing to taper yet");
        // DEGREES IN, RADIANS STORED. A script says what a drawing says, and a
        // drawing says 3 degrees -- but the Parameter carries Radian because
        // that is what the feature's unit check demands, and the conversion
        // happens once, here.
        const std::string parameterName = "DraftAngle" + std::to_string(++draftCount_);
        Parameter& angle = document_.addParameter(
            parameterName, degrees * 3.14159265358979323846 / 180.0, UnitType::Radian);
        document_.addDraftFeature(*body, tokens[1] + "Draft", base, std::move(faces),
                                  std::move(neutral), angle.id());
        note("draft " + tokens[1] + " " + tokens[2] + " deg (" + parameterName + ")");
        return true;
    }

    bool doHole(const std::vector<std::string>& tokens) {
        // hole BODY SKETCH DIAMETER [DEPTH]
        if (tokens.size() != 4 && tokens.size() != 5)
            return fail("hole needs a body, a sketch of points and a diameter");
        Body* body = document_.findBodyNamed(tokens[1]);
        if (body == nullptr) return fail("there is no body called '" + tokens[1] + "'");
        const Sketch* where = sketchNamed(tokens[2]);
        if (where == nullptr) return fail("there is no sketch called '" + tokens[2] + "'");
        double bore = 0.0;
        if (!ParseNumber(tokens[3], &bore)) return fail("a hole's diameter is in mm");
        // NO DEPTH MEANS THROUGH ALL, which is what a hole usually is, and the
        // feature spells that as a depth of nought.
        double depth = 0.0;
        if (tokens.size() == 5 && !ParseNumber(tokens[4], &depth))
            return fail("a hole's depth is in mm, and leaving it out means all the way through");
        const ObjectId base = lastSolidIn(*body);
        if (base == kInvalidObjectId)
            return fail("'" + tokens[1] + "' has nothing to drill yet");
        const std::string boreName = "HoleDia" + std::to_string(++holeCount_);
        const std::string depthName = "HoleDepth" + std::to_string(holeCount_);
        Parameter& diameter = document_.addParameter(boreName, bore, UnitType::Millimeter);
        Parameter& deep = document_.addParameter(depthName, depth, UnitType::Millimeter);
        document_.addHoleFeature(*body, tokens[1] + "Hole", base, where->id(), diameter.id(),
                                 deep.id());
        note("hole " + tokens[1] + " " + tokens[3] + " through " + tokens[2]);
        return true;
    }

    // ONE BODY PER NAME. `addBody` always makes a new one, so building a part
    // in two steps -- `pad Pipe`, then `shell Pipe` -- used to leave two bodies
    // both called Pipe, and the second command dressed an empty one.
    Body& bodyNamed(const std::string& name) {
        if (Body* found = document_.findBodyNamed(name)) return *found;
        return document_.addBody(name);
    }

    int shellCount_{0};
    int draftCount_{0};
    int holeCount_{0};

    // A file's extension, lower-cased, or empty. One place, so `export` and
    // `import` cannot disagree about what a `.STEP` is.
    static std::string LowerSuffix(const std::string& path) {
        const std::size_t dot = path.rfind('.');
        if (dot == std::string::npos || dot + 1 >= path.size()) return {};
        std::string suffix = path.substr(dot + 1);
        for (char& c : suffix)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return suffix;
    }

    // WHAT A READER CALLED IT. An id in a failure message names nothing a
    // person typed, so every failure that can name a feature or a sketch does.
    std::string nameOfObject(ObjectId id) const {
        for (const auto& body : document_.bodies())
            for (const auto& feature : body->features())
                if (feature->id() == id) return feature->name();
        for (const Sketch* one : document_.sketches())
            if (one->id() == id) return one->name();
        return {};
    }

    static const char* RecomputeStatusName(RecomputeStatus status) {
        switch (status) {
            case RecomputeStatus::Success: return "ok";
            case RecomputeStatus::Failed: return "failed with no reason given";
            case RecomputeStatus::BlockedByDependency: return "blocked by something upstream";
            case RecomputeStatus::SkippedSuppressed: return "suppressed";
        }
        return "unknown";
    }

    const ISolidFeature* solidFeature(ObjectId id) const {
        for (const auto& body : document_.bodies())
            for (const auto& feature : body->features())
                if (feature->id() == id)
                    return dynamic_cast<const ISolidFeature*>(feature.get());
        return nullptr;
    }

    // THE SOLIDS NOTHING HAS EATEN, in the order they were made -- the body's
    // separate parts. Two of them is multi-body; a boolean turns two into one.
    std::vector<ObjectId> unconsumedSolidsIn(const Body& body) const {
        std::vector<ObjectId> eaten;
        for (const auto& feature : body.features())
            if (const auto* solid = dynamic_cast<const ISolidFeature*>(feature.get()))
                for (const ObjectId one : solid->consumedSolidIds())
                    if (one != kInvalidObjectId) eaten.push_back(one);
        std::vector<ObjectId> loose;
        for (const auto& feature : body.features()) {
            if (dynamic_cast<const ISolidFeature*>(feature.get()) == nullptr) continue;
            if (std::find(eaten.begin(), eaten.end(), feature->id()) != eaten.end()) continue;
            loose.push_back(feature->id());
        }
        return loose;
    }

    // WHAT TO DRESS: the last solid feature in the body, which is what "the
    // part as it stands" means in a script that builds top to bottom. A chain
    // feature has to name its base, and asking the user to name it by id in a
    // script that never showed them one would be asking for a number they do
    // not have.
    ObjectId lastSolidIn(const Body& body) const {
        ObjectId last = kInvalidObjectId;
        for (const auto& feature : body.features())
            if (dynamic_cast<const ISolidFeature*>(feature.get()) != nullptr)
                last = feature->id();
        return last;
    }

    bool doSweep(const std::vector<std::string>& tokens) {
        // sweep BODY PROFILE PATH
        if (tokens.size() != 4)
            return fail("sweep needs a body, a profile sketch and a path sketch");
        const Sketch* profile = sketchNamed(tokens[2]);
        if (profile == nullptr) return fail("there is no sketch called '" + tokens[2] + "'");
        const Sketch* path = sketchNamed(tokens[3]);
        if (path == nullptr) return fail("there is no sketch called '" + tokens[3] + "'");
        if (profile->id() == path->id())
            return fail("a sweep needs two different sketches: a section swept along a path on "
                        "its own plane has no volume");
        Body& body = bodyNamed(tokens[1]);
        document_.addSweepFeature(body, tokens[1] + "Sweep", profile->id(), path->id());
        note("sweep " + tokens[1] + " along " + tokens[3]);
        return true;
    }

    bool doLoft(const std::vector<std::string>& tokens) {
        // loft BODY SECTION SECTION [SECTION...]
        if (tokens.size() < 4) return fail("loft needs a body and at least two sections");
        std::vector<ObjectId> sections;
        for (std::size_t i = 2; i < tokens.size(); ++i) {
            const Sketch* one = sketchNamed(tokens[i]);
            if (one == nullptr) return fail("there is no sketch called '" + tokens[i] + "'");
            // IN THE ORDER THEY WERE TYPED. Nothing sorts them, because the
            // order is the shape.
            sections.push_back(one->id());
        }
        Body& body = bodyNamed(tokens[1]);
        document_.addLoftFeature(body, tokens[1] + "Loft", std::move(sections));
        note("loft " + tokens[1] + " through " + std::to_string(tokens.size() - 2) + " sections");
        return true;
    }

    bool doSolve() {
        // THE PART FIRST, ALWAYS. A script that builds parts and then
        // assembles them needs both current, and the assembly reads the parts
        // from FILES rather than from this document -- so the order here is
        // not a dependency, it is just the order the log reads best in.
        const DocumentRecomputeReport report = document_.recompute();
        for (const Sketch* one : document_.sketches()) {
            std::string what = "  " + one->name() + ": DOF " +
                               std::to_string(one->degreesOfFreedom()) + " " +
                               SolveStatusName(one->solveStatus());
            if (!one->solveMessage().empty()) what += " -- " + one->solveMessage();
            outcome_->log.push_back(ScriptLogEntry{line_, what});
        }
        if (!report.success) {
            // THE REASON, not a pointer at a log that never got it. The report
            // carries a message per failed item and this used to throw all of
            // them away and say "see the log above", which was the one thing
            // above that was not there.
            std::string why;
            for (const RecomputeItemReport& item : report.items) {
                if (item.status == RecomputeStatus::Success) continue;
                std::string named = nameOfObject(item.id);
                if (named.empty()) named = "#" + std::to_string(item.id);
                why += (why.empty() ? "" : "; ") + named + ": " +
                       (item.message.empty() ? RecomputeStatusName(item.status) : item.message);
            }
            return fail(why.empty() ? "recompute failed" : "recompute failed -- " + why);
        }
        if (assembly_ != nullptr) {
            const DocumentRecomputeReport built = assembly_->recompute();
            if (!built.success) {
                std::string why;
                for (const RecomputeItemReport& item : built.items) {
                    if (item.status == RecomputeStatus::Success) continue;
                    std::string named = assembly_->objectName(item.id);
                    if (named.empty()) named = "#" + std::to_string(item.id);
                    why += (why.empty() ? "" : "; ") + named + ": " +
                           (item.message.empty() ? RecomputeStatusName(item.status)
                                                 : item.message);
                }
                return fail(why.empty() ? "the assembly did not build"
                                        : "the assembly did not build -- " + why);
            }
        }
        note("solve");
        return true;
    }

    // The rotation that takes +Z onto `axis`. Used by `connector`, which is the
    // one place a script says which way a mate's freedom points.
    //
    // The degenerate case is REFUSED rather than defaulted: a zero-length axis
    // is a point, not a direction, and quietly using +Z would put a hinge on
    // an axis nobody chose.
    static bool RotationTakingZTo(const Vec3& axis, Quaternion* out) {
        const double length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
        if (!(length > 1e-9)) return false;
        const Vec3 to{axis.x / length, axis.y / length, axis.z / length};
        // The shortest rotation from +Z to `to`: axis = z x to, angle from the
        // dot product. The antiparallel case has no shortest rotation -- every
        // axis in the XY plane works -- so +X is chosen and said so here.
        const double dot = to.z;
        if (dot > 1.0 - 1e-12) {
            *out = Quaternion{1.0, 0.0, 0.0, 0.0};
            return true;
        }
        if (dot < -1.0 + 1e-12) {
            *out = Quaternion{0.0, 1.0, 0.0, 0.0}; // half turn about +X
            return true;
        }
        const Vec3 cross{-to.y, to.x, 0.0}; // z_hat x to
        const double s = std::sqrt((1.0 + dot) * 2.0);
        *out = Quaternion{s / 2.0, cross.x / s, cross.y / s, cross.z / s};
        return true;
    }

    bool doConnector(const std::vector<std::string>& tokens) {
        // connector NAME X Y Z [AX AY AZ]
        //
        // A MATE CONNECTOR ON THE PART, which is where one belongs: define
        // "the shaft axis" once on the motor and every motor in every assembly
        // has it (roadmap §21). This is a PART verb, not an assembly verb.
        //
        // The three optional numbers are where the connector's +Z POINTS, and
        // that is not decoration: a revolute turns about +Z and a slider
        // slides along it, so pointing Z down the hinge pin is the whole act
        // of saying where the hinge is. Default is +Z, which is what a part
        // drawn with its axis up already wants.
        if (tokens.size() != 5 && tokens.size() != 8)
            return fail("connector needs a name and x y z, and optionally an axis");
        Vec3 at{};
        if (!ParseNumber(tokens[2], &at.x) || !ParseNumber(tokens[3], &at.y) ||
            !ParseNumber(tokens[4], &at.z))
            return fail("a connector needs three numbers in mm");
        Vec3 axis{0.0, 0.0, 1.0};
        if (tokens.size() == 8) {
            if (!ParseNumber(tokens[5], &axis.x) || !ParseNumber(tokens[6], &axis.y) ||
                !ParseNumber(tokens[7], &axis.z))
                return fail("a connector axis needs three numbers");
        }
        Transform3D placement;
        placement.translation = at;
        if (!RotationTakingZTo(axis, &placement.rotation))
            return fail("a connector axis needs a direction, not a point");

        // A connector IS a frame plus meaning (ADR-M10-004), so this makes
        // both -- and the frame is named after the connector so a tree reads
        // as one thing rather than two.
        ReferenceFrame& frame = document_.addFrame(tokens[1] + " frame");
        document_.setFrameTransform(frame.id(), placement);
        document_.addConnector(tokens[1], ConnectorRole::Generic, frame.id());
        note("connector " + tokens[1] + " at " + FormatNumber(at.x) + ", " +
             FormatNumber(at.y) + ", " + FormatNumber(at.z) + " along " +
             FormatNumber(axis.x) + ", " + FormatNumber(axis.y) + ", " + FormatNumber(axis.z));
        return true;
    }

    bool doGround(const std::vector<std::string>& tokens) {
        // ground INSTANCE
        if (assembly_ == nullptr)
            return fail("there is no assembly yet; use `assembly NAME` first");
        if (tokens.size() != 2) return fail("ground needs an instance");
        const PartInstance* instance = assembly_->findInstanceNamed(tokens[1]);
        if (instance == nullptr)
            return fail("there is no instance called '" + tokens[1] + "'");
        if (!assembly_->setInstanceGrounded(instance->id(), true))
            return fail("could not ground '" + tokens[1] + "'");
        note("ground " + tokens[1]);
        return true;
    }

    bool doMate(const std::vector<std::string>& tokens) {
        // mate KIND NAME A/connector B/connector [VALUE]
        //
        // The value is in the unit that mate's remaining freedom has, said the
        // way a drawing says it: DEGREES for a revolute, millimetres for a
        // slider. The conversion to radians happens here, once, exactly as
        // `draft` does it -- the model stores radians and a script should not
        // have to know that.
        if (assembly_ == nullptr)
            return fail("there is no assembly yet; use `assembly NAME` first");
        if (tokens.size() != 5 && tokens.size() != 6)
            return fail("mate needs a kind, a name, and two INSTANCE/CONNECTOR ends");
        MateType type = MateType::Fastened;
        if (tokens[1] == "fastened") type = MateType::Fastened;
        else if (tokens[1] == "revolute") type = MateType::Revolute;
        else if (tokens[1] == "slider") type = MateType::Slider;
        else return fail("'" + tokens[1] + "' is not a mate kind; use fastened, revolute or "
                         "slider");

        ObjectId ends[2] = {kInvalidObjectId, kInvalidObjectId};
        std::string connectors[2];
        for (int i = 0; i < 2; ++i) {
            const std::string& text = tokens[3 + i];
            const std::size_t slash = text.find('/');
            if (slash == std::string::npos || slash == 0 || slash + 1 >= text.size())
                return fail("'" + text + "' is not an INSTANCE/CONNECTOR pair");
            const PartInstance* instance =
                assembly_->findInstanceNamed(text.substr(0, slash));
            if (instance == nullptr)
                return fail("there is no instance called '" + text.substr(0, slash) + "'");
            ends[i] = instance->id();
            connectors[i] = text.substr(slash + 1);
        }

        double value = 0.0;
        if (tokens.size() == 6) {
            if (!ParseNumber(tokens[5], &value)) return fail("a mate value is a number");
            if (type == MateType::Fastened)
                return fail("a fastened mate has no freedom to give a value to");
            if (type == MateType::Revolute) value = value * 3.14159265358979323846 / 180.0;
        }
        if (assembly_->findMateNamed(tokens[2]) != nullptr)
            return fail("there is already a mate called '" + tokens[2] + "'");
        assembly_->addMate(tokens[2], type, ends[0], connectors[0], ends[1], connectors[1],
                           value);
        note("mate " + tokens[1] + " " + tokens[2] + ": " + tokens[3] + " <-> " + tokens[4] +
             (tokens.size() == 6 ? " at " + tokens[5] : ""));
        return true;
    }

    bool doDrive(const std::vector<std::string>& tokens) {
        // drive MATE VALUE  -- turn the hinge
        if (assembly_ == nullptr)
            return fail("there is no assembly yet; use `assembly NAME` first");
        if (tokens.size() != 3) return fail("drive needs a mate and a value");
        const Mate* mate = assembly_->findMateNamed(tokens[1]);
        if (mate == nullptr) return fail("there is no mate called '" + tokens[1] + "'");
        double value = 0.0;
        if (!ParseNumber(tokens[2], &value)) return fail("a mate value is a number");
        if (mate->type() == MateType::Revolute) value = value * 3.14159265358979323846 / 180.0;
        if (!assembly_->setMateValue(mate->id(), value))
            return fail("'" + tokens[1] + "' has no freedom to drive");
        note("drive " + tokens[1] + " to " + tokens[2]);
        return true;
    }

    bool doAssembly(const std::vector<std::string>& tokens) {
        // assembly NAME
        //
        // FROM HERE ON THIS SCRIPT IS ASSEMBLING. The part verbs still work --
        // one script can build three parts, save them, and then put them
        // together, which is the most useful thing a script can do here and
        // the shape of the M23 example.
        //
        // What changes is what `solve`, `measure` and `save` are ABOUT. Said
        // out loud in the log rather than left to be inferred, because a
        // `save` that quietly wrote the wrong document would be discovered by
        // opening the file.
        if (tokens.size() != 2) return fail("assembly needs a name");
        if (assembly_ != nullptr) return fail("this script already has an assembly");
        assembly_ = std::make_unique<AssemblyDocument>(tokens[1]);
        assembly_->setGeometryKernel(document_.geometryKernel());
        assembly_->setSketchSolver(document_.sketchSolver());
        note("assembly " + tokens[1] + " (solve, measure and save now mean the assembly)");
        return true;
    }

    bool doInsert(const std::vector<std::string>& tokens) {
        // insert NAME FILE [BODY]
        if (assembly_ == nullptr)
            return fail("there is no assembly yet; use `assembly NAME` first");
        if (tokens.size() != 3 && tokens.size() != 4)
            return fail("insert needs a name and a part file");
        if (assembly_->findInstanceNamed(tokens[1]) != nullptr)
            return fail("there is already an instance called '" + tokens[1] + "'");
        const std::string body = tokens.size() == 4 ? tokens[3] : std::string();
        assembly_->addInstance(tokens[1], tokens[2], body);
        note("insert " + tokens[1] + " <- " + tokens[2] + (body.empty() ? "" : " [" + body + "]"));
        return true;
    }

    bool doPlace(const std::vector<std::string>& tokens) {
        // place NAME X Y Z [DEGREES]
        //
        // The angle is about +Z, which is the one axis a script can name
        // without a vocabulary for axes. Anything else is M24's business: a
        // mate says where a part goes by what it touches, and typing three
        // Euler angles is the thing mates exist to replace.
        if (assembly_ == nullptr)
            return fail("there is no assembly yet; use `assembly NAME` first");
        if (tokens.size() != 5 && tokens.size() != 6)
            return fail("place needs an instance and x y z, and optionally an angle about +Z");
        const PartInstance* instance = assembly_->findInstanceNamed(tokens[1]);
        if (instance == nullptr)
            return fail("there is no instance called '" + tokens[1] + "'");
        Transform3D placement;
        if (!ParseNumber(tokens[2], &placement.translation.x) ||
            !ParseNumber(tokens[3], &placement.translation.y) ||
            !ParseNumber(tokens[4], &placement.translation.z))
            return fail("a place needs three numbers in mm");
        double degrees = 0.0;
        if (tokens.size() == 6 && !ParseNumber(tokens[5], &degrees))
            return fail("an angle is given in degrees");
        const double radians = degrees * 3.14159265358979323846 / 180.0;
        placement.rotation = Quaternion{std::cos(radians / 2.0), 0.0, 0.0,
                                        std::sin(radians / 2.0)};
        if (!assembly_->setInstanceTransform(instance->id(), placement))
            return fail("could not place '" + tokens[1] + "'");
        note("place " + tokens[1] + " at " + FormatNumber(placement.translation.x) + ", " +
             FormatNumber(placement.translation.y) + ", " +
             FormatNumber(placement.translation.z) +
             (degrees == 0.0 ? "" : " turned " + FormatNumber(degrees) + " deg"));
        return true;
    }

    // Every instance, its size and where it ended up. The assembly's answer to
    // `measure`, and the evidence a script needs that the parts are really
    // there rather than merely listed.
    bool measureAssembly() {
        IGeometryKernel* kernel = assembly_->geometryKernel();
        if (kernel == nullptr) return fail("no geometry kernel configured");
        const std::vector<const PartInstance*> instances = assembly_->instances();
        if (instances.empty()) return fail("this assembly has nothing in it yet");
        double total = 0.0;
        for (const PartInstance* one : instances) {
            if (one->currentState() != ComputeState::Valid || !one->currentShape().isValid())
                return fail("'" + one->name() +
                            "' has not been built yet -- `solve` before measuring");
            const KernelMassPropertiesResult mass =
                kernel->calculateMassProperties(one->currentShape());
            if (!mass) return fail(mass.message);
            total += mass.properties.volumeMm3;
            note("  " + one->name() + ": volume = " +
                 FormatNumber(mass.properties.volumeMm3) + " mm^3, centre = " +
                 FormatNumber(mass.properties.centerOfMassMm.x) + ", " +
                 FormatNumber(mass.properties.centerOfMassMm.y) + ", " +
                 FormatNumber(mass.properties.centerOfMassMm.z) + " mm");
        }
        note("  " + std::to_string(instances.size()) + " instances, total volume = " +
             FormatNumber(total) + " mm^3");
        return true;
    }

    bool doSave(const std::vector<std::string>& tokens) {
        if (tokens.size() != 2) return fail("save needs a path");
        if (assembly_ != nullptr) {
            const SaveResult wrote = saveAssemblyDocumentToFile(*assembly_, tokens[1]);
            if (!wrote)
                return fail(wrote.message.empty() ? "the assembly refused to save"
                                                  : wrote.message);
            note("save " + tokens[1] + " (assembly)");
            return true;
        }
        const SaveResult saved = savePartDocumentToFile(document_, tokens[1]);
        if (!saved)
            return fail(saved.message.empty() ? "the document refused to save" : saved.message);
        note("save " + tokens[1]);
        return true;
    }
};

} // namespace

std::vector<std::string> ScriptToolNames() {
    std::vector<std::string> names;
    for (const ToolName& entry : kTools) names.emplace_back(entry.text);
    return names;
}

std::vector<std::string> ScriptConstraintNames() {
    std::vector<std::string> names;
    for (const KindName& entry : kConstraints) names.emplace_back(entry.text);
    return names;
}

std::vector<std::string> ScriptDimensionNames() {
    std::vector<std::string> names;
    for (const KindName& entry : kDimensions) names.emplace_back(entry.text);
    return names;
}

struct SketchScriptSession::State {
    explicit State(PartDocument& document) : runner(document) {}
    Runner runner;
};

SketchScriptSession::SketchScriptSession(PartDocument& document)
    : state_(std::make_unique<State>(document)) {}

SketchScriptSession::~SketchScriptSession() = default;

ScriptOutcome SketchScriptSession::run(const std::string& text) {
    return state_->runner.run(text);
}

ScriptOutcome RunSketchScript(PartDocument& document, const std::string& text) {
    // ONE session, one call. A file is the degenerate connection.
    return SketchScriptSession(document).run(text);
}

} // namespace paramcad
