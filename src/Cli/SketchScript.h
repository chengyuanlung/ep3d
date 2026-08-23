#pragma once

#include "Core/Document/PartDocument.h"

#include <memory>
#include <string>
#include <vector>

namespace paramcad {

// A text script that drives the sketch tools (M17.27).
//
// WHY THIS EXISTS, and it is not convenience: nothing in this project has ever
// walked the whole path. The unit tests build documents in memory, the
// serialization tests check that fields come back, and --selftest runs a fixed
// script nobody can change. The one time a path was walked end to end -- the
// worked example under examples/, which draws, saves, reloads and re-solves --
// it found a loader that had been dropping an ellipse dimension's Parameter,
// which 1470 tests had not (ADR-M17-050).
//
// A script is that walk, made repeatable and available to anyone.
//
// IT DRIVES THE SAME PATH THE MOUSE DOES. `tool`/`click` go through
// SketchCanvasModel exactly as a click does, and `constrain`/`dimension` go
// through requestConstraint/requestDimension -- the functions that decide
// whether a selection means anything. A script that built constraint structs
// directly would be a second way into the model, and the second way is always
// the one that stays working while the first one breaks.
//
// ------------------------------------------------------------------ grammar
//
//   # anything after a hash is a comment; blank lines are ignored
//
//   sketch NAME                 start a new sketch and make it current
//   tool NAME                   pick a drawing tool (line, circle, spline, ...)
//   click U V                   one click, in sketch millimetres
//   finish                      end an open-ended tool (spline). A spline is
//                               CLOSED by clicking its first point again, so
//                               there is no separate verb for it
//   constrain KIND REF [REF...] a non-dimensional constraint
//   dimension KIND REF... VALUE [as NAME]
//                               a dimension, its value, and optionally a name
//                               for the Parameter it creates
//   pad BODY VALUE [as NAME]    extrude the current sketch
//   solve                       recompute, and log each sketch's DOF
//   save PATH                   write the document
//   echo TEXT                   put TEXT in the log
//   help                        list the commands and the three vocabularies
//
// A REF is an entity name, optionally with a sub-element:
//
//   Line1        the whole entity
//   Line1.start  Line1.end  Circle1.center
//   Spline1.p3   a spline's Nth point, counted from 0 -- p0 and the last one
//                are the same references as .start and .end, and are written
//                that way, so one point never has two names
//
// Entities are named as they are created: the first line in a sketch is
// `Line1`, the second `Line2`, the first arc `Arc1`, and so on. Every command
// that creates something LOGS the names it made, so a script can be written by
// running it once and reading the log.
//
// VALUE is a number, or the name of a Parameter that already exists.

// One thing a command did, and which line said to do it.
//
// The line is kept APART from the text rather than printed into it, because the
// two callers want different things: a file run wants "line 12: tool circle",
// and a connection -- where every message is its own call and every line is
// line 1 -- wants "tool circle". Formatting in the interpreter forced the
// second one to un-format it again by string surgery, which is a transport
// knowing the interpreter's punctuation.
struct ScriptLogEntry {
    int line{0};
    std::string text;
};

struct ScriptOutcome {
    bool ok{false};
    // 1-based, and 0 when ok.
    int failedLine{0};
    // Always set, on success and failure alike, and WITHOUT a line prefix --
    // the caller adds one if a line number means anything where it is printing.
    std::string message;
    // One entry per command that did something, in order. This is how a script
    // author learns the names the interpreter gave their geometry.
    std::vector<ScriptLogEntry> log;
};

// A script's STATE, kept between calls.
//
// The current sketch, the current tool, a half-drawn spline, and the names
// given to the geometry so far. A file gives all of that to one call and never
// needs to think about it -- but a SOCKET does not: `tool line` arrives, then
// `click 0 0`, then `click 100 0`, each in its own message, and an interpreter
// that started fresh each time would answer "there is no sketch yet" to the
// second line of every session.
//
// So the session is the primitive and RunSketchScript is one line of code on
// top of it. Two interpreters -- one for files and one for connections --
// would be the same vocabulary twice, and this project has paid for that shape
// often enough.
class SketchScriptSession {
public:
    // `document` must outlive the session, and must already have a solver (and
    // a kernel, if the script pads or solves).
    explicit SketchScriptSession(PartDocument& document);
    ~SketchScriptSession();

    SketchScriptSession(const SketchScriptSession&) = delete;
    SketchScriptSession& operator=(const SketchScriptSession&) = delete;

    // Runs one or more lines. Line numbers in the outcome are counted from the
    // START OF THIS CALL, not from the start of the session: over a connection
    // there is no file for an absolute number to refer to.
    ScriptOutcome run(const std::string& text);

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Runs `text` against `document`, which must already have a solver (and a
// kernel, if the script pads or solves).
//
// STOPS AT THE FIRST FAILURE and says which line. A script that carried on
// past a refused command would build something the author did not write, and
// the surprise would arrive several commands later.
ScriptOutcome RunSketchScript(PartDocument& document, const std::string& text);

// The tool names a script may use, and the constraint and dimension kinds.
// Exported so the CLI's help text and the error messages are the same list --
// and so a test can check the list covers every tool the program has.
std::vector<std::string> ScriptToolNames();
std::vector<std::string> ScriptConstraintNames();
std::vector<std::string> ScriptDimensionNames();

} // namespace paramcad
