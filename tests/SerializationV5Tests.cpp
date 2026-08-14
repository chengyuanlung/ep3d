// M5-L: schema v5 persistence for sketch constraints (spec 17, ADR-M5-001).
//
// Two rules govern everything here:
//   1. What is persisted is SEMANTIC -- constraint id, kind, entity and
//      sub-element references, bound Parameter ObjectId. Solver variable
//      indexes, residual indexes, DOF and solve status are derived and are
//      never written.
//   2. There are no dangling references. A reference the loader would reject
//      must never have been savable either (ADR-M3-008), and the deletion
//      policy (ADR-M5-009) is what keeps a document from reaching that state.

#include "Core/Document/PartDocument.h"
#include "Core/Feature/PadFeature.h"
#include "Core/Feature/BoxFeature.h"
#include "Core/Serialization/PartDocumentSerializer.h"
#include "Fakes/FakeGeometryKernel.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace paramcad;

std::string SaveToString(const PartDocument& document) {
    std::ostringstream out;
    const SaveResult result = savePartDocument(document, out);
    EXPECT_TRUE(result) << result.message;
    return out.str();
}

SaveResult TrySave(const PartDocument& document, std::string& text) {
    std::ostringstream out;
    const SaveResult result = savePartDocument(document, out);
    text = out.str();
    return result;
}

LoadResult LoadFromString(const std::string& text) {
    std::istringstream in(text);
    return loadPartDocument(in);
}

// A rectangle with one of every constraint kind that applies to it, plus a
// circle carrying Radius/Diameter and a pair of lines carrying Angle -- so the
// round-trip covers all nine kinds rather than the four a rectangle needs.
struct ConstrainedDoc {
    PartDocument document{"V5Doc"};
    FakeGeometryKernel kernel;
    Sketch* sketch = nullptr;
    Parameter* width = nullptr;
    Parameter* height = nullptr;
    Parameter* diagonal = nullptr;
    Parameter* radius = nullptr;
    Parameter* diameter = nullptr;
    Parameter* angle = nullptr;
    Parameter* padLength = nullptr;
    PadFeature* pad = nullptr;

    SketchEntityId bottom{}, right{}, top{}, left{}, circle{}, ray{};
    std::vector<SketchConstraintId> constraintIds;

    ConstrainedDoc() {
        document.setGeometryKernel(&kernel);
        document.addMaterial("Steel", 7850.0);
        width = &document.addParameter("Width", 100.0, UnitType::Millimeter);
        height = &document.addParameter("Height", 50.0, UnitType::Millimeter);
        diagonal = &document.addParameter("Diagonal", 111.803398874989, UnitType::Millimeter);
        radius = &document.addParameter("Radius", 12.0, UnitType::Millimeter);
        diameter = &document.addParameter("Diameter", 24.0, UnitType::Millimeter);
        angle = &document.addParameter("Angle", 0.5, UnitType::Radian);
        padLength = &document.addParameter("PadLength", 20.0, UnitType::Millimeter);

        Sketch& s = document.addSketch("Sketch001");
        sketch = &s;
        bottom = s.addLine(Vec2{0, 0}, Vec2{100, 0});
        right = s.addLine(Vec2{100, 0}, Vec2{100, 50});
        top = s.addLine(Vec2{100, 50}, Vec2{0, 50});
        left = s.addLine(Vec2{0, 50}, Vec2{0, 0});
        circle = s.addCircle(Vec2{200, 200}, 12.0);
        ray = s.addLine(Vec2{300, 0}, Vec2{340, 20});

        const auto sp = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::StartPoint};
        };
        const auto ep = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::EndPoint};
        };
        const auto cp = [](SketchEntityId id) {
            return SketchElementRef{id, SketchSubElement::CenterPoint};
        };

        const auto add = [&](SketchConstraintData data) {
            const SketchConstraintId id = document.addSketchConstraint(s.id(), std::move(data));
            EXPECT_NE(id, kInvalidSketchConstraintId);
            constraintIds.push_back(id);
        };

        add(CoincidentConstraint{ep(bottom), sp(right)});
        add(CoincidentConstraint{ep(right), sp(top)});
        add(CoincidentConstraint{ep(top), sp(left)});
        add(CoincidentConstraint{ep(left), sp(bottom)});
        add(HorizontalConstraint{bottom});
        add(VerticalConstraint{right});
        add(FixConstraint{sp(bottom)});
        add(LengthConstraint{bottom, width->id()});
        add(LengthConstraint{right, height->id()});
        add(DistanceConstraint{sp(bottom), ep(right), diagonal->id()});
        add(FixConstraint{cp(circle)});
        add(RadiusConstraint{circle, radius->id()});
        add(DiameterConstraint{circle, diameter->id()});
        add(AngleConstraint{bottom, ray, angle->id()});

        Body& body = document.addBody("Body001");
        pad = &document.addPadFeature(body, "Pad001", s.id(), padLength->id());
    }
};

