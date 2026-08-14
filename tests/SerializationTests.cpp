#include "Core/Document/PartDocument.h"
#include "Core/Feature/PlaceholderFeature.h"
#include "Core/Recompute/IRecomputable.h"
#include "Core/Serialization/JsonValue.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using namespace paramcad;

std::string saveToString(const PartDocument& document) {
    std::ostringstream out;
    const SaveResult result = savePartDocument(document, out);
    EXPECT_TRUE(result) << result.message;
    return out.str();
}

LoadResult loadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadPartDocument(in);
}

// A minimal well-formed schema v1 document used as the base for the
// error-path tests below.
constexpr const char* kMinimalDocument = R"({
  "format": "ParametricCAD",
  "schemaVersion": 1,
  "documentType": "Part",
  "id": "4001",
  "name": "Minimal",
  "parameters": [],
  "bodies": []
})";

TEST(SerializationTests, RoundTripPreservesAllFields) {
    PartDocument original("RoundTrip");
    Parameter& width = original.addParameter("Width", 120.0, UnitType::Millimeter);
    Parameter& depth = original.addParameter("Depth", 60.5, UnitType::Millimeter);
    depth.setExpression("Width / 2"); // round-trips as data; also sets state Dirty
    ASSERT_EQ(depth.state(), ParameterState::Dirty);

    Body& body = original.addBody("Body001");
    PlaceholderFeature& pad = body.addFeature<PlaceholderFeature>("Pad001", "Placeholder");
    PlaceholderFeature& pocket = body.addFeature<PlaceholderFeature>("Pocket001", "Placeholder");
    ASSERT_TRUE(pad.recompute()); // pad Valid, pocket stays Dirty
    ASSERT_EQ(pad.state(), ComputeState::Valid);
    ASSERT_EQ(pocket.state(), ComputeState::Dirty);

    const std::string firstSave = saveToString(original);
    const LoadResult loaded = loadFromString(firstSave);
    ASSERT_TRUE(loaded) << loaded.message;
    const PartDocument& copy = *loaded.document;

    // Document header.
    EXPECT_EQ(copy.id(), original.id());
    EXPECT_EQ(copy.name(), "RoundTrip");

    // Parameters, field by field, in order.
    ASSERT_EQ(copy.parameters().items().size(), 2u);
    const Parameter& widthCopy = *copy.parameters().items()[0];
    EXPECT_EQ(widthCopy.id(), width.id());
    EXPECT_EQ(widthCopy.name(), "Width");
    EXPECT_EQ(widthCopy.value(), 120.0);
    EXPECT_EQ(widthCopy.unit(), UnitType::Millimeter);
    EXPECT_EQ(widthCopy.expression(), "");
    EXPECT_EQ(widthCopy.state(), ParameterState::Valid);
    const Parameter& depthCopy = *copy.parameters().items()[1];
    EXPECT_EQ(depthCopy.id(), depth.id());
    EXPECT_EQ(depthCopy.name(), "Depth");
    EXPECT_EQ(depthCopy.value(), 60.5);
    EXPECT_EQ(depthCopy.unit(), UnitType::Millimeter);
    EXPECT_EQ(depthCopy.expression(), "Width / 2");
    EXPECT_EQ(depthCopy.state(), ParameterState::Dirty);

    // Bodies and features.
    ASSERT_EQ(copy.bodies().size(), 1u);
    const Body& bodyCopy = *copy.bodies()[0];
    EXPECT_EQ(bodyCopy.id(), body.id());
    EXPECT_EQ(bodyCopy.name(), "Body001");
    ASSERT_EQ(bodyCopy.features().size(), 2u);
    const auto* padCopy = dynamic_cast<const PlaceholderFeature*>(bodyCopy.features()[0].get());
    ASSERT_NE(padCopy, nullptr);
    EXPECT_EQ(padCopy->id(), pad.id());
    EXPECT_EQ(padCopy->name(), "Pad001");
    EXPECT_EQ(padCopy->typeName(), "Placeholder");
    EXPECT_EQ(padCopy->state(), ComputeState::Valid);
    const auto* pocketCopy = dynamic_cast<const PlaceholderFeature*>(bodyCopy.features()[1].get());
    ASSERT_NE(pocketCopy, nullptr);
    EXPECT_EQ(pocketCopy->id(), pocket.id());
    EXPECT_EQ(pocketCopy->name(), "Pocket001");
    EXPECT_EQ(pocketCopy->typeName(), "Placeholder");
    EXPECT_EQ(pocketCopy->state(), ComputeState::Dirty);

    // Canonical form: saving the loaded document is byte-identical.
    EXPECT_EQ(saveToString(copy), firstSave);
}

TEST(SerializationTests, EmptyDocumentRoundTrip) {
    PartDocument original("Empty");
    const std::string firstSave = saveToString(original);

    const LoadResult loaded = loadFromString(firstSave);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->id(), original.id());
    EXPECT_EQ(loaded.document->name(), "Empty");
    EXPECT_TRUE(loaded.document->parameters().items().empty());
    EXPECT_TRUE(loaded.document->bodies().empty());
    EXPECT_EQ(saveToString(*loaded.document), firstSave);
}

TEST(SerializationTests, LoadFromNonexistentFileReportsFileNotFound) {
    const LoadResult result =
        loadPartDocumentFromFile("Z:\\definitely\\missing\\no_such_document.pcad.json");
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::FileNotFound);
    EXPECT_EQ(result.document, nullptr);
}

TEST(SerializationTests, MalformedJsonReportsLineAndColumn) {
    const LoadResult result = loadFromString("{\n  \"format\": \"ParametricCAD\",\n  oops\n}");
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::MalformedJson);
    EXPECT_EQ(result.document, nullptr);
    EXPECT_NE(result.message.find("line 3"), std::string::npos) << result.message;
    EXPECT_NE(result.message.find("column"), std::string::npos) << result.message;
}

