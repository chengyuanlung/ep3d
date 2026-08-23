#include "Cli/SketchScript.h"

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
#include <sstream>
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
            if (!command(tokens)) {
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
        if (verb == "solve") return doSolve();
        if (verb == "save") return doSave(tokens);
        if (verb == "help") {
            // OVER A CONNECTION there is no --help to run, and the three
            // vocabularies are the same three lists the CLI's help prints --
            // asked for here rather than copied.
            note("tools: " + Join(ScriptToolNames()));
            note("constraints: " + Join(ScriptConstraintNames()));
            note("dimensions: " + Join(ScriptDimensionNames()));
            note("commands: sketch, tool, click, finish, constrain, dimension, pad, solve, "
                 "save, measure, handle, echo, help");
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
        if (tokens.size() != 2) return fail("sketch needs a name");
        Sketch& made = document_.addSketch(tokens[1]);
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
        Body& body = document_.addBody(tokens[1]);
        document_.addPadFeature(body, tokens[1] + "Pad", sketchId_, thickness.id());
        note("pad " + tokens[1] + " " + tokens[last - 1] + " (" + parameterName + ")");
        return true;
    }

    int padCount_{0};

    bool doSolve() {
        const DocumentRecomputeReport report = document_.recompute();
        for (const Sketch* one : document_.sketches()) {
            std::string what = "  " + one->name() + ": DOF " +
                               std::to_string(one->degreesOfFreedom()) + " " +
                               SolveStatusName(one->solveStatus());
            if (!one->solveMessage().empty()) what += " -- " + one->solveMessage();
            outcome_->log.push_back(ScriptLogEntry{line_, what});
        }
        if (!report.success) return fail("recompute failed; see the log above");
        note("solve");
        return true;
    }

    bool doSave(const std::vector<std::string>& tokens) {
        if (tokens.size() != 2) return fail("save needs a path");
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