const Sketch& OnlySketch(const PartDocument& document) {
    const std::vector<const Sketch*> sketches = document.sketches();
    EXPECT_EQ(sketches.size(), 1u);
    return *sketches.front();
}

// --- Round trip --------------------------------------------------------------

TEST(SerializationV5Test, M5_SER_001_SchemaVersionIsFive) {
    ConstrainedDoc doc;
    EXPECT_NE(SaveToString(doc.document).find("\"schemaVersion\": 8"), std::string::npos); // v8: M8.3 Fillet/Chamfer
}

TEST(SerializationV5Test, M5_SER_002_AllNineConstraintKindsSurviveRoundTrip) {
    ConstrainedDoc doc;
    const LoadResult loaded = LoadFromString(SaveToString(doc.document));
    ASSERT_TRUE(loaded) << loaded.message;

    const Sketch& before = *doc.sketch;
    const Sketch& after = OnlySketch(*loaded.document);
    ASSERT_EQ(after.constraints().size(), before.constraints().size());

    // Compared BY ID, never by position: the file's array order is storage, not
    // identity, and a test that walked both vectors in parallel would pass even
    // if every constraint came back attached to the wrong id.
    for (const SketchConstraint& original : before.constraints()) {
        const SketchConstraint* restored = after.findConstraint(original.id);
        ASSERT_NE(restored, nullptr)
            << "constraint " << ToObjectId(original.id) << " did not survive";
        EXPECT_STREQ(ConstraintKindName(restored->data), ConstraintKindName(original.data));
        EXPECT_EQ(BoundParameterId(restored->data), BoundParameterId(original.data));
        EXPECT_EQ(ReferencedEntities(restored->data), ReferencedEntities(original.data));
    }

    // Every kind actually appeared, so a silently dropped kind cannot pass the
    // loop above by simply not being there on either side.
    std::vector<std::string> kinds;
    for (const SketchConstraint& c : after.constraints())
        kinds.push_back(ConstraintKindName(c.data));
    std::sort(kinds.begin(), kinds.end());
    kinds.erase(std::unique(kinds.begin(), kinds.end()), kinds.end());
    EXPECT_EQ(kinds.size(), 9u) << "not all nine constraint kinds round-tripped";
}

TEST(SerializationV5Test, M5_SER_003_SubElementsSurviveAsNamesNotIntegers) {
    ConstrainedDoc doc;
    const std::string saved = SaveToString(doc.document);
    EXPECT_NE(saved.find("\"subElement\": \"StartPoint\""), std::string::npos);
    EXPECT_NE(saved.find("\"subElement\": \"EndPoint\""), std::string::npos);
    EXPECT_NE(saved.find("\"subElement\": \"CenterPoint\""), std::string::npos);

    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& after = OnlySketch(*loaded.document);

    // The Fix on the circle's centre must still be on the CENTRE. A sub-element
    // that came back as Whole or StartPoint would still solve, just wrongly.
    bool foundCenterFix = false;
    for (const SketchConstraint& c : after.constraints()) {
        const auto* fix = std::get_if<FixConstraint>(&c.data);
        if (fix == nullptr || fix->target.entityId != doc.circle) continue;
        EXPECT_EQ(fix->target.subElement, SketchSubElement::CenterPoint);
        foundCenterFix = true;
    }
    EXPECT_TRUE(foundCenterFix);
}

TEST(SerializationV5Test, M5_SER_004_ByteIdenticalRoundTrip) {
    ConstrainedDoc doc;
    const std::string first = SaveToString(doc.document);
    const LoadResult loaded = LoadFromString(first);
    ASSERT_TRUE(loaded) << loaded.message;
    EXPECT_EQ(SaveToString(*loaded.document), first);
}