TEST(SerializationTests, HeaderValidationErrors) {
    std::string wrongFormat = kMinimalDocument;
    wrongFormat.replace(wrongFormat.find("ParametricCAD"), 13, "OtherProgram1");
    LoadResult result = loadFromString(wrongFormat);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::WrongFormat);
    EXPECT_EQ(result.document, nullptr);

    std::string futureVersion = kMinimalDocument;
    futureVersion.replace(futureVersion.find("\"schemaVersion\": 1"), 18, "\"schemaVersion\": 9");
    result = loadFromString(futureVersion);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::UnsupportedSchemaVersion);
    EXPECT_EQ(result.document, nullptr);

    std::string assembly = kMinimalDocument;
    assembly.replace(assembly.find("\"Part\""), 6, "\"Assembly\"");
    result = loadFromString(assembly);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::WrongDocumentType);
    EXPECT_EQ(result.document, nullptr);
}

TEST(SerializationTests, FieldValidationErrors) {
    // Missing required field: parameter without "value".
    const std::string missingValue = R"({
      "format": "ParametricCAD", "schemaVersion": 1, "documentType": "Part",
      "id": "4002", "name": "Broken",
      "parameters": [ {"id": "4003", "name": "Width", "unit": "Millimeter",
                       "expression": "", "state": "Valid"} ],
      "bodies": []
    })";
    LoadResult result = loadFromString(missingValue);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::MissingField);
    EXPECT_EQ(result.document, nullptr);
    EXPECT_NE(result.message.find("value"), std::string::npos) << result.message;

    // Wrong JSON type: "value" as string.
    const std::string valueAsString = R"({
      "format": "ParametricCAD", "schemaVersion": 1, "documentType": "Part",
      "id": "4002", "name": "Broken",
      "parameters": [ {"id": "4003", "name": "Width", "value": "120",
                       "unit": "Millimeter", "expression": "", "state": "Valid"} ],
      "bodies": []
    })";
    result = loadFromString(valueAsString);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::InvalidFieldType);
    EXPECT_EQ(result.document, nullptr);

    // Unrecognized enum string.
    const std::string badUnit = R"({
      "format": "ParametricCAD", "schemaVersion": 1, "documentType": "Part",
      "id": "4002", "name": "Broken",
      "parameters": [ {"id": "4003", "name": "Width", "value": 120.0,
                       "unit": "Parsec", "expression": "", "state": "Valid"} ],
      "bodies": []
    })";
    result = loadFromString(badUnit);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::InvalidEnumValue);
    EXPECT_EQ(result.document, nullptr);
}

TEST(SerializationTests, UnknownKeysAreIgnored) {
    const std::string withExtras = R"({
      "format": "ParametricCAD", "schemaVersion": 1, "documentType": "Part",
      "id": "4010", "name": "Tolerant",
      "futureDocumentField": {"nested": [1, 2, 3]},
      "parameters": [ {"id": "4011", "name": "Width", "value": 10.0,
                       "unit": "Millimeter", "expression": "", "state": "Valid",
                       "futureParameterField": true} ],
      "bodies": [ {"id": "4012", "name": "Body001",
                   "features": [ {"id": "4013", "name": "Pad001", "type": "Placeholder",
                                  "state": "Dirty", "futureFeatureField": "ignored"} ]} ]
    })";
    const LoadResult result = loadFromString(withExtras);
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(result.document->id(), 4010u);
    ASSERT_EQ(result.document->parameters().items().size(), 1u);
    EXPECT_EQ(result.document->parameters().items()[0]->id(), 4011u);
    ASSERT_EQ(result.document->bodies().size(), 1u);
    EXPECT_EQ(result.document->bodies()[0]->id(), 4012u);
    ASSERT_EQ(result.document->bodies()[0]->features().size(), 1u);
    EXPECT_EQ(result.document->bodies()[0]->features()[0]->id(), 4013u);
}

TEST(SerializationTests, LoadedIdsNeverCollideWithNewObjects) {
    PartDocument original("Collision");
    original.addParameter("Width", 10.0, UnitType::Millimeter);
    Body& body = original.addBody("Body001");
    body.addFeature<PlaceholderFeature>("Pad001", "Placeholder");

    const std::string saved = saveToString(original);
    const LoadResult loaded = loadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    PartDocument& copy = *loaded.document;

    ObjectId maxLoadedId = copy.id();
    for (const auto& parameter : copy.parameters().items())
        maxLoadedId = std::max(maxLoadedId, parameter->id());
    for (const auto& loadedBody : copy.bodies()) {
        maxLoadedId = std::max(maxLoadedId, loadedBody->id());
        for (const auto& feature : loadedBody->features())
            maxLoadedId = std::max(maxLoadedId, feature->id());
    }

    // Every object created after the load gets an id strictly greater than
    // anything in the file.
    Parameter& newParameter = copy.addParameter("Depth", 5.0, UnitType::Millimeter);
    Body& newBody = copy.addBody("Body002");
    Feature& newFeature = newBody.addFeature<PlaceholderFeature>("Pad002", "Placeholder");
    EXPECT_GT(newParameter.id(), maxLoadedId);
    EXPECT_GT(newBody.id(), maxLoadedId);
    EXPECT_GT(newFeature.id(), maxLoadedId);
}

TEST(SerializationTests, AdvancePastSemantics) {
    // Advancing past a value below the counter is a no-op: ids stay sequential.
    const ObjectId before = ObjectIdGenerator::Next();
    ObjectIdGenerator::AdvancePast(before / 2);
    const ObjectId unaffected = ObjectIdGenerator::Next();
    EXPECT_EQ(unaffected, before + 1);

    // Advancing past a higher value moves the counter beyond it.
    const ObjectId target = unaffected + 1000;
    ObjectIdGenerator::AdvancePast(target);
    const ObjectId advanced = ObjectIdGenerator::Next();
    EXPECT_GT(advanced, target);
}

