// M11.3 -- the DECISION half of the expression UI.
//
// Everything here is Qt-free, which is the point: a test can call it directly.
// What it deliberately CANNOT answer is whether any of it reaches the screen --
// that is the M6.14 defect, and the viewer smoke test
// (`--selftest --sample m11-expression`) is what answers it, by reading the
// property table widget.

#include "Core/Document/PartDocument.h"
#include "Core/Parameter/Parameter.h"
#include "Core/Body/Body.h"
#include "Core/Feature/EdgeDressFeatures.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/PocketFeature.h"
#include "Core/Feature/RevolveFeature.h"
#include "Core/Sketch/Sketch.h"
#include "Core/Sketch/SketchConstraint.h"
#include "Viewer/DocumentOutline.h"
#include "Viewer/PropertyEditing.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using namespace paramcad;

constexpr double kTol = 1e-9;

const PropertyRow* RowFor(const std::vector<PropertyRow>& rows, const std::string& label) {
    for (const PropertyRow& row : rows)
        if (row.label == label) return &row;
    return nullptr;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    for (char c : text) {
        if (c == '\n') {
            out.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    out.push_back(current);
    return out;
}

double ValueOf(const PartDocument& document, ObjectId id) {
    const Parameter* parameter = document.parameters().findById(id);
    return parameter != nullptr ? parameter->value() : 0.0;
}

} // namespace

// =============================================================================
// The rows a parameter offers
// =============================================================================

TEST(PropertyEditingTest, M11_UI_001_ALiteralParameterOffersAnEditableValueAndAnEmptyExpression) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();

    const DocumentOutline outline(document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(width);

    const PropertyRow* value = RowFor(rows, "Value");
    ASSERT_NE(value, nullptr);
    EXPECT_TRUE(value->editable);
    EXPECT_EQ(value->field, PropertyField::Value);
    EXPECT_EQ(value->unitLabel, "mm");

    // Present even when empty: a row that only appears once you know it exists
    // is not discoverable.
    const PropertyRow* expression = RowFor(rows, "Expression");
    ASSERT_NE(expression, nullptr) << "the Expression row is missing entirely";
    EXPECT_TRUE(expression->editable);
    EXPECT_EQ(expression->field, PropertyField::Expression);
    EXPECT_EQ(expression->value, "");
}

TEST(PropertyEditingTest, M11_UI_002_ADrivenParameterLocksItsValueRowAndShowsTheExpression) {
    PartDocument document{"Doc"};
    document.addParameter("Base", 40.0, UnitType::Millimeter);
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(width, "#Base / 2"));
    ASSERT_TRUE(document.recompute().success);

    const DocumentOutline outline(document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(width);

    const PropertyRow* value = RowFor(rows, "Value");
    ASSERT_NE(value, nullptr);
    // Typing here would silently delete the formula (ADR-M11-006).
    EXPECT_FALSE(value->editable);
    EXPECT_EQ(value->field, PropertyField::None);
    EXPECT_EQ(value->value.rfind("20", 0), 0u) << "the row shows " << value->value;

    const PropertyRow* expression = RowFor(rows, "Expression");
    ASSERT_NE(expression, nullptr);
    EXPECT_TRUE(expression->editable);
    EXPECT_EQ(expression->value, "#Base / 2");
}

TEST(PropertyEditingTest, M11_UI_003_AUnitWithNoExpressionDimensionOffersNoExpressionRow) {
    // Offering a row the facade will always refuse is an invitation to fail.
    PartDocument document{"Doc"};
    const ObjectId mass = document.addParameter("Mass", 1.0, UnitType::Kilogram).id();

    const DocumentOutline outline(document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(mass);
    EXPECT_EQ(RowFor(rows, "Expression"), nullptr);
    ASSERT_NE(RowFor(rows, "Value"), nullptr);
    EXPECT_TRUE(RowFor(rows, "Value")->editable);
}

// =============================================================================
// The caret rendering
// =============================================================================

TEST(PropertyEditingTest, M11_UI_020_TheCaretRowPointsAtTheOffendingCharacters) {
    // "#Base / #Nope" -- the reference starts at index 8 and spans 5
    // characters, which is what the parser reports.
    const ExpressionError error{ExpressionErrorCode::UnknownVariable, 8, 5,
                                "there is no parameter named 'Nope'"};
    const std::vector<std::string> lines = Lines(RenderExpressionError("#Base / #Nope", error));

    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "#Base / #Nope");
    EXPECT_EQ(lines[1], "        ^^^^^");
    EXPECT_EQ(lines[2].rfind("col 9:", 0), 0u) << lines[2];
}

TEST(PropertyEditingTest, M11_UI_021_AZeroLengthErrorStillGetsOneCaret) {
    const ExpressionError error{ExpressionErrorCode::UnexpectedEnd, 3, 0, "ends too soon"};
    const std::vector<std::string> lines = Lines(RenderExpressionError("1 +", error));
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[1], "   ^");
}