TEST(SerializationV5Test, M5_SER_005_NoSolverStateIsPersisted) {
    ConstrainedDoc doc;
    const std::string saved = SaveToString(doc.document);
    // Solver variable/residual indexes, DOF and solve status are derived
    // (spec 17). None of them may appear in the file under any name.
    for (const char* forbidden : {"variableIndex", "residualIndex", "varA", "varB",
                                  "jacobian", "degreesOfFreedom", "dof", "solveStatus",
                                  "Solved", "UnderConstrained", "OverConstrained"}) {
        EXPECT_EQ(saved.find(forbidden), std::string::npos)
            << "solver-derived state '" << forbidden << "' reached the file";
    }
}

TEST(SerializationV5Test, M5_SER_006_ParameterEdgesAreRederivedNotStored) {
    ConstrainedDoc doc;
    const LoadResult loaded = LoadFromString(SaveToString(doc.document));
    ASSERT_TRUE(loaded) << loaded.message;

    const Sketch& after = OnlySketch(*loaded.document);
    const DependencyGraph& graph = loaded.document->dependencyGraph();

    // Each bound Parameter drives the sketch again after load. Without the
    // re-derivation, the sketch loads, solves once, and then silently freezes:
    // editing Width would never dirty it again.
    for (const ObjectId parameterId : {doc.width->id(), doc.height->id(), doc.diagonal->id(),
                                       doc.radius->id(), doc.diameter->id(), doc.angle->id()}) {
        const std::vector<ObjectId> dependents = graph.dependentsOf(parameterId);
        EXPECT_NE(std::find(dependents.begin(), dependents.end(), after.id()), dependents.end())
            << "parameter " << parameterId << " no longer drives the sketch after load";
    }
    // PadLength drives the Pad, never the sketch (spec 13).
    const std::vector<ObjectId> padLengthDependents = graph.dependentsOf(doc.padLength->id());
    EXPECT_EQ(std::find(padLengthDependents.begin(), padLengthDependents.end(), after.id()),
              padLengthDependents.end());
}

TEST(SerializationV5Test, M5_SER_007_ConstraintOrderIsNotIdentity) {
    ConstrainedDoc doc;
    const std::string baseline = SaveToString(doc.document);

    // Remove a middle constraint and re-add an equivalent one: the vector order
    // changes and the new constraint gets a new id, but every OTHER constraint
    // must keep its id and its references.
    const SketchConstraintId removed = doc.constraintIds[4]; // Horizontal(bottom)
    ASSERT_TRUE(doc.document.removeSketchConstraint(doc.sketch->id(), removed));
    ASSERT_NE(doc.document.addSketchConstraint(doc.sketch->id(),
                                               HorizontalConstraint{doc.bottom}),
              kInvalidSketchConstraintId);

    const LoadResult loaded = LoadFromString(SaveToString(doc.document));
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& after = OnlySketch(*loaded.document);

    EXPECT_EQ(after.findConstraint(removed), nullptr);
    for (std::size_t i = 0; i < doc.constraintIds.size(); ++i) {
        if (doc.constraintIds[i] == removed) continue;
        EXPECT_NE(after.findConstraint(doc.constraintIds[i]), nullptr)
            << "constraint " << ToObjectId(doc.constraintIds[i]) << " lost its identity";
    }
    EXPECT_NE(baseline, SaveToString(*loaded.document)); // the edit did happen
}

// --- Backward compatibility ---------------------------------------------------