TEST(SerializationTests, LargeIdRoundTripsExactly) {
    // 2^53 + 1 is not representable as a double; the decimal-string encoding
    // must still round-trip it bit-exactly.
    const ObjectId largeId = (1ull << 53) + 1; // 9007199254740993
    const std::string text = R"({
      "format": "ParametricCAD", "schemaVersion": 1, "documentType": "Part",
      "id": "9007199254740993", "name": "Large",
      "parameters": [],
      "bodies": []
    })";
    const LoadResult loaded = loadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->id(), largeId);

    const std::string saved = saveToString(*loaded.document);
    EXPECT_NE(saved.find("\"9007199254740993\""), std::string::npos);
    const LoadResult reloaded = loadFromString(saved);
    ASSERT_TRUE(reloaded) << reloaded.message;
    EXPECT_EQ(reloaded.document->id(), largeId);
}

TEST(SerializationTests, SuppressedFeatureStateRoundTrips) {
    PartDocument original("Suppress");
    Body& body = original.addBody("Body001");
    PlaceholderFeature& pad = body.addFeature<PlaceholderFeature>("Pad001", "Placeholder");
    pad.setSuppressed(true);
    ASSERT_EQ(pad.state(), ComputeState::Suppressed);

    const LoadResult loaded = loadFromString(saveToString(original));
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->bodies().size(), 1u);
    ASSERT_EQ(loaded.document->bodies()[0]->features().size(), 1u);
    EXPECT_EQ(loaded.document->bodies()[0]->features()[0]->state(), ComputeState::Suppressed);
}

TEST(SerializationTests, FileRoundTripThroughRealFile) {
    PartDocument original("FileRoundTrip");
    original.addParameter("Width", 42.0, UnitType::Millimeter);

    const ::testing::TestInfo* info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string path = std::string(info->name()) + ".pcad.json";
    const SaveResult saved = savePartDocumentToFile(original, path);
    ASSERT_TRUE(saved) << saved.message;

    const LoadResult loaded = loadPartDocumentFromFile(path);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->id(), original.id());
    EXPECT_EQ(loaded.document->name(), "FileRoundTrip");
    ASSERT_EQ(loaded.document->parameters().items().size(), 1u);
    EXPECT_EQ(loaded.document->parameters().items()[0]->name(), "Width");
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Edge-case tests added by TEST agent verification pass.
// ---------------------------------------------------------------------------

// --- JSON parser ------------------------------------------------------------

TEST(SerializationTests, ParserDepthGuardReturnsStructuredError) {
    // 5000 nested arrays must hit the depth guard as a structured parse error,
    // not a stack overflow.
    const std::string deep(5000, '[');
    JsonParseError err;
    const JsonValue value = parseJson(deep, err);
    EXPECT_FALSE(err.ok);
    EXPECT_NE(err.message.find("nesting too deep"), std::string::npos) << err.message;
    EXPECT_TRUE(value.isNull());

    // Nesting comfortably under the limit still parses.
    const std::string shallow = std::string(200, '[') + std::string(200, ']');
    const JsonValue ok = parseJson(shallow, err);
    EXPECT_TRUE(err.ok) << err.message;
    EXPECT_EQ(ok.type(), JsonType::Array);
}

TEST(SerializationTests, ParserDuplicateKeysLastWinsDeterministically) {
    JsonParseError err;
    const JsonValue value = parseJson(R"({"a": 1, "a": 2, "b": 3})", err);
    ASSERT_TRUE(err.ok) << err.message;
    // set() replaces in place: one member per key, last value wins, first
    // insertion position kept -- deterministic.
    ASSERT_EQ(value.members().size(), 2u);
    EXPECT_EQ(value.members()[0].first, "a");
    EXPECT_EQ(value.members()[0].second.asNumber(), 2.0);
    EXPECT_EQ(value.members()[1].first, "b");
    EXPECT_EQ(writeJson(value), writeJson(parseJson(R"({"a": 2, "b": 3})", err)));
}

TEST(SerializationTests, ParserOutOfRangeNumberRejected) {
    JsonParseError err;
    parseJson("1e309", err); // > DBL_MAX
    EXPECT_FALSE(err.ok);
    parseJson("-1e309", err);
    EXPECT_FALSE(err.ok);

    // Through the serializer: a parameter value of 1e309 is MalformedJson,
    // not silently clamped to infinity.
    std::string text = kMinimalDocument;
    text.replace(text.find("\"parameters\": []"), 16,
                 "\"parameters\": [ {\"id\": \"4003\", \"name\": \"W\", \"value\": 1e309, "
                 "\"unit\": \"Millimeter\", \"expression\": \"\", \"state\": \"Valid\"} ]");
    const LoadResult result = loadFromString(text);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::MalformedJson);
}

TEST(SerializationTests, ParserNegativeZeroRoundTrips) {
    JsonParseError err;
    const JsonValue value = parseJson("-0.0", err);
    ASSERT_TRUE(err.ok) << err.message;
    EXPECT_TRUE(std::signbit(value.asNumber()));
    const std::string written = writeJson(value);
    const JsonValue reparsed = parseJson(written, err);
    ASSERT_TRUE(err.ok) << err.message;
    EXPECT_TRUE(std::signbit(reparsed.asNumber()));
}