TEST(PropertyEditingTest, M11_UI_022_TabsAreRenderedAsSpacesSoTheCaretLinesUp) {
    // A caret that is confidently pointing at the wrong character is worse than
    // no caret at all.
    const ExpressionError error{ExpressionErrorCode::UnknownUnit, 2, 3, "not a unit"};
    const std::vector<std::string> lines = Lines(RenderExpressionError("1\tfoo", error));
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "1 foo");
    EXPECT_EQ(lines[1], "  ^^^");
}

TEST(PropertyEditingTest, M11_UI_023_AnOutOfRangePositionIsClampedNotTrusted) {
    const ExpressionError error{ExpressionErrorCode::TrailingInput, 99, 40, "past the end"};
    const std::vector<std::string> lines = Lines(RenderExpressionError("1 + 1", error));
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_LE(lines[1].size(), lines[0].size() + 1);
}

TEST(PropertyEditingTest, M11_UI_024_NoErrorRendersNothing) {
    EXPECT_EQ(RenderExpressionError("1 + 1", ExpressionError{}), "");
}

TEST(PropertyEditingTest, M11_UI_025_TheValueSourceIsNamedNotImplied) {
    EXPECT_NE(DescribeValueSource("#Base / 2").find("#Base / 2"), std::string::npos);
    EXPECT_NE(DescribeValueSource("").find("literal"), std::string::npos);
}

// =============================================================================
// ApplyPropertyEdit
// =============================================================================

TEST(PropertyEditingTest, M11_UI_040_TypingANumberIntoALiteralValueApplies) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();

    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(document, width, PropertyField::Value, "120");
    EXPECT_TRUE(outcome.applied);
    EXPECT_NE(outcome.status.find("Width"), std::string::npos) << outcome.status;
    EXPECT_TRUE(outcome.detail.empty());
    EXPECT_NEAR(ValueOf(document, width), 120.0, kTol);
}

TEST(PropertyEditingTest, M11_UI_041_SurroundingWhitespaceIsNotATypoTheUserMustHunt) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();
    EXPECT_TRUE(ApplyPropertyEdit(document, width, PropertyField::Value, "  7  ").applied);
    EXPECT_NEAR(ValueOf(document, width), 7.0, kTol);
}

TEST(PropertyEditingTest, M11_UI_042_PartialNumbersAreRefusedNotTruncated) {
    // "5abc" must not become 5. Silently discarding the tail is how a typo
    // becomes a dimension.
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();

    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(document, width, PropertyField::Value, "5abc");
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("not a number"), std::string::npos) << outcome.status;
    EXPECT_EQ(outcome.rejectedText, "5abc");
    EXPECT_NEAR(ValueOf(document, width), 100.0, kTol) << "a refusal changed the document";
}

TEST(PropertyEditingTest, M11_UI_043_ADrivenValueRefusesAPlainNumberAndSaysHow) {
    PartDocument document{"Doc"};
    document.addParameter("Base", 40.0, UnitType::Millimeter);
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(width, "#Base / 2"));

    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(document, width, PropertyField::Value, "5");
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("Expression"), std::string::npos) << outcome.status;
    EXPECT_EQ(document.parameters().findById(width)->expression(), "#Base / 2");
}