TEST(SerializationV5Test, M5_SER_008_V4FileWithoutConstraintsStillLoads) {
    ConstrainedDoc doc;
    std::string saved = SaveToString(doc.document);

    // Strip every constraints array and claim v4 -- exactly the shape of a file
    // written before M5 existed.
    const std::size_t versionPos = saved.find("\"schemaVersion\": 8");
    ASSERT_NE(versionPos, std::string::npos); // v8: M8.3 Fillet/Chamfer -- update on every bump
    saved.replace(versionPos, 18, "\"schemaVersion\": 4");
    for (;;) {
        const std::size_t start = saved.find("\"constraints\": [");
        if (start == std::string::npos) break;
        const std::size_t end = saved.find(']', start);
        ASSERT_NE(end, std::string::npos);
        saved.erase(start, end - start + 1);
        // Remove the now-trailing comma/whitespace the erase left behind.
        std::size_t comma = saved.find_last_not_of(" \t\r\n", start - 1);
        if (comma != std::string::npos && saved[comma] == ',') saved.erase(comma, 1);
        comma = saved.find_first_not_of(" \t\r\n", start > comma ? comma : start);
        if (comma != std::string::npos && saved[comma] == ',') saved.erase(comma, 1);
    }

    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;
    const Sketch& after = OnlySketch(*loaded.document);
    // A v4 sketch is free geometry with no constraints -- which is what it was.
    EXPECT_TRUE(after.constraints().empty());
    EXPECT_EQ(after.entities().size(), 6u);
}

// --- Rejected files (spec 17: no dangling references) -------------------------

TEST(SerializationV5Test, M5_SER_009_ConstraintNamingAMissingEntityIsRejected) {
    ConstrainedDoc doc;
    std::string saved = SaveToString(doc.document);
    const std::string realId = std::to_string(ToObjectId(doc.bottom));
    // Re-point one constraint's line at an entity id that is not in the sketch.
    const std::size_t at = saved.find("\"line\": \"" + realId + "\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, 9 + realId.size() + 1, "\"line\": \"999999\"");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
}

TEST(SerializationV5Test, M5_SER_010_ConstraintNamingAMissingParameterIsRejected) {
    ConstrainedDoc doc;
    std::string saved = SaveToString(doc.document);
    const std::string realId = std::to_string(doc.radius->id());
    const std::size_t at = saved.find("\"parameterId\": \"" + realId + "\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, 16 + realId.size() + 1, "\"parameterId\": \"999999\"");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::UnknownDependencyId);
}

TEST(SerializationV5Test, M5_SER_011_DuplicateConstraintIdIsRejected) {
    ConstrainedDoc doc;
    std::string saved = SaveToString(doc.document);
    const std::string first = std::to_string(ToObjectId(doc.constraintIds[0]));
    const std::string second = std::to_string(ToObjectId(doc.constraintIds[1]));
    const std::size_t at = saved.find("\"id\": \"" + second + "\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, 7 + second.size() + 1, "\"id\": \"" + first + "\"");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::DuplicateId);
}

TEST(SerializationV5Test, M5_SER_012_UnknownConstraintKindIsRejected) {
    ConstrainedDoc doc;
    std::string saved = SaveToString(doc.document);
    const std::size_t at = saved.find("\"type\": \"Horizontal\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, 20, "\"type\": \"Perpendic\"  ");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::InvalidEnumValue);
}

TEST(SerializationV5Test, M5_SER_013_UnknownSubElementIsRejected) {
    ConstrainedDoc doc;
    std::string saved = SaveToString(doc.document);
    const std::size_t at = saved.find("\"subElement\": \"CenterPoint\"");
    ASSERT_NE(at, std::string::npos);
    saved.replace(at, 27, "\"subElement\": \"Middle\"     ");

    const LoadResult loaded = LoadFromString(saved);
    EXPECT_FALSE(loaded);
    EXPECT_EQ(loaded.error, SerializationError::InvalidEnumValue);
}

// --- Deletion policy (spec 17, ADR-M5-009) ------------------------------------

TEST(SerializationV5Test, M5_DELETE_001_RemovingAnEntityCascadesToItsConstraints) {
    ConstrainedDoc doc;
    const std::vector<SketchConstraintId> referencing =
        doc.sketch->constraintsReferencing(doc.circle);
    ASSERT_EQ(referencing.size(), 3u); // Fix(centre), Radius, Diameter

    ASSERT_TRUE(doc.document.removeSketchEntity(doc.sketch->id(), doc.circle));

    const Sketch& after = OnlySketch(doc.document);
    EXPECT_EQ(after.findEntity(doc.circle), nullptr);
    for (SketchConstraintId id : referencing) EXPECT_EQ(after.findConstraint(id), nullptr);
    // Everything else is untouched.
    EXPECT_NE(after.findConstraint(doc.constraintIds[0]), nullptr);

    // And the file it now writes has no dangling reference in it.
    const LoadResult reloaded = LoadFromString(SaveToString(doc.document));
    EXPECT_TRUE(reloaded) << reloaded.message;
}