TEST(SerializationTests, ParserLoneSurrogatesRejected) {
    JsonParseError err;
    parseJson(R"("\uD800")", err); // high surrogate, nothing after
    EXPECT_FALSE(err.ok);
    parseJson(R"("\uD800x")", err); // high surrogate, non-escape after
    EXPECT_FALSE(err.ok);
    parseJson(R"("\uD800\n")", err); // high surrogate, wrong escape after
    EXPECT_FALSE(err.ok);
    parseJson(R"("\uDC00")", err); // lone low surrogate
    EXPECT_FALSE(err.ok);
    parseJson(R"("\uD8")", err); // truncated hex
    EXPECT_FALSE(err.ok);

    // A valid surrogate pair escape (U+1F600) decodes to 4-byte UTF-8.
    const JsonValue emoji = parseJson("\"\\uD83D\\uDE00\"", err);
    ASSERT_TRUE(err.ok) << err.message;
    EXPECT_EQ(emoji.asString(), "\xF0\x9F\x98\x80");
}

TEST(SerializationTests, ParserUnterminatedInputRejected) {
    JsonParseError err;
    parseJson("\"abc", err);
    EXPECT_FALSE(err.ok);
    EXPECT_NE(err.message.find("unterminated string"), std::string::npos) << err.message;
    parseJson("{\"a\": \"b", err);
    EXPECT_FALSE(err.ok);
    parseJson("[1, 2", err);
    EXPECT_FALSE(err.ok);
    parseJson("{} extra", err); // trailing content
    EXPECT_FALSE(err.ok);
}

TEST(SerializationTests, ParserSkipsUtf8Bom) {
    // A leading UTF-8 BOM (as written by Notepad and other editors) is
    // skipped; the document parses and loads normally.
    JsonParseError err;
    const JsonValue value = parseJson("\xEF\xBB\xBF{}", err);
    EXPECT_TRUE(err.ok) << err.message;
    EXPECT_EQ(value.type(), JsonType::Object);

    const LoadResult result = loadFromString(std::string("\xEF\xBB\xBF") + kMinimalDocument);
    ASSERT_TRUE(result) << result.message;
    EXPECT_EQ(result.document->id(), 4001u);
    EXPECT_EQ(result.document->name(), "Minimal");
}

TEST(SerializationTests, WriterNonFiniteNumbersEmitZero) {
    // Documented JsonValue contract: non-finite numbers are written as 0 so
    // the output is always valid JSON (never "nan"/"inf" tokens).
    EXPECT_EQ(writeJson(JsonValue::makeNumber(std::numeric_limits<double>::quiet_NaN())), "0");
    EXPECT_EQ(writeJson(JsonValue::makeNumber(std::numeric_limits<double>::infinity())), "0");
    EXPECT_EQ(writeJson(JsonValue::makeNumber(-std::numeric_limits<double>::infinity())), "0");
}

TEST(SerializationTests, NanParameterValueSavesAsZeroValidJson) {
    // Consequence of the contract above at the document level: a NaN parameter
    // value is silently persisted as 0 (valid JSON, reloads cleanly). The
    // silent value change is flagged in the TEST report as an observation.
    PartDocument document("NanDoc");
    Parameter& p = document.addParameter("W", 1.0, UnitType::Millimeter);
    p.setValue(std::numeric_limits<double>::quiet_NaN());

    const std::string saved = saveToString(document);
    EXPECT_EQ(saved.find("nan"), std::string::npos);
    EXPECT_EQ(saved.find("inf"), std::string::npos);
    const LoadResult loaded = loadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->parameters().items().size(), 1u);
    EXPECT_EQ(loaded.document->parameters().items()[0]->value(), 0.0);
}

// --- serializer -------------------------------------------------------------

TEST(SerializationTests, EmptyOrWhitespaceOnlyStreamRejected) {
    LoadResult result = loadFromString("");
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::MalformedJson);
    EXPECT_EQ(result.document, nullptr);

    result = loadFromString("   \n\t  \r\n ");
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::MalformedJson);
    EXPECT_EQ(result.document, nullptr);
}

TEST(SerializationTests, InvalidObjectIdZeroRejected) {
    // Document id "0" (kInvalidObjectId) is rejected, not silently accepted.
    std::string docIdZero = kMinimalDocument;
    docIdZero.replace(docIdZero.find("\"4001\""), 6, "\"0\"");
    LoadResult result = loadFromString(docIdZero);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::InvalidFieldType);
    EXPECT_EQ(result.document, nullptr);

    // Parameter id "0" likewise.
    std::string paramIdZero = kMinimalDocument;
    paramIdZero.replace(paramIdZero.find("\"parameters\": []"), 16,
                        "\"parameters\": [ {\"id\": \"0\", \"name\": \"W\", \"value\": 1.0, "
                        "\"unit\": \"Millimeter\", \"expression\": \"\", \"state\": \"Valid\"} ]");
    result = loadFromString(paramIdZero);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::InvalidFieldType);
}

TEST(SerializationTests, MalformedIdStringsRejected) {
    const char* badIds[] = {
        "\"-3\"",                    // negative
        "\"abc\"",                   // non-numeric
        "\"12abc\"",                 // trailing garbage
        "\" 12\"",                   // leading space
        "\"+5\"",                    // explicit plus
        "\"\"",                      // empty
        "\"18446744073709551616\"",  // uint64 overflow
    };
    for (const char* badId : badIds) {
        std::string text = kMinimalDocument;
        text.replace(text.find("\"4001\""), 6, badId);
        const LoadResult result = loadFromString(text);
        EXPECT_FALSE(result) << "id " << badId << " was accepted";
        EXPECT_EQ(result.error, SerializationError::InvalidFieldType) << "id " << badId;
        EXPECT_EQ(result.document, nullptr);
    }
}