TEST(PropertyEditingTest, M11_UI_044_AValidExpressionApplies) {
    PartDocument document{"Doc"};
    document.addParameter("Base", 40.0, UnitType::Millimeter);
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();

    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(document, width, PropertyField::Expression, "#Base / 2");
    EXPECT_TRUE(outcome.applied);
    EXPECT_TRUE(outcome.detail.empty());
    ASSERT_TRUE(document.recompute().success);
    EXPECT_NEAR(ValueOf(document, width), 20.0, kTol);
}

TEST(PropertyEditingTest, M11_UI_045_ARefusedExpressionCarriesAColumnACaretAndTheTypedText) {
    PartDocument document{"Doc"};
    document.addParameter("Base", 40.0, UnitType::Millimeter);
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();

    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(document, width, PropertyField::Expression, "#Base / #Nope");
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("col "), std::string::npos) << outcome.status;
    EXPECT_NE(outcome.status.find("Nope"), std::string::npos) << outcome.status;

    // The caret rendering exists nowhere else -- it is the only place the
    // POSITION becomes something a user can see.
    const std::vector<std::string> lines = Lines(outcome.detail);
    ASSERT_EQ(lines.size(), 3u) << outcome.detail;
    EXPECT_EQ(lines[0], "#Base / #Nope");
    EXPECT_NE(lines[1].find('^'), std::string::npos);

    // And what was typed comes back, so one bad character does not cost the
    // whole expression.
    EXPECT_EQ(outcome.rejectedText, "#Base / #Nope");
    EXPECT_TRUE(document.parameters().findById(width)->expression().empty());
}

TEST(PropertyEditingTest, M11_UI_046_ClearingAnExpressionKeepsTheValue) {
    PartDocument document{"Doc"};
    document.addParameter("Base", 40.0, UnitType::Millimeter);
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();
    ASSERT_TRUE(document.setParameterExpression(width, "#Base / 2"));
    ASSERT_TRUE(document.recompute().success);
    ASSERT_NEAR(ValueOf(document, width), 20.0, kTol);

    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(document, width, PropertyField::Expression, "");
    EXPECT_TRUE(outcome.applied);
    EXPECT_NE(outcome.status.find("cleared"), std::string::npos) << outcome.status;
    EXPECT_NEAR(ValueOf(document, width), 20.0, kTol) << "clearing must not reset the value";

    // And the value row is typeable again.
    EXPECT_TRUE(ApplyPropertyEdit(document, width, PropertyField::Value, "33").applied);
    EXPECT_NEAR(ValueOf(document, width), 33.0, kTol);
}

TEST(PropertyEditingTest, M11_UI_047_ANonEditableRowIsRefusedRatherThanGuessedAt) {
    PartDocument document{"Doc"};
    const ObjectId width = document.addParameter("Width", 100.0, UnitType::Millimeter).id();
    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(document, width, PropertyField::None, "5");
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("not editable"), std::string::npos) << outcome.status;
}

TEST(PropertyEditingTest, M11_UI_048_AVanishedParameterIsReportedNotDereferenced) {
    PartDocument document{"Doc"};
    const ObjectId ghost = ObjectIdGenerator::Next();
    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(document, ghost, PropertyField::Value, "5");
    EXPECT_FALSE(outcome.applied);
    EXPECT_FALSE(outcome.status.empty());
}

TEST(PropertyEditingTest, M11_UI_049_TheStatusLineIsNeverSilent) {
    // Silence after an edit is indistinguishable from an edit that did nothing.
    PartDocument document{"Doc"};
    document.addParameter("Base", 40.0, UnitType::Millimeter);
    const ObjectId width = document.addParameter("Width", 0.0, UnitType::Millimeter).id();

    const PropertyField fields[] = {PropertyField::None, PropertyField::Value,
                                    PropertyField::Expression};
    const char* texts[] = {"", "5", "#Base", "not an expression $", "5abc"};
    for (PropertyField field : fields) {
        for (const char* text : texts) {
            const PropertyEditOutcome outcome =
                ApplyPropertyEdit(document, width, field, text);
            EXPECT_FALSE(outcome.status.empty())
                << "field " << static_cast<int>(field) << ", text '" << text << "'";
        }
    }
}

