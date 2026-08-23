#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Expression/ExpressionTypes.h"
#include "Viewer/DocumentOutline.h"

#include <string>

namespace paramcad {

class PartDocument;

// The DECISION half of a property-panel edit (M11.3), deliberately free of Qt.
//
// M6.14 is the reason this file exists: a property panel shipped showing ten
// labels and no values, and none of 561 tests could see it, because all of them
// asked the model and none asked the widget. The lesson taken here is the one
// that generalises -- keep everything that DECIDES in a layer a test can call
// directly, and leave the widget with nothing but rendering. What remains in
// MainWindow after this is item text, tooltips and a status line; what a test
// then still has to check at the widget is exactly that, and the smoke test
// does.

struct PropertyEditOutcome {
    // Whether the document changed. False for every refusal, and refusals leave
    // the document byte-for-byte unchanged (the facade guarantees it).
    bool applied{false};

    // One line, for the status bar. Always populated -- silence after an edit
    // is indistinguishable from an edit that did nothing.
    std::string status;

    // Multi-line, for a tooltip on the offending cell: the text the user typed,
    // a caret row under the offending characters, and the message. Empty on
    // success.
    std::string detail;

    // What the user typed, kept so the panel can put it BACK in the cell after
    // a refusal instead of replacing it with the stored value. Losing a typo is
    // worse than showing one: the user has to retype the whole expression to
    // fix one character.
    std::string rejectedText;
};

// Apply one committed cell edit through the document facade.
//
// `field` decides how `text` is read -- as a plain number, or as an expression.
// Never writes into a Parameter directly; every path goes through
// PartDocument, so the validation, the dependency edges and the undo record all
// happen where they are defined (UI spec 20).
PropertyEditOutcome ApplyPropertyEdit(PartDocument& document, ObjectId parameterId,
                                      PropertyField field, const std::string& text);

// The text a user typed, a caret row under the characters at fault, and the
// message -- three lines a monospace tooltip can show:
//
//     #Width / #Nope
//              ^^^^^
//     col 10: there is no parameter named 'Nope'
//
// Tabs in the source are rendered as single spaces, so the caret row lines up
// with what is actually displayed rather than with the byte offsets.
std::string RenderExpressionError(const std::string& text, const ExpressionError& error);

// One line describing what a parameter's value currently comes from, for the
// value row's tooltip: "driven by: #Width / 2", or a plain statement that it is
// a literal. A number whose source is invisible is a number a user cannot
// trust.
std::string DescribeValueSource(const std::string& expression);

} // namespace paramcad