TEST(SerializationV5Test, M5_DELETE_002_CascadeDropsTheReleasedParameterEdges) {
    ConstrainedDoc doc;
    ASSERT_TRUE(doc.document.removeSketchEntity(doc.sketch->id(), doc.circle));

    const DependencyGraph& graph = doc.document.dependencyGraph();
    for (ObjectId released : {doc.radius->id(), doc.diameter->id()}) {
        const std::vector<ObjectId> dependents = graph.dependentsOf(released);
        EXPECT_EQ(std::find(dependents.begin(), dependents.end(), doc.sketch->id()),
                  dependents.end())
            << "parameter " << released << " still drives a sketch that no longer reads it";
    }
    // Width's constraint survived, so its edge must survive too.
    const std::vector<ObjectId> widthDependents = graph.dependentsOf(doc.width->id());
    EXPECT_NE(std::find(widthDependents.begin(), widthDependents.end(), doc.sketch->id()),
              widthDependents.end());
}

TEST(SerializationV5Test, M5_DELETE_003_DeletingABoundParameterIsRefused) {
    ConstrainedDoc doc;
    const ObjectId widthId = doc.width->id();
    ASSERT_FALSE(doc.document.constraintsBindingParameter(widthId).empty());

    EXPECT_FALSE(doc.document.removeObject(widthId));

    // Refused means UNCHANGED, not half-removed: the parameter, its graph node
    // and its edge are all still there.
    EXPECT_NE(doc.document.parameters().findById(widthId), nullptr);
    EXPECT_TRUE(doc.document.dependencyGraph().hasNode(widthId));
    const std::vector<ObjectId> dependents =
        doc.document.dependencyGraph().dependentsOf(widthId);
    EXPECT_NE(std::find(dependents.begin(), dependents.end(), doc.sketch->id()), dependents.end());
    EXPECT_TRUE(LoadFromString(SaveToString(doc.document)));
}

TEST(SerializationV5Test, M5_DELETE_004_UnboundParameterIsStillDeletable) {
    ConstrainedDoc doc;
    Parameter& spare = doc.document.addParameter("Spare", 1.0, UnitType::Millimeter);
    const ObjectId spareId = spare.id();
    // The refusal is about BOUND parameters only; it must not have turned into
    // "parameters can never be deleted".
    EXPECT_TRUE(doc.document.removeObject(spareId));
    EXPECT_EQ(doc.document.parameters().findById(spareId), nullptr);
}

TEST(SerializationV5Test, M5_DELETE_005_ParameterBecomesDeletableOnceUnbound) {
    ConstrainedDoc doc;
    const ObjectId diagonalId = doc.diagonal->id();
    const std::vector<SketchConstraintId> binding =
        doc.document.constraintsBindingParameter(diagonalId);
    ASSERT_EQ(binding.size(), 1u);

    ASSERT_TRUE(doc.document.removeSketchConstraint(doc.sketch->id(), binding.front()));
    EXPECT_TRUE(doc.document.constraintsBindingParameter(diagonalId).empty());
    EXPECT_TRUE(doc.document.removeObject(diagonalId));
}

TEST(SerializationV5Test, M5_DELETE_006_RemovingAConstraintLeavesEveryOtherIdentityIntact) {
    ConstrainedDoc doc;
    const SketchConstraintId removed = doc.constraintIds[7]; // Length(bottom, Width)
    const std::vector<SketchEntityId> entitiesBefore = [&] {
        std::vector<SketchEntityId> ids;
        for (const SketchEntity& e : doc.sketch->entities()) ids.push_back(e.id);
        return ids;
    }();

    ASSERT_TRUE(doc.document.removeSketchConstraint(doc.sketch->id(), removed));

    const Sketch& after = OnlySketch(doc.document);
    EXPECT_EQ(after.findConstraint(removed), nullptr);
    for (SketchConstraintId id : doc.constraintIds) {
        if (id == removed) continue;
        EXPECT_NE(after.findConstraint(id), nullptr);
    }
    std::vector<SketchEntityId> entitiesAfter;
    for (const SketchEntity& e : after.entities()) entitiesAfter.push_back(e.id);
    EXPECT_EQ(entitiesAfter, entitiesBefore); // no entity was renumbered
}