// --- M17: the row and the edit, joined up ------------------------------------
//
// Every test above hands ApplyPropertyEdit a field it chose ITSELF. The panel
// does not: it reads `field` off the PropertyRow that DocumentOutline built,
// and passes that. So a row built with the seven-argument aggregate -- leaving
// `field` at its default of None -- produced a cell that looked editable,
// accepted the typing, kept it on screen, and was answered "that row is not
// editable" by a decision layer no test ever reached with that field.
//
// That is exactly what shipped: a Pad whose Length could be retyped all day
// while the solid stayed the same height. Five rows had it -- Pad, Pocket,
// Fillet/Chamfer, Revolve, and every dimensional constraint.
//
// The rule these tests enforce is one sentence: AN EDITABLE ROW MUST CARRY A
// FIELD THAT CAN WRITE. They drive the row the way the panel drives it, so a
// regression here is a regression a user would see.

namespace {

// A document holding one of every parameter-backed feature, plus a dimensional
// constraint -- the full set of rows that write through a Parameter.
struct FeatureDoc {
    PartDocument document{"FeatureDoc"};
    ObjectId padId = kInvalidObjectId;
    ObjectId pocketId = kInvalidObjectId;
    ObjectId filletId = kInvalidObjectId;
    ObjectId revolveId = kInvalidObjectId;
    ObjectId constraintId = kInvalidObjectId;
    ObjectId lengthId = kInvalidObjectId;

    FeatureDoc() {
        Parameter& length = document.addParameter("PadLength", 20.0, UnitType::Millimeter);
        Parameter& depth = document.addParameter("PocketDepth", 5.0, UnitType::Millimeter);
        Parameter& radius = document.addParameter("FilletRadius", 2.0, UnitType::Millimeter);
        Parameter& angle = document.addParameter("RevolveAngle", 1.0, UnitType::Radian);
        Parameter& width = document.addParameter("Width", 60.0, UnitType::Millimeter);
        lengthId = length.id();

        Sketch& sketch = document.addSketch("Sketch001");
        const SketchEntityId axis = sketch.addLine(Vec2{0, 0}, Vec2{0, 50});
        const SketchEntityId edge = sketch.addLine(Vec2{0, 0}, Vec2{60, 0});
        constraintId = ToObjectId(document.addSketchConstraint(
            sketch.id(), DistanceConstraint{SketchElementRef{edge, SketchSubElement::StartPoint},
                                            SketchElementRef{edge, SketchSubElement::EndPoint},
                                            width.id()}));

        Body& body = document.addBody("Body001");
        padId = document.addPadFeature(body, "Pad001", sketch.id(), length.id()).id();
        pocketId =
            document.addPocketFeature(body, "Pocket001", padId, sketch.id(), depth.id()).id();
        filletId = document.addFilletFeature(body, "Fillet001", pocketId, radius.id()).id();
        revolveId =
            document.addRevolveFeature(body, "Revolve001", sketch.id(), axis, angle.id()).id();
    }
};

} // namespace

TEST(PropertyEditingTest, M17_UI_060_NoEditableRowIsBuiltWithoutAWritableField) {
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);

    std::vector<ObjectId> selectable{doc.padId,      doc.pocketId, doc.filletId,
                                     doc.revolveId,  doc.constraintId};
    for (const auto& parameter : doc.document.parameters().items())
        selectable.push_back(parameter->id());
    for (const Sketch* sketch : doc.document.sketches()) selectable.push_back(sketch->id());

    std::size_t editableRows = 0;
    for (ObjectId id : selectable) {
        for (const PropertyRow& row : outline.propertiesOf(id)) {
            if (!row.editable) continue;
            ++editableRows;
            // The three halves of a writable row. Any one missing makes the
            // cell a decoration: typing into it changes nothing.
            EXPECT_NE(row.field, PropertyField::None)
                << row.group << " / " << row.label << " is editable but writes nothing";
            EXPECT_NE(row.parameterId, kInvalidObjectId)
                << row.group << " / " << row.label << " is editable but has no target";
        }
    }
    // The loop above passes vacuously if propertiesOf returns nothing editable
    // at all -- which is precisely the shape of the bug it guards.
    EXPECT_GE(editableRows, 6u);
}