TEST(SerializationTests, SpecialCharacterStringsRoundTrip) {
    PartDocument original("Name \"with\" \\backslash\\ and\nnewline\ttab");
    original.addParameter("\xE5\xAF\xAC\xE5\xBA\xA6 (width)", 12.5, UnitType::Millimeter);
    Body& body = original.addBody(""); // empty body name is preserved
    body.addFeature<PlaceholderFeature>("Pad \"1\"\\", "Placeholder");

    const std::string firstSave = saveToString(original);
    const LoadResult loaded = loadFromString(firstSave);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->name(), original.name());
    ASSERT_EQ(loaded.document->parameters().items().size(), 1u);
    EXPECT_EQ(loaded.document->parameters().items()[0]->name(), "\xE5\xAF\xAC\xE5\xBA\xA6 (width)");
    ASSERT_EQ(loaded.document->bodies().size(), 1u);
    EXPECT_EQ(loaded.document->bodies()[0]->name(), "");
    ASSERT_EQ(loaded.document->bodies()[0]->features().size(), 1u);
    EXPECT_EQ(loaded.document->bodies()[0]->features()[0]->name(), "Pad \"1\"\\");

    // Canonical form survives the escaping round-trip byte-identically.
    EXPECT_EQ(saveToString(*loaded.document), firstSave);
}

TEST(SerializationTests, FailedStatesRoundTrip) {
    const std::string text = R"({
      "format": "ParametricCAD", "schemaVersion": 1, "documentType": "Part",
      "id": "4101", "name": "Failures",
      "parameters": [ {"id": "4102", "name": "W", "value": 1.0,
                       "unit": "Millimeter", "expression": "bad / 0", "state": "Failed"} ],
      "bodies": [ {"id": "4103", "name": "Body001",
                   "features": [ {"id": "4104", "name": "Pad001", "type": "Placeholder",
                                  "state": "Failed"} ]} ]
    })";
    const LoadResult loaded = loadFromString(text);
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->parameters().items().size(), 1u);
    EXPECT_EQ(loaded.document->parameters().items()[0]->state(), ParameterState::Failed);
    ASSERT_EQ(loaded.document->bodies().size(), 1u);
    ASSERT_EQ(loaded.document->bodies()[0]->features().size(), 1u);
    EXPECT_EQ(loaded.document->bodies()[0]->features()[0]->state(), ComputeState::Failed);

    // And back out again.
    const std::string saved = saveToString(*loaded.document);
    const LoadResult reloaded = loadFromString(saved);
    ASSERT_TRUE(reloaded) << reloaded.message;
    EXPECT_EQ(reloaded.document->parameters().items()[0]->state(), ParameterState::Failed);
    EXPECT_EQ(reloaded.document->bodies()[0]->features()[0]->state(), ComputeState::Failed);
}

TEST(SerializationTests, DuplicateParameterIdsRejected) {
    // Stable-identity rule: duplicate persistent ids within one document are
    // invalid; the loader rejects them with DuplicateId and a null document.
    const std::string text = R"({
      "format": "ParametricCAD", "schemaVersion": 1, "documentType": "Part",
      "id": "4201", "name": "Dupes",
      "parameters": [
        {"id": "4202", "name": "First", "value": 1.0,
         "unit": "Millimeter", "expression": "", "state": "Valid"},
        {"id": "4202", "name": "Second", "value": 2.0,
         "unit": "Millimeter", "expression": "", "state": "Valid"} ],
      "bodies": []
    })";
    const LoadResult loaded = loadFromString(text);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::DuplicateId);
    EXPECT_EQ(loaded.document, nullptr);
    EXPECT_NE(loaded.message.find("4202"), std::string::npos) << loaded.message;
}

TEST(SerializationTests, CrossCategoryDuplicateIdsRejected) {
    // Uniqueness holds across categories too: a body reusing a parameter's id
    // (or the document id) is rejected.
    const std::string bodyReusesParameterId = R"({
      "format": "ParametricCAD", "schemaVersion": 1, "documentType": "Part",
      "id": "4301", "name": "CrossDupes",
      "parameters": [ {"id": "4302", "name": "W", "value": 1.0,
                       "unit": "Millimeter", "expression": "", "state": "Valid"} ],
      "bodies": [ {"id": "4302", "name": "Body001", "features": []} ]
    })";
    LoadResult result = loadFromString(bodyReusesParameterId);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::DuplicateId);
    EXPECT_EQ(result.document, nullptr);
    EXPECT_NE(result.message.find("4302"), std::string::npos) << result.message;

    const std::string featureReusesDocumentId = R"({
      "format": "ParametricCAD", "schemaVersion": 1, "documentType": "Part",
      "id": "4301", "name": "CrossDupes",
      "parameters": [],
      "bodies": [ {"id": "4303", "name": "Body001",
                   "features": [ {"id": "4301", "name": "Pad001", "type": "Placeholder",
                                  "state": "Dirty"} ]} ]
    })";
    result = loadFromString(featureReusesDocumentId);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::DuplicateId);
    EXPECT_EQ(result.document, nullptr);
}

TEST(SerializationTests, MaxUint64IdStringRejectedBySerializer) {
    // Ids above kMaxObjectId (2^63 - 1) are rejected at load time so a file
    // can never push the id generator toward its wrap point.
    std::string text = kMinimalDocument;
    text.replace(text.find("\"parameters\": []"), 16,
                 "\"parameters\": [ {\"id\": \"18446744073709551615\", \"name\": \"W\", "
                 "\"value\": 1.0, \"unit\": \"Millimeter\", \"expression\": \"\", "
                 "\"state\": \"Valid\"} ]");
    const LoadResult result = loadFromString(text);
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::InvalidFieldType);
    EXPECT_EQ(result.document, nullptr);
    EXPECT_NE(result.message.find("18446744073709551615"), std::string::npos) << result.message;
}

