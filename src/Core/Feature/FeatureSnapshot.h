#pragma once

#include "Core/Document/ObjectId.h"
#include "Core/Kernel/EdgeQuery.h"
#include "Core/Feature/ComputeState.h"
#include <string>

namespace paramcad {

class Body;
class Feature;
class PartDocument;

// The SEMANTIC description of one feature: its identity, its name, its type,
// its compute state, and every reference it holds -- each an ObjectId. Nothing
// else. No KernelShape, no OCCT handle, no container position, no address.
//
// WHY THIS EXISTS, AND WHY IT IS NOT A THIRD ENUMERATION (M9.1).
//
// Two places already knew how to turn a feature into semantic fields and back:
// the serializer's save-side field writer and its load-side restore dispatch.
// M9's undo history needs exactly the same knowledge -- an undone deletion has
// to rebuild the feature it removed, with its ORIGINAL ObjectId and all its
// references intact -- and writing a third copy of that per-type knowledge
// would reproduce this project's second recurring defect class: a table pinned
// for some of its members with a comment claiming all of them (found in the
// solid-type table in review round 3, and again in the consumer table in round
// 4, inside the very commit that fixed the first).
//
// So this is the shared unit, and the serializer's LOAD path was rewritten to
// use it rather than to duplicate it. The count of per-type enumerations in
// the project goes DOWN by adopting this, not up.
//
// A per-type field is meaningful only for the type the snapshot declares --
// `depthParameterId` serves both Box and Pocket, exactly as it did in the
// serializer's own record, because a record only ever answers for its own type.
struct FeatureSnapshot {
    ObjectId id = kInvalidObjectId;
    std::string name;
    std::string typeName;
    ComputeState state = ComputeState::Dirty;

    // Box
    ObjectId widthParameterId = kInvalidObjectId;
    ObjectId heightParameterId = kInvalidObjectId;
    ObjectId depthParameterId = kInvalidObjectId; // shared with Pocket
    // Every solid-producing type
    ObjectId materialId = kInvalidObjectId; // kInvalidObjectId == "no material"
    // Pad / Pocket / Revolve
    ObjectId sketchId = kInvalidObjectId;
    ObjectId lengthParameterId = kInvalidObjectId;
    // Pocket / Fillet / Chamfer -- the chain reference (ADR-M8-001)
    ObjectId baseFeatureId = kInvalidObjectId;
    // Revolve
    ObjectId axisEntityId = kInvalidObjectId;
    ObjectId angleParameterId = kInvalidObjectId;
    // Fillet / Chamfer
    ObjectId sizeParameterId = kInvalidObjectId;
    // Fillet/Chamfer only: WHICH edges (M17.12, ADR-M17-034). Defaults to
    // every edge, which is what a snapshot from before selections existed --
    // and every file written before v18 -- says by not mentioning them.
    EdgeSelection edgeSelection = AllEdgesSelection();
    // Mirror / Pattern (M10.6): the frame that supplies the plane or the axis,
    // and the pattern's two driving Parameters.
    ObjectId frameId = kInvalidObjectId;
    ObjectId countParameterId = kInvalidObjectId;
    ObjectId spacingParameterId = kInvalidObjectId;
};

// Reads a feature's semantic fields. Type dispatch is keyed by `typeName()`
// (ADR-M3-005); the `dynamic_cast`s reach the already-known concrete type's
// accessors, they do not decide which type it is.
//
// A type this function does not know yields a snapshot carrying only the
// common fields -- which is correct for PlaceholderFeature and is a silent
// hole for anything else, so: WHEN YOU ADD A FEATURE TYPE, ADD IT HERE, and
// add it to `M9_UNDO_401`, which round-trips every type in
// `kSolidFeatureTypeNames` through snapshot and back.
FeatureSnapshot SnapshotFeature(const Feature& feature);

// Rebuilds a feature from its snapshot, through the document's own restore
// facade -- so the registry entry, the graph node and every dependency edge
// are wired exactly as they are on load, and the duplicate-id guard applies.
//
// Throws std::runtime_error on an invariant violation, exactly as the restore
// paths do. An unknown type name restores a PlaceholderFeature, which is the
// loader's own rule for a record it does not recognise.
Feature& RestoreFeatureFromSnapshot(PartDocument& document, Body& body,
                                    const FeatureSnapshot& snapshot);

} // namespace paramcad