// --- The id generator must clear EVERY id in the file -------------------------

TEST(SerializationV5Test, M5_SER_015_LoadAdvancesTheGeneratorPastSketchEntityAndConstraintIds) {
    // A v5 file's LARGEST ids are typically its entity and constraint ids, and
    // the loader's maxPersistedId scan omitted all three sketch-side id kinds.
    // The generator was then left below the file's true maximum, and
    // PartDocument's constructor allocated massPropertiesNode_ / the Origin
    // frame from it -- taking an id a sketch in the same file already owned.
    // registerObject then failed on the duplicate while graph_.addNode
    // succeeded, so the two objects shared a node: the Pad lost its profile and
    // the sketch was never invoked, silently and permanently.
    //
    // A document saved and reloaded IN THIS PROCESS cannot show this, because
    // the generator is already past every id it contains -- which is exactly
    // why the entire serialization suite was blind to it. So the sketch-side
    // ids are rewritten ABOVE the live generator first, reproducing what a
    // fresh process sees when it opens a file written by an earlier one.
    ConstrainedDoc doc;
    std::string saved = SaveToString(doc.document);

    // Every id is persisted as a QUOTED decimal string and is unique
    // document-wide, so a quoted-token rewrite updates the definition and all
    // of its references consistently. Two passes via a placeholder, so a
    // rewritten id cannot be rewritten again.
    // The sketch id is set to EXACTLY the next id the generator will hand out,
    // because that is the collision the bug produces: PartDocument's
    // constructor allocates massPropertiesNode_ and the Origin frame BEFORE any
    // restore runs, so a file whose sketch id equals one of those takes the id
    // first and the sketch is never registered. A merely "large" id cannot
    // reproduce it -- and RestoreObjectId's defence-in-depth advance happens
    // too late to help, since it runs after the constructor.
    const ObjectId probe = ObjectIdGenerator::Next();
    const ObjectId kLift = (probe + 1) - doc.sketch->id();
    std::vector<ObjectId> sketchSideIds{doc.sketch->id()};
    for (const SketchEntity& entity : doc.sketch->entities())
        sketchSideIds.push_back(ToObjectId(entity.id));
    for (const SketchConstraint& constraint : doc.sketch->constraints())
        sketchSideIds.push_back(ToObjectId(constraint.id));

    const auto replaceAll = [](std::string& text, const std::string& from,
                               const std::string& to) {
        for (std::size_t at = text.find(from); at != std::string::npos;
             at = text.find(from, at + to.size()))
            text.replace(at, from.size(), to);
    };
    for (ObjectId id : sketchSideIds)
        replaceAll(saved, "\"" + std::to_string(id) + "\"",
                   "\"@" + std::to_string(id + kLift) + "\"");
    replaceAll(saved, "\"@", "\"");

    const LoadResult loaded = LoadFromString(saved);
    ASSERT_TRUE(loaded) << loaded.message;

    // The sketch is genuinely registered -- resolvable as a Sketch, not as
    // whatever object collided with it.
    const Sketch& after = OnlySketch(*loaded.document);
    const ObjectRegistry::ObjectRef* ref = loaded.document->objectRegistry().find(after.id());
    ASSERT_NE(ref, nullptr) << "the sketch is not in the registry at all";
    EXPECT_TRUE(std::holds_alternative<Sketch*>(*ref))
        << "the sketch's id resolves to some other object";
    EXPECT_EQ(after.constraints().size(), doc.sketch->constraints().size());
}

// --- Removing a Body must unhook the features it owns -------------------------

TEST(SerializationV5Test, M5_DELETE_007_RemovingABodyUnhooksItsFeatures) {
    // The Body branch of removeObject destroyed the Body -- and with it every
    // Feature it owns -- while leaving those features registered and
    // graph-scheduled. The next recompute() then called recompute() on freed
    // memory. savePartDocument succeeded in between, so the crash landed later
    // and somewhere else.
    // No solver is injected here on purpose: this fixture exists for
    // persistence, and whether the sketch solves is irrelevant to whether a
    // destroyed feature stays registered.
    ConstrainedDoc doc;
    doc.document.recompute();
    const ObjectId padId = doc.pad->id();
    ObjectId bodyId = kInvalidObjectId;
    for (const auto& body : doc.document.bodies()) bodyId = body->id();
    ASSERT_NE(bodyId, kInvalidObjectId);

    ASSERT_TRUE(doc.document.removeObject(bodyId));

    EXPECT_EQ(doc.document.objectRegistry().find(padId), nullptr)
        << "a destroyed feature is still resolvable";
    EXPECT_FALSE(doc.document.dependencyGraph().hasNode(padId))
        << "a destroyed feature is still a graph node";

    // The line that used to be a use-after-free.
    doc.document.recompute();

    // And the document is still coherent afterwards.
    std::string text;
    EXPECT_TRUE(TrySave(doc.document, text));
    EXPECT_TRUE(LoadFromString(text));
}