// Every id reachable in a loaded document, including the auto-created frames.
std::vector<ObjectId> collectAllIds(const PartDocument& document) {
    std::vector<ObjectId> ids;
    ids.push_back(document.id());
    for (const auto& frame : document.frames()) ids.push_back(frame->id());
    for (const auto& parameter : document.parameters().items()) ids.push_back(parameter->id());
    for (const auto& body : document.bodies()) {
        ids.push_back(body->id());
        for (const auto& feature : body->features()) ids.push_back(feature->id());
    }
    return ids;
}

void expectPairwiseDistinct(const std::vector<ObjectId>& ids) {
    const std::unordered_set<ObjectId> unique(ids.begin(), ids.end());
    EXPECT_EQ(unique.size(), ids.size()) << "duplicate ObjectId in loaded document";
}

// Builds a schema v1 document whose ids are chosen by the test.
std::string documentWithIds(ObjectId docId, ObjectId paramId, ObjectId bodyId,
                            ObjectId featureId, const std::string& name) {
    std::ostringstream text;
    text << R"({"format": "ParametricCAD", "schemaVersion": 1, "documentType": "Part", "id": ")"
         << docId << R"(", "name": ")" << name << R"(", "parameters": [ {"id": ")" << paramId
         << R"(", "name": "W", "value": 1.0, "unit": "Millimeter", "expression": "", )"
         << R"("state": "Valid"} ], "bodies": [ {"id": ")" << bodyId
         << R"(", "name": "Body001", "features": [ {"id": ")" << featureId
         << R"(", "name": "Pad001", "type": "Placeholder", "state": "Dirty"} ]} ]})";
    return text.str();
}

TEST(SerializationTests, LoadedDocumentHasNoInternalIdCollisions) {
    // Emulates loading a file written by an earlier session into a session
    // whose id counter sits BELOW the file's id range (the counter is
    // monotonic in-process, so the file is crafted ahead of it). The Origin
    // frame allocated during restore must not collide with any persisted id:
    // paramId is chosen to equal exactly the counter value the frame would
    // receive if the generator were only advanced past the document id.
    const ObjectId base = ObjectIdGenerator::Next();
    const ObjectId docId = base + 1;
    const ObjectId paramId = base + 2; // frame-allocation spot of the defect
    const ObjectId bodyId = base + 3;
    const ObjectId featureId = base + 4;

    const LoadResult loaded =
        loadFromString(documentWithIds(docId, paramId, bodyId, featureId, "Ahead"));
    ASSERT_TRUE(loaded) << loaded.message;

    std::vector<ObjectId> ids = collectAllIds(*loaded.document);
    expectPairwiseDistinct(ids);

    // The first fresh id created after the load is strictly greater than every
    // persisted id in the file, and stays collision-free.
    const ReferenceFrame& newFrame = loaded.document->addFrame("PostLoad");
    EXPECT_GT(newFrame.id(), featureId);
    ids.push_back(newFrame.id());
    expectPairwiseDistinct(ids);
}

TEST(SerializationTests, SequentialLoadsRemainCollisionFree) {
    // Two documents saved by one earlier session, loaded one after the other
    // into a session whose counter starts below both id ranges (crafted ahead
    // of the monotonic counter, as above). Each file's parameter id sits
    // exactly on the frame-allocation spot that collided before the fix.
    const ObjectId base = ObjectIdGenerator::Next();
    const std::string fileA =
        documentWithIds(base + 10, base + 11, base + 13, base + 14, "SessionDocA");
    const std::string fileB =
        documentWithIds(base + 20, base + 21, base + 23, base + 24, "SessionDocB");

    const LoadResult loadedA = loadFromString(fileA);
    ASSERT_TRUE(loadedA) << loadedA.message;
    const LoadResult loadedB = loadFromString(fileB);
    ASSERT_TRUE(loadedB) << loadedB.message;

    expectPairwiseDistinct(collectAllIds(*loadedA.document));
    expectPairwiseDistinct(collectAllIds(*loadedB.document));

    // New objects created after both loads exceed both files' max ids and
    // collide with neither document.
    const ObjectId maxPersisted = base + 24;
    Parameter& newParameter = loadedA.document->addParameter("Fresh", 2.0, UnitType::Millimeter);
    Body& newBody = loadedB.document->addBody("Body002");
    EXPECT_GT(newParameter.id(), maxPersisted);
    EXPECT_GT(newBody.id(), maxPersisted);
    std::vector<ObjectId> idsA = collectAllIds(*loadedA.document);
    std::vector<ObjectId> idsB = collectAllIds(*loadedB.document);
    idsA.insert(idsA.end(), idsB.begin(), idsB.end());
    expectPairwiseDistinct(idsA);
}

// ---------------------------------------------------------------------------
// M2 schema v2 (explicit dependency edges, ADR-012).
// ---------------------------------------------------------------------------

// Minimal recomputable stub for the stub-edges-not-persisted test.
class StubRecomputable final : public IRecomputable {
public:
    StubRecomputable() : id_(ObjectIdGenerator::Next()) {}
    ObjectId id() const noexcept override { return id_; }
    RecomputeResult recompute(const RecomputeContext&) override {
        return {RecomputeStatus::Success, {}};
    }

private:
    ObjectId id_;
};

// Crafted v2 document with two parameters (6002, 6003) and one body (6004);
// the dependencies array is injected by each test.
std::string v2DocumentWithDependencies(const std::string& dependenciesJson) {
    return std::string(R"({
      "format": "ParametricCAD", "schemaVersion": 2, "documentType": "Part",
      "id": "6001", "name": "V2",
      "parameters": [ {"id": "6002", "name": "A", "value": 1.0, "unit": "Millimeter",
                       "expression": "", "state": "Valid"},
                      {"id": "6003", "name": "B", "value": 2.0, "unit": "Millimeter",
                       "expression": "", "state": "Valid"} ],
      "bodies": [ {"id": "6004", "name": "Body001", "features": []} ],
      "dependencies": )") + dependenciesJson + "\n}";
}