TEST(PropertyEditingTest, M17_UI_061_EveryFeatureDimensionRowActuallyWritesItsParameter) {
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);

    struct Case {
        ObjectId id;
        const char* label;
        const char* typed;
        double expected;
    };
    const Case cases[] = {
        {doc.padId, "Length", "35", 35.0},      {doc.pocketId, "Depth", "8", 8.0},
        {doc.filletId, "Radius", "3.5", 3.5},   {doc.revolveId, "Angle", "2", 2.0},
        {doc.constraintId, "Value", "75", 75.0},
    };

    for (const Case& c : cases) {
        const std::vector<PropertyRow> rows = outline.propertiesOf(c.id);
        const PropertyRow* row = RowFor(rows, c.label);
        ASSERT_NE(row, nullptr) << c.label;
        ASSERT_TRUE(row->editable) << c.label;

        // Driven the way the PANEL drives it: the row's own field, not one the
        // test picked. That is the whole point of this test.
        const PropertyEditOutcome outcome =
            ApplyPropertyEdit(doc.document, row->parameterId, row->field, c.typed);
        EXPECT_TRUE(outcome.applied) << c.label << ": " << outcome.status;

        const Parameter* parameter = doc.document.parameters().findById(row->parameterId);
        ASSERT_NE(parameter, nullptr);
        EXPECT_NEAR(parameter->value(), c.expected, kTol) << c.label;

        // And the panel now SHOWS the new number, because the row is rebuilt
        // from the document rather than from what was typed.
        // Held in a named vector: propertiesOf returns BY VALUE, so passing the
        // call straight into RowFor leaves the returned pointer dangling the
        // moment the full expression ends.
        const std::vector<PropertyRow> rebuilt = outline.propertiesOf(c.id);
        const PropertyRow* after = RowFor(rebuilt, c.label);
        ASSERT_NE(after, nullptr);
        EXPECT_NE(after->value.find(c.typed), std::string::npos)
            << c.label << " still reads " << after->value;
    }
}

// --- M17.8: direction is the SIGN, shown as a checkbox -----------------------

TEST(PropertyEditingTest, M17_UI_070_PadAndPocketOfferAReversedRowOverTheirOwnParameter) {
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);

    for (ObjectId id : {doc.padId, doc.pocketId}) {
        const std::vector<PropertyRow> rows = outline.propertiesOf(id);
        const PropertyRow* reversed = RowFor(rows, "Reversed");
        ASSERT_NE(reversed, nullptr);
        EXPECT_TRUE(reversed->editable);
        EXPECT_EQ(reversed->field, PropertyField::Reversed);
        // It writes THE SAME parameter the size row writes. A separate id would
        // mean a second stored fact about one direction, and the two would
        // eventually disagree.
        const PropertyRow* size = RowFor(rows, "Length");
        if (size == nullptr) size = RowFor(rows, "Depth");
        ASSERT_NE(size, nullptr);
        EXPECT_EQ(reversed->parameterId, size->parameterId);
    }
}