// --- Regressions for the M5 round-3 review ------------------------------------
//
// Five round-2 fixes shipped with NO test at all: a reviewer reverted all five
// and the suite stayed 473/473 green. These close that, and cover the defects
// the round-2 fixes themselves introduced.

TEST(SerializationV5Test, M5_REV3_010_RestoringADuplicateParameterIdIsRefusedCleanly) {
    PartDocument document{"Dup"};
    Parameter& first = document.addParameter("W", 1.0, UnitType::Millimeter);
    const ObjectId id = first.id();
    const std::size_t before = document.parameters().items().size();

    EXPECT_THROW(document.restoreParameter(id, "Clash", 2.0, UnitType::Millimeter, "",
                                           ParameterState::Valid),
                 std::runtime_error);

    // Refused means UNCHANGED. Storing first and throwing afterwards left the
    // duplicate in ParameterManager, and the document then saved cleanly and
    // could never be loaded back.
    EXPECT_EQ(document.parameters().items().size(), before);
    EXPECT_EQ(document.parameters().findById(id), &first);
    std::string text;
    ASSERT_TRUE(TrySave(document, text));
    EXPECT_TRUE(LoadFromString(text));
}

TEST(SerializationV5Test, M5_REV3_011_RestoringADuplicateMaterialIdDoesNotFreeTheLiveMaterial) {
    PartDocument document{"DupMat"};
    Material& original = document.addMaterial("Steel", 7850.0);
    const ObjectId id = original.id();

    // Assigning material_ BEFORE the registration check destroyed the previous
    // Material (use_count 1) while the registry still held its address, and the
    // next recompute read the density out of freed memory.
    EXPECT_THROW(document.restoreMaterial(id, "Clash", 1234.0, 0.0, 0.0, 0.0,
                                          ContactProperties{}),
                 std::runtime_error);

    // The document must still hold the ORIGINAL material, not the rejected one.
    // Assigning material_ before the check swapped it in and destroyed the
    // original, so these three lines are what separate "refused" from
    // "half-applied, and the old object freed".
    ASSERT_TRUE(document.material());
    EXPECT_EQ(document.material().get(), &original);
    EXPECT_EQ(document.material()->name(), "Steel");
    EXPECT_DOUBLE_EQ(document.material()->density(), 7850.0);
    const ObjectRegistry::ObjectRef* ref = document.objectRegistry().find(id);
    ASSERT_NE(ref, nullptr);
    ASSERT_TRUE(std::holds_alternative<Material*>(*ref));
    // The registered handle is the LIVE material, not a freed address.
    EXPECT_EQ(*std::get_if<Material*>(ref), document.material().get());
    EXPECT_DOUBLE_EQ((*std::get_if<Material*>(ref))->density(), 7850.0);
}

TEST(SerializationV5Test, M5_REV3_012_RestoringADuplicateBodyOrFeatureIdIsRefused) {
    PartDocument document{"DupBody"};
    Body& body = document.addBody("Body001");
    EXPECT_THROW(document.restoreBody(body.id(), "Clash"), std::runtime_error);

    Parameter& w = document.addParameter("W", 10.0, UnitType::Millimeter);
    Parameter& h = document.addParameter("H", 10.0, UnitType::Millimeter);
    Parameter& d = document.addParameter("D", 10.0, UnitType::Millimeter);
    BoxFeature& box = document.addBoxFeature(body, "Box001", w.id(), h.id(), d.id());

    // ADR-M5-018 said "every restored type" and covered four of six. A
    // duplicate feature id threw nothing, left two features sharing one id in
    // the same Body, and the document saved cleanly and reloaded with
    // "duplicate ObjectId".
    EXPECT_THROW(document.restoreBoxFeature(body, box.id(), "Clash", ComputeState::Valid,
                                            w.id(), h.id(), d.id(), kInvalidObjectId),
                 std::runtime_error);

    std::string text;
    ASSERT_TRUE(TrySave(document, text));
    EXPECT_TRUE(LoadFromString(text)) << "a duplicate id reached the file";
}