TEST(SerializationV2Test, M2_SER_001_StableIdsSurviveRoundTrip) {
    PartDocument original("V2Ids");
    Parameter& a = original.addParameter("A", 1.0, UnitType::Millimeter);
    Parameter& b = original.addParameter("B", 2.0, UnitType::Millimeter);
    Body& body = original.addBody("Body001");
    ASSERT_TRUE(original.addDependency(b.id(), a.id())); // A -> B

    const std::string saved = saveToString(original);
    // Save always writes the CURRENT schema version (v5 as of M5), even
    // though this document only exercises v2-era features (parameters +
    // generic dependency edges, no Material/BoxFeature).
    EXPECT_NE(saved.find("\"schemaVersion\": 7"), std::string::npos); // v7: M8.2 Revolve
    const LoadResult loaded = loadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(loaded.document->id(), original.id());
    ASSERT_EQ(loaded.document->parameters().items().size(), 2u);
    EXPECT_EQ(loaded.document->parameters().items()[0]->id(), a.id());
    EXPECT_EQ(loaded.document->parameters().items()[1]->id(), b.id());
    ASSERT_EQ(loaded.document->bodies().size(), 1u);
    EXPECT_EQ(loaded.document->bodies()[0]->id(), body.id());
}

TEST(SerializationV2Test, M2_SER_002_DependencyEdgesSurviveRoundTrip) {
    PartDocument original("V2Edges");
    Parameter& a = original.addParameter("A", 1.0, UnitType::Millimeter);
    Parameter& b = original.addParameter("B", 2.0, UnitType::Millimeter);
    ASSERT_TRUE(original.addDependency(b.id(), a.id())); // A -> B

    const std::string firstSave = saveToString(original);
    EXPECT_NE(firstSave.find("\"dependencies\""), std::string::npos);
    EXPECT_NE(firstSave.find("\"prerequisite\""), std::string::npos);

    const LoadResult loaded = loadFromString(firstSave);
    ASSERT_TRUE(loaded) << loaded.message;
    const DependencyGraph& graph = loaded.document->dependencyGraph();
    EXPECT_EQ(graph.dependentsOf(a.id()), std::vector<ObjectId>{b.id()});
    EXPECT_EQ(graph.prerequisitesOf(b.id()), std::vector<ObjectId>{a.id()});

    // Graph states are NOT persisted: every node starts Dirty after load ...
    EXPECT_EQ(graph.state(a.id()), ComputeState::Dirty);
    EXPECT_EQ(graph.state(b.id()), ComputeState::Dirty);
    // ... and the first recompute covers everything.
    EXPECT_TRUE(loaded.document->recompute().success);
    EXPECT_EQ(loaded.document->dependencyGraph().state(b.id()), ComputeState::Valid);

    // Canonical v2 output is byte-identical across a round trip.
    EXPECT_EQ(saveToString(*loaded.document), firstSave);
}

TEST(SerializationV2Test, M2_SER_003_ObjectRegistryRebuiltAfterLoad) {
    PartDocument original("V2Registry");
    Parameter& parameter = original.addParameter("A", 1.0, UnitType::Millimeter);
    Body& body = original.addBody("Body001");

    const LoadResult loaded = loadFromString(saveToString(original));
    ASSERT_TRUE(loaded) << loaded.message;
    const ObjectRegistry& registry = loaded.document->objectRegistry();
    EXPECT_TRUE(registry.contains(parameter.id()));
    EXPECT_TRUE(registry.contains(body.id()));

    // Lookup resolves the loaded document's own runtime objects.
    const ObjectRegistry::ObjectRef* parameterRef = registry.find(parameter.id());
    ASSERT_NE(parameterRef, nullptr);
    EXPECT_EQ(std::get<Parameter*>(*parameterRef),
              loaded.document->parameters().findById(parameter.id()));
    const ObjectRegistry::ObjectRef* bodyRef = registry.find(body.id());
    ASSERT_NE(bodyRef, nullptr);
    EXPECT_EQ(std::get<Body*>(*bodyRef), loaded.document->bodies()[0].get());
}

TEST(SerializationV2Test, M2_SER_004_V1FileStillLoads) {
    // kMinimalDocument is schema v1 (no dependencies array).
    const LoadResult minimal = loadFromString(kMinimalDocument);
    ASSERT_TRUE(minimal) << minimal.message;
    EXPECT_EQ(minimal.document->dependencyGraph().nodeCount(), 0u);

    // A v1 file with a parameter: the parameter gets a graph node (Dirty).
    std::string v1WithParameter = kMinimalDocument;
    v1WithParameter.replace(v1WithParameter.find("\"parameters\": []"), 16,
                            "\"parameters\": [ {\"id\": \"4005\", \"name\": \"W\", \"value\": 1.0, "
                            "\"unit\": \"Millimeter\", \"expression\": \"\", \"state\": \"Valid\"} ]");
    const LoadResult loaded = loadFromString(v1WithParameter);
    ASSERT_TRUE(loaded) << loaded.message;
    ASSERT_EQ(loaded.document->parameters().items().size(), 1u);
    const ObjectId parameterId = loaded.document->parameters().items()[0]->id();
    EXPECT_TRUE(loaded.document->dependencyGraph().hasNode(parameterId));
    EXPECT_EQ(loaded.document->dependencyGraph().state(parameterId), ComputeState::Dirty);
    EXPECT_TRUE(loaded.document->dependencyGraph().dependentsOf(parameterId).empty());
}