TEST(PropertyEditingTest, M17_UI_071_TickingReversedFlipsTheSignAndKeepsTheSize) {
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.padId);
    const PropertyRow* reversed = RowFor(rows, "Reversed");
    ASSERT_NE(reversed, nullptr);

    // 20 mm, forward. Ticking the box must not change how DEEP it goes.
    PropertyEditOutcome outcome =
        ApplyPropertyEdit(doc.document, reversed->parameterId, reversed->field, "1");
    EXPECT_TRUE(outcome.applied) << outcome.status;
    const Parameter* parameter = doc.document.parameters().findById(reversed->parameterId);
    ASSERT_NE(parameter, nullptr);
    EXPECT_NEAR(parameter->value(), -20.0, kTol);

    // And back. Un-ticking is not "add 20" -- it is "make it positive".
    outcome = ApplyPropertyEdit(doc.document, reversed->parameterId, reversed->field, "0");
    EXPECT_TRUE(outcome.applied) << outcome.status;
    EXPECT_NEAR(parameter->value(), 20.0, kTol);
}

TEST(PropertyEditingTest, M17_UI_072_TheRowREADSTheSignRatherThanRememberingIt) {
    // The whole reason this is a view of the sign and not a stored flag: a user
    // who types -20 into the Length row must find the box already ticked. A
    // separate flag would still say "forward" over a negative length, and the
    // feature would point the way neither of them asked for.
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);
    // NAMED, because propertiesOf returns BY VALUE: passing the call straight
    // into RowFor leaves the pointer dangling at the end of the expression.
    const std::vector<PropertyRow> initial = outline.propertiesOf(doc.padId);
    const PropertyRow* before = RowFor(initial, "Length");
    ASSERT_NE(before, nullptr);
    const ObjectId lengthId = before->parameterId;

    ASSERT_TRUE(ApplyPropertyEdit(doc.document, lengthId, PropertyField::Value, "-20").applied);

    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.padId);
    const PropertyRow* reversed = RowFor(rows, "Reversed");
    ASSERT_NE(reversed, nullptr);
    EXPECT_LT(reversed->numericValue, 0.0) << "the checkbox would show forward";
    EXPECT_EQ(reversed->value, "yes");
}

TEST(PropertyEditingTest, M17_UI_073_ZeroHasNoDirectionAndSaysSo) {
    // Flipping the sign of zero leaves the feature exactly as broken and gives
    // the user a ticked box as the only sign anything happened.
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.padId);
    const PropertyRow* reversed = RowFor(rows, "Reversed");
    ASSERT_NE(reversed, nullptr);
    ASSERT_TRUE(doc.document.setParameterValue(reversed->parameterId, 0.0));

    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(doc.document, reversed->parameterId, reversed->field, "1");
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("no direction"), std::string::npos) << outcome.status;
}

TEST(PropertyEditingTest, M17_UI_074_ADrivenValuesDirectionIsRefusedNotSilentlyNegated) {
    // Negating an expression's result would leave the panel and the formula
    // saying different things -- the same rule the Value row already follows.
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.padId);
    const PropertyRow* reversed = RowFor(rows, "Reversed");
    ASSERT_NE(reversed, nullptr);
    doc.document.addParameter("Base", 12.0, UnitType::Millimeter);
    ASSERT_TRUE(doc.document.setParameterExpression(reversed->parameterId, "#Base"));

    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(doc.document, reversed->parameterId, reversed->field, "1");
    EXPECT_FALSE(outcome.applied);
    EXPECT_FALSE(outcome.status.empty());
    const Parameter* parameter = doc.document.parameters().findById(reversed->parameterId);
    ASSERT_NE(parameter, nullptr);
    // The EXPRESSION is intact and the value was not negated. Checked as two
    // claims, because setParameterValue clears an expression as its first act
    // (ADR-M11-006) -- so a refusal that leaked through would show up here as a
    // formula that quietly vanished, not only as a changed number.
    EXPECT_EQ(parameter->expression(), "#Base");
    EXPECT_GT(parameter->value(), 0.0) << "the driven value was negated anyway";
}

// --- M17.16: renaming, through the row the panel actually uses ---------------