TEST(SerializationV5Test, M5_REV3_013_MassPropertiesNodeStaysRegisteredAndLosesCurrency) {
    // A BOX document, not ConstrainedDoc. ConstrainedDoc has no solver, so its
    // mass properties were never valid and "expect invalid" would hold for the
    // wrong reason -- which is exactly what the first draft of this test did,
    // and a mutation proved it could not fail.
    PartDocument document{"MassNode"};
    FakeGeometryKernel kernel;
    document.setGeometryKernel(&kernel);
    document.addMaterial("Steel", 7850.0);
    Parameter& w = document.addParameter("W", 10.0, UnitType::Millimeter);
    Parameter& h = document.addParameter("H", 20.0, UnitType::Millimeter);
    Parameter& d = document.addParameter("D", 30.0, UnitType::Millimeter);
    Body& body = document.addBody("Body001");
    document.addBoxFeature(body, "Box001", w.id(), h.id(), d.id());

    ASSERT_TRUE(document.recompute().success);
    ASSERT_TRUE(document.massProperties().valid) << "the fixture must start CURRENT";

    const ObjectId nodeId = document.massPropertiesNodeId();
    ASSERT_TRUE(document.removeObject(nodeId));

    // Leaving valid == true meant the status bar went on reporting a stale
    // volume as CURRENT through every later edit.
    EXPECT_FALSE(document.massProperties().valid);

    // Re-wiring restores BOTH the graph node and the registry entry. Re-adding
    // only the graph node left a node the engine schedules but cannot resolve.
    Body& second = document.addBody("Body002");
    document.addBoxFeature(second, "Box002", w.id(), h.id(), d.id());
    EXPECT_TRUE(document.dependencyGraph().hasNode(nodeId));
    EXPECT_TRUE(document.objectRegistry().contains(nodeId));
    EXPECT_TRUE(document.recompute().success);
    EXPECT_TRUE(document.massProperties().valid);
}

TEST(SerializationV5Test, M5_REV3_014_ReconcilerNeverWiresAnEdgeItCannotRemove) {
    // The reconciler ADDED an edge for any bound id but only REMOVED Parameter
    // prerequisites. A dimension bound to the Material id -- reachable through
    // editSketch -- wired Material -> Sketch permanently: after the constraint
    // was deleted the edge survived, and a density edit kept re-solving a
    // sketch that read nothing from it. Add and remove must range over the same
    // set or they cannot agree.
    ConstrainedDoc doc;
    const ObjectId materialId = doc.document.material()->id();
    ASSERT_TRUE(doc.document.editSketch(doc.sketch->id(), [&](Sketch& s) {
        s.addConstraint(LengthConstraint{doc.top, materialId});
    }));
    doc.document.recompute();

    const std::vector<ObjectId> dependents =
        doc.document.dependencyGraph().dependentsOf(materialId);
    EXPECT_EQ(std::find(dependents.begin(), dependents.end(), doc.sketch->id()),
              dependents.end())
        << "the reconciler wired an edge from a non-Parameter it can never remove";
}

// --- Save-side guard ----------------------------------------------------------

TEST(SerializationV5Test, M5_SER_014_SketchLevelRemovalCannotProduceAnUnloadableFile) {
    ConstrainedDoc doc;
    // Sketch::removeEntity cascades too, so even this lower-level path -- the
    // one that bypasses the document facade -- cannot leave a dangling
    // reference. If the cascade were ever dropped, this save would fail rather
    // than write a file that can never be loaded back (ADR-M3-008).
    ASSERT_TRUE(doc.document.editSketch(doc.sketch->id(), [&](Sketch& s) {
        EXPECT_TRUE(s.removeEntity(doc.circle));
    }));

    std::string text;
    const SaveResult result = TrySave(doc.document, text);
    ASSERT_TRUE(result) << result.message;
    EXPECT_TRUE(LoadFromString(text));
}

} // namespace