TEST(SerializationV2Test, M2_SER_005_StubEdgesNotPersisted) {
    PartDocument document("V2Stubs");
    Parameter& width = document.addParameter("Width", 1.0, UnitType::Millimeter);
    StubRecomputable stub; // runtime-only, externally owned
    ASSERT_TRUE(document.addRecomputableNode(stub));
    ASSERT_TRUE(document.addDependency(stub.id(), width.id())); // Width -> stub

    const std::string saved = saveToString(document);
    // The stub id appears nowhere in the file; the edge touching it is not
    // saved. Searched in its PERSISTED form -- ids are written as quoted
    // decimal strings -- because a bare numeric search matches any number
    // with the same digits: it collided with "schemaVersion": 5 the moment
    // M5 bumped the version, failing a test about stub edges for a reason
    // that had nothing to do with stub edges.
    EXPECT_EQ(saved.find("\"" + std::to_string(stub.id()) + "\""), std::string::npos);

    const LoadResult loaded = loadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_FALSE(loaded.document->dependencyGraph().hasNode(stub.id()));
    EXPECT_TRUE(loaded.document->dependencyGraph().dependentsOf(width.id()).empty());
}

TEST(SerializationV2Test, M2_SER_006_UnknownDependencyIdRejected) {
    // Endpoint that is no object in the file at all.
    LoadResult result = loadFromString(v2DocumentWithDependencies(
        R"([ {"prerequisite": "6002", "dependent": "9999"} ])"));
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::UnknownDependencyId);
    EXPECT_EQ(result.document, nullptr);
    EXPECT_NE(result.message.find("9999"), std::string::npos) << result.message;

    // Endpoint that is a persisted object but not a graph-node object (body).
    result = loadFromString(v2DocumentWithDependencies(
        R"([ {"prerequisite": "6002", "dependent": "6004"} ])"));
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::UnknownDependencyId);
    EXPECT_EQ(result.document, nullptr);
}

TEST(SerializationV2Test, M2_SER_007_InvalidDependencyRejected) {
    // Self-edge.
    LoadResult result = loadFromString(v2DocumentWithDependencies(
        R"([ {"prerequisite": "6002", "dependent": "6002"} ])"));
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::InvalidDependency);
    EXPECT_EQ(result.document, nullptr);

    // Two-edge cycle.
    result = loadFromString(v2DocumentWithDependencies(
        R"([ {"prerequisite": "6002", "dependent": "6003"},
             {"prerequisite": "6003", "dependent": "6002"} ])"));
    EXPECT_FALSE(result);
    EXPECT_EQ(result.error, SerializationError::InvalidDependency);
    EXPECT_EQ(result.document, nullptr);

    // A well-formed edge in the same crafted file loads fine.
    result = loadFromString(v2DocumentWithDependencies(
        R"([ {"prerequisite": "6002", "dependent": "6003"} ])"));
    ASSERT_TRUE(result) << result.message;
    ASSERT_EQ(result.document->parameters().items().size(), 2u);
    const ObjectId a = result.document->parameters().items()[0]->id();
    const ObjectId b = result.document->parameters().items()[1]->id();
    EXPECT_EQ(result.document->dependencyGraph().dependentsOf(a), std::vector<ObjectId>{b});
}

// Defense-in-depth layer under the serializer cap: even a direct
// AdvancePast(max uint64) clamps to kMaxObjectId and never wraps the counter.
// NOTE: this permanently advances the shared generator to 2^63, so it lives
// in its own suite whose first (and only) registration is the LAST test in
// the last translation unit -- gtest runs whole suites in first-registration
// order, so every other test precedes it in a single-process run; assertions
// use monotonicity (not exact equality) so the test also passes under
// gtest_discover_tests (one process per test).
TEST(GeneratorLimitTest, AdvancePastMaxUint64DoesNotWrapGenerator) {
    const ObjectId sane = ObjectIdGenerator::Next();
    ObjectIdGenerator::AdvancePast(std::numeric_limits<ObjectId>::max());
    const ObjectId next1 = ObjectIdGenerator::Next();
    const ObjectId next2 = ObjectIdGenerator::Next();

    EXPECT_NE(next1, kInvalidObjectId)
        << "id generator wrapped to kInvalidObjectId after AdvancePast(max)";
    EXPECT_GE(next1, ObjectId{1} << 63)
        << "counter did not advance past the clamped kMaxObjectId";
    EXPECT_GT(next1, sane)
        << "id generator restarted below previously issued ids after AdvancePast(max)";
    EXPECT_GT(next2, next1) << "ids stopped increasing after AdvancePast(max)";
}

// The other half of the same hazard, and it belongs in THIS suite for the same
// reason: it must leave the generator past the cap to mean anything.
//
// The counter surviving is only half the contract. Every id issued after that
// point is ABOVE what the loader accepts, so a save that succeeds produces a
// file that can never be opened -- independent review found
// `ASSERT_TRUE(savePartDocument(...))` passing and the very next
// `loadPartDocument` refusing the bytes it had just written. ObjectId.h claimed
// "2^63 organic allocations of headroom"; the SAVABLE headroom is zero.
//
// validateSaveable now refuses, turning silent data loss into a reported error.
TEST(GeneratorLimitTest, SavingIsRefusedOnceTheGeneratorHasPassedTheCap) {
    ObjectIdGenerator::AdvancePast(kMaxObjectId);

    PartDocument document{"beyondTheCap"};
    Sketch& sketch = document.addSketch("s");
    sketch.addLine(Vec2{0, 0}, Vec2{100, 0});
    ASSERT_GT(document.id(), kMaxObjectId);

    std::ostringstream out;
    const SaveResult saved = savePartDocument(document, out);

    EXPECT_FALSE(saved);
    EXPECT_NE(saved.message.find("could never be loaded back"), std::string::npos)
        << saved.message;
}

} // namespace