TEST(PropertyEditingTest, M17_UI_080_RenamingAFeatureWorksAndIsOneUndoStep) {
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.padId);
    const PropertyRow* name = RowFor(rows, "Name");
    ASSERT_NE(name, nullptr);

    const std::size_t before = doc.document.undoDepth();
    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(doc.document, name->parameterId, name->field, "Base plate");
    EXPECT_TRUE(outcome.applied) << outcome.status;
    EXPECT_EQ(doc.document.objectName(doc.padId), "Base plate");

    // ONE step, and it goes back. A rename nobody can undo is a typo that
    // survives everything after it.
    EXPECT_EQ(doc.document.undoDepth(), before + 1);
    ASSERT_TRUE(doc.document.undo());
    EXPECT_EQ(doc.document.objectName(doc.padId), "Pad001");
}

TEST(PropertyEditingTest, M17_UI_081_ADuplicateNameIsREFUSED) {
    // Names are how a user picks what to delete (ADR-M17-038) -- and for a
    // PARAMETER a duplicate is a correctness bug, because expressions resolve
    // by name and findByName answers with the first match. Renaming is the one
    // way left to create one, so it is the one place that has to refuse.
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.padId);
    const PropertyRow* name = RowFor(rows, "Name");
    ASSERT_NE(name, nullptr);

    const PropertyEditOutcome outcome =
        ApplyPropertyEdit(doc.document, name->parameterId, name->field, "Pocket001");
    EXPECT_FALSE(outcome.applied);
    EXPECT_NE(outcome.status.find("already taken"), std::string::npos) << outcome.status;
    EXPECT_EQ(doc.document.objectName(doc.padId), "Pad001");
}

TEST(PropertyEditingTest, M17_UI_082_AnEmptyOrWhitespaceNameIsREFUSED) {
    // A blank row in the tree names nothing, and a name made of spaces is one
    // a user cannot tell from another.
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.padId);
    const PropertyRow* name = RowFor(rows, "Name");
    ASSERT_NE(name, nullptr);

    for (const char* blank : {"", "   ", "\t"}) {
        const PropertyEditOutcome outcome =
            ApplyPropertyEdit(doc.document, name->parameterId, name->field, blank);
        EXPECT_FALSE(outcome.applied) << "accepted '" << blank << "'";
        EXPECT_FALSE(outcome.status.empty());
    }
    EXPECT_EQ(doc.document.objectName(doc.padId), "Pad001");
}

TEST(PropertyEditingTest, M17_UI_083_SurroundingSpaceIsTrimmedNotStored) {
    // "Pad001 " and "Pad001" are indistinguishable in the tree, so storing the
    // first would let a user make two rows that look identical after all the
    // work of making names unique.
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);
    const std::vector<PropertyRow> rows = outline.propertiesOf(doc.padId);
    const PropertyRow* name = RowFor(rows, "Name");
    ASSERT_NE(name, nullptr);

    ASSERT_TRUE(ApplyPropertyEdit(doc.document, name->parameterId, name->field, "  Boss  ")
                    .applied);
    EXPECT_EQ(doc.document.objectName(doc.padId), "Boss");
}

TEST(PropertyEditingTest, M17_UI_084_RenamingASketchAndAParameterUsesTheSameRow) {
    // One row kind for every object that has a name. A per-kind rename would
    // be five copies of one idea, and the fifth would be the one nobody added.
    FeatureDoc doc;
    const DocumentOutline outline(doc.document);
    for (const Sketch* sketch : doc.document.sketches()) {
        const std::vector<PropertyRow> rows = outline.propertiesOf(sketch->id());
        const PropertyRow* name = RowFor(rows, "Name");
        ASSERT_NE(name, nullptr);
        EXPECT_TRUE(ApplyPropertyEdit(doc.document, name->parameterId, name->field, "Outline")
                        .applied);
        EXPECT_EQ(doc.document.objectName(sketch->id()), "Outline");
    }
    const Parameter* first = doc.document.parameters().items().front().get();
    const std::vector<PropertyRow> rows = outline.propertiesOf(first->id());
    const PropertyRow* name = RowFor(rows, "Name");
    ASSERT_NE(name, nullptr);
    EXPECT_TRUE(ApplyPropertyEdit(doc.document, name->parameterId, name->field, "Thickness")
                    .applied);
    EXPECT_EQ(doc.document.objectName(first->id()), "Thickness");
}
