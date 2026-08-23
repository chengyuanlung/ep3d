#include "Viewer/PropertyEditing.h"

#include <cmath>

#include "Core/Document/PartDocument.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Sketch/SketchConstraint.h"
#include "Core/Sketch/Sketch.h"

#include <cstdlib>
#include <string>

namespace paramcad {
namespace {

constexpr const char* kWhitespace = " \t\r\n";

// Trim for the "did the user type anything?" question only. The text handed to
// the facade for an EXPRESSION is the ORIGINAL, because roadmap 42.3.2 requires
// the stored expression to be what the user wrote.
bool IsBlank(const std::string& text) {
    return text.find_first_not_of(kWhitespace) == std::string::npos;
}

} // namespace

std::string RenderExpressionError(const std::string& text, const ExpressionError& error) {
    if (!error.isError()) return {};

    // Tabs become one space each so the caret row lines up with what a
    // monospace tooltip actually draws. Without this a single tab shifted every
    // caret after it and pointed at the wrong character -- which is worse than
    // no caret, because it is confidently wrong.
    std::string shown;
    shown.reserve(text.size());
    for (char c : text) shown.push_back(c == '\t' ? ' ' : c);

    // Clamp rather than trust: an error position past the end of the text would
    // otherwise build a caret row longer than the line it points at.
    const std::size_t start = error.position <= shown.size() ? error.position : shown.size();
    std::size_t span = error.length == 0 ? 1 : error.length;
    if (start + span > shown.size()) span = shown.size() > start ? shown.size() - start : 1;

    std::string caret(start, ' ');
    caret.append(span, '^');

    return shown + "\n" + caret + "\n" + DescribeExpressionError(error);
}

std::string DescribeValueSource(const std::string& expression) {
    if (expression.empty()) return "A literal value. Type a number to change it.";
    return "Driven by: " + expression + "\nEdit the Expression row to change it.";
}

PropertyEditOutcome ApplyPropertyEdit(PartDocument& document, ObjectId parameterId,
                                      PropertyField field, const std::string& text) {
    PropertyEditOutcome outcome;
    outcome.rejectedText = text;

    // The NAME row is answered FIRST, because its id is an OBJECT's, not a
    // parameter's -- looking it up in the parameter table would refuse every
    // rename of a sketch or a feature with "that parameter no longer exists"
    // (M17.16).
    if (field == PropertyField::Name) {
        const PartDocument::RenameResult renamed = document.renameObject(parameterId, text);
        if (!renamed.ok) {
            outcome.status = renamed.message;
            return outcome;
        }
        outcome.applied = true;
        outcome.status = "Renamed to " + document.objectName(parameterId);
        outcome.rejectedText.clear();
        return outcome;
    }

    const Parameter* parameter = document.parameters().findById(parameterId);
    if (parameter == nullptr) {
        outcome.status = "that parameter no longer exists";
        return outcome;
    }
    const std::string name = parameter->name();

    if (field == PropertyField::Value) {
        // A REFERENCE dimension's number is derived (M17.19, ADR-M17-042):
        // typing into it would be overwritten by the next recompute, which is
        // the worst kind of editable field -- it accepts the value, looks
        // changed, and reverts without saying why.
        for (const Sketch* sketch : document.sketches())
            for (const SketchConstraint& constraint : sketch->constraints()) {
                if (!constraint.driven) continue;
                if (BoundParameterId(constraint.data) != parameterId) continue;
                outcome.status = name + " is a reference dimension: it measures the geometry, "
                                        "so its value cannot be typed. Make it drive first.";
                return outcome;
            }

        // A driven value is not editable in the panel, and the row is rendered
        // read-only -- but the rule is enforced HERE too, not only by a widget
        // flag. A flag is presentation; this is the decision.
        if (!parameter->expression().empty()) {
            outcome.status = name + " is driven by an expression; clear the Expression "
                                    "row to type a value";
            return outcome;
        }
        // Surrounding whitespace is not a typo the user should have to hunt
        // for: a value pasted from anywhere arrives with some.
        const std::size_t first = text.find_first_not_of(kWhitespace);
        const std::size_t last = text.find_last_not_of(kWhitespace);
        const std::string trimmed =
            first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
        char* end = nullptr;
        const double value = std::strtod(trimmed.c_str(), &end);
        const bool consumedAll = end != nullptr && *end == '\0' && !trimmed.empty();
        if (!consumedAll || IsBlank(trimmed)) {
            outcome.status = "'" + text + "' is not a number";
            return outcome;
        }
        if (!document.setParameterValue(parameterId, value)) {
            outcome.status = "the document refused that value";
            return outcome;
        }
        outcome.applied = true;
        outcome.status = name + " = " + text;
        outcome.rejectedText.clear();
        return outcome;
    }

    if (field == PropertyField::Reversed) {
        // A driven value is not flipped here, for the same reason it is not
        // typed over: the expression is what decides the number, and silently
        // negating its result would leave the panel and the formula saying
        // different things.
        if (!parameter->expression().empty()) {
            outcome.status = name + " is driven by an expression; the direction is whatever "
                                    "that expression works out to";
            return outcome;
        }
        const bool wantReversed = text == "1" || text == "true" || text == "yes";
        const double magnitude = std::fabs(parameter->value());
        if (magnitude <= 0.0) {
            // Zero has no side. Flipping it would leave the feature just as
            // broken and give the user a ticked box as the only sign anything
            // happened.
            outcome.status = name + " is zero, which has no direction -- give it a size first";
            return outcome;
        }
        const double wanted = wantReversed ? -magnitude : magnitude;
        if (!document.setParameterValue(parameterId, wanted)) {
            outcome.status = "the document refused that direction";
            return outcome;
        }
        outcome.applied = true;
        outcome.status = name + (wantReversed ? " reversed" : " back to its normal direction");
        outcome.rejectedText.clear();
        return outcome;
    }

    if (field == PropertyField::Expression) {
        if (IsBlank(text)) {
            const bool had = !parameter->expression().empty();
            if (!document.setParameterExpression(parameterId, std::string{})) {
                outcome.status = "the document refused to clear that expression";
                return outcome;
            }
            outcome.applied = true;
            outcome.status = had ? name + ": expression cleared, value kept"
                                 : name + " has no expression";
            outcome.rejectedText.clear();
            return outcome;
        }

        ExpressionError error;
        if (!document.setParameterExpression(parameterId, text, &error)) {
            // The message the FACADE produced, not one invented here. Two
            // sources of truth for why an edit failed is how a UI ends up
            // saying something the model never said.
            outcome.status = error.isError() ? DescribeExpressionError(error)
                                             : std::string("that expression was refused");
            outcome.detail = RenderExpressionError(text, error);
            return outcome;
        }
        outcome.applied = true;
        outcome.status = name + " = " + text;
        outcome.rejectedText.clear();
        return outcome;
    }

    outcome.status = "that row is not editable";
    return outcome;
}

} // namespace paramcad
