# EP3D Milestone M2 — Document Recompute Infrastructure

Status: **Implementation Specification + Independent Evaluation Specification**  
Target repository: `chengyuanlung/ep3d`  
Target language/toolchain: **C++20 + CMake + GoogleTest**  
Primary execution agent: **Codex**  
Required review mode: **Architect → Developer → Tester → Independent Reviewer**

---

## 1. Purpose

M2 establishes the execution backbone of the parametric CAD document.

M1 created the persistent Core data model and a generic `DependencyGraph`. M2 must connect those pieces into a real CAD document recompute pipeline without introducing Qt or OpenCASCADE into `src/Core`.

The key behavior to prove is:

```text
Parameter changes
      ↓
Document object becomes dirty
      ↓
Dirty state propagates through DependencyGraph
      ↓
Affected objects recompute in topological order
      ↓
Failures propagate downstream
      ↓
Unaffected objects are not recomputed
```

M2 is infrastructure. It is intentionally **not** a geometry milestone.

---

# 2. M2 Definition of Done

M2 is complete only when all of the following are true:

1. `PartDocument` owns or has controlled access to an object registry.
2. Stable `ObjectId` can resolve a registered Core object without scanning unrelated vectors.
3. `PartDocument` owns a document-level dependency graph.
4. A document recompute engine can:
   - mark objects dirty,
   - propagate dirtiness,
   - calculate topological execution order,
   - recompute only affected nodes,
   - stop or propagate failure correctly,
   - handle suppressed nodes deterministically.
5. CAD-specific behavior is kept outside the generic `DependencyGraph`.
6. Parameters can participate in the graph.
7. Stub/test recomputable objects can participate in the graph.
8. Deleting an object safely removes registry and graph references.
9. Save/load preserves all persistent IDs required by M2.
10. Existing M1 tests continue to pass.
11. New M2 tests pass.
12. `src/Core` remains free of Qt dependencies.
13. No OpenCASCADE types appear in `src/Core` public APIs.
14. No global mutable document registry is introduced.
15. Reviewer gives `APPROVE` or `APPROVE WITH MINOR FOLLOW-UP`; no Critical or Major architectural finding remains.

---

# 3. Explicit Non-Goals

Codex MUST NOT implement these in M2:

- Qt UI
- OpenCASCADE integration
- B-Rep solids
- real Pad/Pocket geometry
- DXF import
- Sketch constraint solving
- Assembly
- Joint / mechanism
- collision detection
- physics solver
- robot kinematics
- scripting
- plugin system
- expression parser beyond what already exists
- multithreaded recompute
- distributed/background recompute
- GPU acceleration

Do not expand M2 because a future feature seems convenient.

---

# 4. Architectural Invariants

These are hard constraints.

## 4.1 Core independence

`src/Core` MUST NOT include:

```text
Qt headers
TopoDS_*
gp_*
BRep*
AIS_*
V3d_*
```

Kernel-specific code belongs outside Core.

## 4.2 Stable identity

Persistent relationships MUST use stable `ObjectId`.

Never use:

```cpp
vector index
raw pointer address
temporary graph position
face/edge array index
```

as persistent identity.

## 4.3 DependencyGraph stays generic

`DependencyGraph` MUST NOT learn about:

```text
PartDocument
Parameter
Sketch
Pad
Pocket
Body
MassProperties
Assembly
```

It should understand graph concepts only:

```text
node ID
edge
dirty state
compute state
topological ordering
cycle detection
failure propagation
suppression
```

CAD semantics belong in the document/recompute layer.

## 4.4 No duplicated truth

The project currently has both parameter-level state and graph-level compute state.

Their meaning MUST be documented and must not silently become competing sources of truth.

Recommended rule:

```text
ParameterState
    = validity/evaluation state of a Parameter value/expression

ComputeState
    = execution state of a recomputable document node
```

Dirty propagation for document recomputation should be controlled by the document recompute system.

## 4.5 Recompute is deterministic

For the same document state and graph:

- recompute order is deterministic where dependency ordering defines it,
- unaffected nodes are not recomputed,
- failure results are deterministic,
- no hidden global state changes execution.

---

# 5. Proposed M2 Architecture

```text
PartDocument
│
├── ObjectRegistry
│      ├── registerObject()
│      ├── unregisterObject()
│      ├── find()
│      └── contains()
│
├── DependencyGraph
│      ├── nodes
│      ├── edges
│      ├── dirty propagation
│      ├── cycle detection
│      └── topological ordering
│
└── DocumentRecomputeEngine
       ├── markDirty(ObjectId)
       ├── recompute()
       ├── recompute(ObjectId root)
       ├── validateGraph()
       └── collectReport()

Document Object
│
├── ObjectId
└── optional recompute capability
       ↓
IRecomputable
       ├── recompute(context)
       └── computeState()
```

The exact class names may change after Architect review, but responsibilities MUST remain separated.

---

# 6. Recommended New Core Types

## 6.1 ObjectRegistry

Suggested location:

```text
src/Core/Document/ObjectRegistry.h
src/Core/Document/ObjectRegistry.cpp
```

Suggested responsibility:

```cpp
class ObjectRegistry
{
public:
    bool registerObject(ObjectId id, /* non-owning safe reference or controlled object handle */);
    bool unregisterObject(ObjectId id);

    bool contains(ObjectId id) const;

    // Exact return type must fit existing ownership model.
    // Do not redesign ownership casually.
    ...
};
```

### Required behavior

- duplicate `ObjectId` registration fails,
- invalid IDs fail,
- unregistering unknown ID is deterministic,
- registry lookup is expected O(1) average,
- registry is document-local,
- registry does not become an application singleton,
- registry must not own objects twice if `PartDocument` already owns them.

### Important ownership rule

Before choosing pointer/reference type, Architect MUST inspect existing M1 ownership.

Preferred order:

1. Preserve existing owner.
2. Registry stores a safe non-owning reference/handle.
3. Do not migrate the whole project to `shared_ptr` merely for M2.
4. Avoid raw pointer persistence.
5. Runtime raw pointer use may be acceptable only if lifetime is guaranteed by document ownership and never serialized.

Document the final choice in an ADR.

---

## 6.2 IRecomputable or equivalent

Suggested location:

```text
src/Core/Recompute/IRecomputable.h
```

Possible API:

```cpp
class IRecomputable
{
public:
    virtual ~IRecomputable() = default;

    virtual ObjectId id() const noexcept = 0;

    virtual RecomputeResult recompute(
        const RecomputeContext& context) = 0;
};
```

Do not force every Core object to inherit this interface if it is not logically recomputable.

A `Material` record, for example, may not need to be a recompute node.

---

## 6.3 RecomputeResult

Suggested:

```cpp
enum class RecomputeStatus
{
    Success,
    Failed,
    Skipped,
    Suppressed
};

struct RecomputeResult
{
    RecomputeStatus status;
    std::string message;
};
```

Requirements:

- no exception-only success/failure protocol,
- diagnostic text is retained for tests/logging,
- future error code extension remains possible,
- success is explicit.

---

## 6.4 RecomputeContext

Suggested:

```cpp
class RecomputeContext
{
public:
    PartDocument& document;
    ObjectRegistry& registry;

    // future extension point:
    // units, diagnostics, kernel adapter, cancellation, etc.
};
```

M2 must not add a Kernel dependency here yet.

---

## 6.5 DocumentRecomputeEngine

Suggested location:

```text
src/Core/Recompute/DocumentRecomputeEngine.h
src/Core/Recompute/DocumentRecomputeEngine.cpp
```

Core responsibility:

```text
CAD object ID
   ↓
resolve object
   ↓
ask DependencyGraph for affected/order
   ↓
execute recomputable object
   ↓
update graph compute state
   ↓
return report
```

Suggested API shape:

```cpp
class DocumentRecomputeEngine
{
public:
    explicit DocumentRecomputeEngine(PartDocument& document);

    bool markDirty(ObjectId id);

    RecomputeReport recompute();

    RecomputeReport recomputeFrom(ObjectId id);
};
```

Exact signatures can be adapted to the existing repository style.

---

## 6.6 RecomputeReport

This should exist in M2.

Example:

```cpp
struct RecomputeItemReport
{
    ObjectId id;
    RecomputeStatus status;
    std::string message;
};

struct RecomputeReport
{
    bool success;
    std::vector<RecomputeItemReport> items;
};
```

This becomes valuable later for:

- UI error display,
- Feature Tree warning icons,
- diagnostics,
- automated review,
- scripting,
- CI.

---

# 7. PartDocument Integration

`PartDocument` should become the owner/coordinator of document-level recompute infrastructure.

Conceptually:

```cpp
class PartDocument : public CadDocument
{
public:
    ObjectRegistry& objectRegistry();
    DependencyGraph& dependencyGraph();
    DocumentRecomputeEngine& recomputeEngine();

    ...
};
```

Avoid exposing uncontrolled mutable access if existing style can support safer methods.

Possible façade APIs:

```cpp
bool addDependency(ObjectId dependency, ObjectId dependent);

bool removeDependency(ObjectId dependency, ObjectId dependent);

bool markDirty(ObjectId id);

RecomputeReport recompute();
```

This is preferable for most callers because it keeps invariants inside the document.

---

# 8. Dependency Direction

M2 MUST explicitly define edge direction in one place.

Recommended semantic:

```text
A → B
```

means:

```text
B depends on A
```

Therefore:

```text
WidthParameter → Sketch001 → Pad001 → MassProperties
```

Changing `WidthParameter` propagates dirty state downstream to:

```text
Sketch001
Pad001
MassProperties
```

Never mix the inverse meaning in another subsystem.

Add tests whose names document the direction.

---

# 9. Dirty-State Lifecycle

Recommended lifecycle:

```text
Valid/Clean
   ↓ markDirty()
Dirty
   ↓ recompute success
Valid/Clean

Dirty
   ↓ recompute failure
Failed
   ↓ upstream corrected + markDirty()
Dirty
   ↓ recompute success
Valid/Clean
```

Suppressed:

```text
Suppressed
```

must have a defined rule.

Recommended M2 rule:

- a suppressed node itself is not executed,
- its report state is `Suppressed` or `Skipped`,
- downstream behavior must be explicitly defined and tested.

For M2, the safest default is:

```text
If a required dependency is suppressed and no cached valid output contract exists,
dependent recompute must not falsely report success.
```

Do not invent CAD feature suppression semantics yet. Use simple documented graph semantics.

---

# 10. Failure Propagation

Example:

```text
Parameter A
    ↓
Node B
    ↓
Node C
```

If B fails:

```text
A : success/clean
B : failed
C : blocked/failed/skipped-by-dependency
```

C MUST NOT execute as if B were valid.

M2 should distinguish:

```text
actual computation failed
```

from:

```text
not executed because dependency failed
```

if reasonably possible.

Suggested status extension:

```cpp
BlockedByDependency
```

If the existing enum should remain smaller, represent it through report diagnostics while preserving deterministic behavior.

---

# 11. Cycle Handling

The current generic graph already has cycle-related behavior.

M2 must preserve this invariant:

```text
A → B → C
```

Attempting:

```text
C → A
```

must be rejected.

Requirements:

- graph remains unchanged after rejected edge,
- meaningful status/error is returned,
- unit test proves rollback/no mutation,
- no recompute occurs on invalid cyclic graph.

---

# 12. Object Deletion Rules

Deletion is part of M2 because registry + dependency graph otherwise become unsafe.

When object `X` is deleted:

```text
PartDocument ownership
ObjectRegistry
DependencyGraph
```

must agree.

Required behavior:

1. verify X exists,
2. remove all incoming/outgoing graph edges,
3. remove graph node,
4. unregister X,
5. delete/remove owning document object,
6. no lookup returns dangling X,
7. later recompute does not access X.

Prefer a document-level operation that maintains all three structures.

Do not allow callers to delete an owned object and forget the graph.

---

# 13. Registration Rules

Any object that participates as a graph node should have one deterministic registration path.

Example:

```text
PartDocument.addParameter(...)
        ↓
create Parameter
        ↓
assign stable ObjectId
        ↓
register in ObjectRegistry
        ↓
optionally add graph node
```

Avoid scattered code such as:

```cpp
parameters.push_back(...)
registry.register(...)
graph.addNode(...)
```

performed independently by UI or future features.

The document should maintain consistency.

---

# 14. Serialization Impact

M2 does NOT need a fully general persistent graph format if the existing serializer architecture does not support it cleanly yet, but it must decide and document one of these approaches:

### Option A — Persist dependency edges now

Save:

```text
node IDs
dependency edges
suppression state if persistent
```

Reconstruct runtime recompute engine after load.

### Option B — Rebuild graph from semantic document relationships

For example:

```text
Feature input references
Parameter references
```

generate edges during load.

For long-term CAD architecture, Option B is often safer for semantically derivable edges, while explicit user-created relationships may need persistence.

M2 Architect MUST decide based on existing M1 serializer and document the choice.

Hard rule:

**Do not serialize runtime pointers or container indexes.**

---

# 15. Parameter Integration

M2 must prove at least one real existing M1 object participates.

Minimum acceptable demonstration:

```text
Parameter P
       ↓
TestRecomputable A
       ↓
TestRecomputable B
```

When P changes:

```text
P dirty event/document call
A dirty
B dirty
```

After recompute:

```text
only affected objects execute
execution order = A then B
```

If Parameters are not directly `IRecomputable`, that is acceptable.

A parameter value change can act as a dirty source node without executing a recompute function.

Document this distinction.

---

# 16. Test Double for Recompute

Do not require OpenCASCADE to test M2.

Create a simple test-only object such as:

```cpp
class CountingRecomputable : public IRecomputable
{
public:
    int recomputeCount = 0;
    bool shouldFail = false;
    std::vector<ObjectId>* executionLog = nullptr;
};
```

This allows deterministic testing of:

- execution count,
- execution order,
- failure,
- retry,
- unaffected nodes,
- propagation.

It should live under `tests/` unless a generic Core test utility is already established.

---

# 17. Required M2 Test Matrix

At minimum add a new test file such as:

```text
tests/DocumentRecomputeTests.cpp
```

and registry-specific tests if needed.

## A. ObjectRegistry

### M2-REG-001 — Register and find

```text
Given valid object ID
When registered
Then find(id) resolves the same runtime object
```

### M2-REG-002 — Reject duplicate ID

Second registration with same stable ID fails.

### M2-REG-003 — Unknown lookup

Unknown ID returns controlled "not found", never UB.

### M2-REG-004 — Unregister

After unregister:

```text
contains(id) == false
find(id) == not found
```

### M2-REG-005 — Document isolation

Two PartDocuments can contain identical runtime scenarios without sharing registry state.

---

## B. Dirty propagation

### M2-DIRTY-001 — Linear chain

```text
A → B → C
```

mark A dirty:

```text
A dirty
B dirty
C dirty
```

### M2-DIRTY-002 — Mid-chain

mark B dirty:

```text
A unchanged
B dirty
C dirty
```

### M2-DIRTY-003 — Branch

```text
      B
     /
A ---
     \
      C
```

A marks both B and C dirty.

### M2-DIRTY-004 — Unrelated node

```text
A → B
X
```

mark A dirty; X remains clean.

---

## C. Recompute ordering

### M2-ORDER-001 — Linear topological order

```text
A → B → C
```

execution log must be:

```text
A, B, C
```

for recomputable dirty nodes as applicable.

### M2-ORDER-002 — Dependency before dependent

For every executed edge A → B:

```text
index(A) < index(B)
```

if both execute.

### M2-ORDER-003 — Clean node not executed

Clean unrelated objects have recompute count unchanged.

---

## D. Failure

### M2-FAIL-001 — Node failure

B deliberately fails.

Report contains B failed.

### M2-FAIL-002 — Downstream blocked

```text
A → B → C
```

If B fails, C must not execute successfully.

### M2-FAIL-003 — Recovery

1. B fails.
2. Fix B.
3. mark dirty/retry.
4. B succeeds.
5. C can execute.
6. final report succeeds.

---

## E. Cycle

### M2-CYCLE-001 — Reject cycle

```text
A → B
B → C
```

reject:

```text
C → A
```

### M2-CYCLE-002 — Graph unchanged after rejection

Existing valid edges remain valid and no illegal edge exists.

---

## F. Deletion

### M2-DEL-001 — Delete isolated object

Registry and graph no longer contain it.

### M2-DEL-002 — Delete connected object

All incoming/outgoing edges removed.

### M2-DEL-003 — Recompute after deletion

No dangling access; remaining graph recomputes normally.

---

## G. Suppression

### M2-SUP-001 — Suppressed node not executed

### M2-SUP-002 — Downstream semantics

Behavior matches documented rule.

---

## H. Serialization / identity

### M2-SER-001 — Stable IDs survive round trip

### M2-SER-002 — Dependency relationship survives or is reconstructed

Depending on chosen persistence strategy.

### M2-SER-003 — ObjectRegistry rebuilt after load

Lookup by restored ObjectId succeeds.

---

## I. Regression

All current tests must pass:

```text
CoreSmokeTests
DependencyGraphTests
SerializationTests
```

plus new M2 tests.

---

# 18. Performance Acceptance

M2 is not an optimization milestone, but obvious O(N²) architecture should be rejected.

Minimum expectations:

- registry lookup: average O(1),
- graph traversal: O(V + E) class behavior,
- no full scan of all document containers for every `find(ObjectId)`,
- no full document recompute when only one disconnected branch is dirty.

Optional non-gating test:

Build a synthetic graph of approximately:

```text
1,000 nodes
2,000–5,000 edges
```

and ensure behavior is functionally reasonable.

Do not introduce micro-optimization at the expense of architecture.

---

# 19. Threading

M2 is single-threaded.

Hard rule:

```text
Do not add mutex/thread pool/async recompute.
```

However:

- avoid global mutable state,
- keep APIs capable of later isolation,
- do not make background execution impossible.

Parallel recompute can be considered much later.

---

# 20. Error Handling

Expected/recoverable document errors should not crash.

Use established project conventions.

Examples:

```text
duplicate ObjectId
unknown dependency node
cycle attempt
recompute failure
missing registry object
blocked dependency
```

must yield controlled return/report behavior.

Programming invariant violations may use assertions where appropriate, but user/document data errors should not rely solely on assertions.

---

# 21. Diagnostics

M2 should produce enough information for a future UI to display:

```text
Pad001 failed because Sketch001 failed
```

without re-running the computation.

At minimum report:

- ObjectId
- status
- diagnostic message

If a name is readily available through the registry/document, it may be included, but ID remains authoritative.

---

# 22. Recommended File Layout

The exact layout may adapt to existing source structure:

```text
src/Core/
├── Dependency/
│   ├── DependencyGraph.h
│   └── DependencyGraph.cpp
│
├── Document/
│   ├── ObjectRegistry.h
│   ├── ObjectRegistry.cpp
│   ├── PartDocument.h
│   └── PartDocument.cpp
│
└── Recompute/
    ├── IRecomputable.h
    ├── RecomputeContext.h
    ├── RecomputeResult.h
    ├── RecomputeReport.h
    ├── DocumentRecomputeEngine.h
    └── DocumentRecomputeEngine.cpp

tests/
├── ObjectRegistryTests.cpp
└── DocumentRecomputeTests.cpp

docs/
├── ADR/
│   ├── ADR-xxxx-object-registry.md
│   └── ADR-xxxx-document-recompute.md
└── M2_Implementation_and_Evaluation.md
```

Do not create empty abstraction files merely to match this tree. Every introduced type must have clear responsibility.

---

# 23. Mandatory ADR Decisions

Codex/Architect should create ADRs for at least:

## ADR — Object Registry Ownership

Record:

- who owns objects,
- what registry stores,
- lifetime guarantee,
- why not global registry,
- why not serialize pointer,
- duplicate ID behavior.

## ADR — Recompute State Semantics

Record:

- `ParameterState` meaning,
- `ComputeState` meaning,
- dirty owner,
- failed owner,
- retry transition.

## ADR — Dependency Persistence

Record whether graph edges are:

- persisted,
- reconstructed,
- or hybrid.

These decisions should be short but explicit.

---

# 24. Implementation Sequence

Codex MUST implement in this order unless Architect identifies a concrete reason to change it.

## Phase M2-A — Inspect and freeze current M1 contracts

1. Read:
   - `AGENTS.md`
   - `docs/Architecture.md`
   - `docs/CodingRules.md`
   - `docs/Roadmap.md`
   - `docs/OrchestratorGuide.md`
   - `docs/AgentRoles.md`
   - `docs/ReviewerGuide.md`
   - `docs/ReviewChecklist.md`
2. Inspect:
   - ObjectId implementation
   - PartDocument ownership
   - Parameter ownership/state
   - DependencyGraph semantics
   - serialization ownership
   - current tests
3. Produce a short internal architecture plan.
4. Do not code before resolving ownership semantics.

## Phase M2-B — ObjectRegistry

1. Implement registry.
2. Add unit tests.
3. Integrate with PartDocument creation/add/remove paths.
4. Verify no duplicate owner is introduced.

Gate:

```text
Registry tests pass.
Existing tests pass.
```

## Phase M2-C — Recompute contracts

Implement:

```text
IRecomputable/equivalent
RecomputeResult
RecomputeReport
RecomputeContext
```

Add test double.

Gate:

```text
No CAD-specific types added to DependencyGraph.
```

## Phase M2-D — DocumentRecomputeEngine

Implement:

```text
markDirty
affected node collection
topological execution
state updates
reporting
failure blocking
```

Gate:

```text
linear + branch + unrelated + failure tests pass.
```

## Phase M2-E — PartDocument integration

Add document façade operations.

Ensure object creation/removal maintains:

```text
owner
registry
graph
```

atomically from caller perspective.

Gate:

```text
deletion tests pass.
no dangling object is possible via public path.
```

## Phase M2-F — Parameter integration

A Parameter change must participate as a dirty source.

Do not build expression solver.

Gate:

```text
changing Parameter triggers only its dependent test nodes.
```

## Phase M2-G — Persistence/restore

Ensure restored IDs rebuild registry and graph according to chosen ADR.

Gate:

```text
round-trip test passes.
```

## Phase M2-H — Documentation

Update:

```text
README milestone status
docs/Roadmap.md
AGENTS.md current target
ADRs
```

Clearly state:

```text
MassProperties = data model only until Kernel milestone
```

unless actual calculation already exists.

## Phase M2-I — Full review

Build clean.

Run all tests.

Independent Reviewer evaluates against Section 27 below.

---

# 25. Build and Test Commands

Codex must determine the repository's actual supported commands from CMake.

Typical sequence:

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

On a single-config generator, omit `-C Debug` where appropriate.

Codex must report:

```text
configure result
build result
test count
passed count
failed count
```

Do not report "tests passed" without actually running them.

---

# 26. Required M2 Demonstration Scenario

A single integration-style test should prove the architecture.

Example synthetic CAD dependency:

```text
Width(Parameter)
      │
      ▼
SketchStub
      │
      ▼
PadStub
      │
      ▼
MassStub

Height(Parameter)
      │
      └────────────► SketchStub

Unrelated(Parameter)
      │
      ▼
OtherStub
```

Test:

1. Initial recompute.
2. Record counters.
3. Change `Width`.
4. mark Width dirty.
5. recompute.
6. Expected:

```text
SketchStub +1
PadStub    +1
MassStub   +1
OtherStub  unchanged
```

7. Force `PadStub` failure.
8. Change Width again.
9. recompute.
10. Expected:

```text
SketchStub executes
PadStub fails
MassStub does not execute successfully
report.success == false
```

11. Remove failure.
12. retry.
13. complete chain succeeds.

This scenario is a **release gate**.

---

# 27. Independent Reviewer Evaluation

Reviewer must not modify implementation during initial review.

Use this scorecard.

## A. Architecture boundaries — 20 points

- Core has no Qt dependency: 5
- Core public API has no OCC types: 5
- DependencyGraph remains generic: 5
- no global mutable registry/recompute state: 5

Any violation in this category is at least a **Major Finding**.

---

## B. Identity and ownership — 15 points

- stable ObjectId used consistently: 5
- registry ownership/lifetime is safe and documented: 5
- deletion cannot leave dangling registry/graph references: 5

Dangling lifetime risk is a **Critical Finding** if reachable by normal public API.

---

## C. Recompute correctness — 25 points

- dirty propagation correct: 5
- topological ordering correct: 5
- unaffected branches skipped: 5
- failure blocks downstream correctly: 5
- retry/recovery works: 5

---

## D. Graph integrity — 10 points

- cycle rejection preserves valid graph: 5
- add/remove dependency semantics are deterministic: 5

---

## E. Persistence and state semantics — 10 points

- stable identity restored: 4
- dependency relation restored/reconstructed: 3
- ParameterState vs ComputeState documented and coherent: 3

---

## F. Testing quality — 15 points

- required M2 matrix substantially covered: 7
- tests verify behavior, not implementation trivia: 3
- integration release-gate scenario exists: 3
- existing regression tests still pass: 2

---

## G. Documentation and maintainability — 5 points

- ADRs exist: 2
- README/Roadmap/AGENTS milestone updated: 2
- code naming/comments make dependency direction clear: 1

---

## Total: 100 points

Decision thresholds:

```text
90–100  APPROVE
80–89   APPROVE WITH MINOR FOLLOW-UP
70–79   REQUEST CHANGES
<70     REQUEST CHANGES
```

Override rules:

- any Critical Finding → `REQUEST CHANGES`
- unresolved Major architectural invariant violation → `REQUEST CHANGES`
- build failure → `REQUEST CHANGES`
- any existing test regression → `REQUEST CHANGES`
- fake/missing test execution → `REQUEST CHANGES`

A score above 80 does NOT override these rules.

---

# 28. Reviewer Finding Severity

## Critical

Examples:

- normal operation can dereference dangling object,
- IDs are persisted as pointer/index,
- graph corruption after rejected cycle,
- silent data loss,
- Core architecture fundamentally bypassed.

## Major

Examples:

- Core adds Qt/OCC dependency,
- DependencyGraph becomes CAD-specific,
- all nodes recompute despite disconnected graph,
- failure does not block dependent node,
- duplicated state source causes inconsistent document,
- deletion leaves stale graph nodes.

## Minor

Examples:

- diagnostic wording,
- incomplete comments,
- missing optional convenience API,
- non-critical test naming,
- small README inconsistency.

---

# 29. Exact Reviewer Output Format

Reviewer must output:

```text
# M2 Independent Review

Decision:
APPROVE | APPROVE WITH MINOR FOLLOW-UP | REQUEST CHANGES

Score:
XX / 100

## Build
Configure:
Build:
Tests:
Passed:
Failed:

## Critical Findings
- None
or
- ...

## Major Findings
- None
or
- ...

## Minor Findings
- None
or
- ...

## Architecture Invariants
Core free of Qt: PASS/FAIL
Core public API free of OCC: PASS/FAIL
Stable ObjectId: PASS/FAIL
DependencyGraph generic: PASS/FAIL
Registry document-local: PASS/FAIL
Deletion safe: PASS/FAIL
Dirty propagation: PASS/FAIL
Topological recompute: PASS/FAIL
Failure propagation: PASS/FAIL
Cycle rejection: PASS/FAIL
Persistence identity: PASS/FAIL
ParameterState/ComputeState semantics: PASS/FAIL

## Test Coverage
REG:
DIRTY:
ORDER:
FAIL:
CYCLE:
DEL:
SUP:
SER:
REGRESSION:
INTEGRATION:

## Required Changes
1. ...
2. ...

## Suggested Follow-Up
- ...

## M3 Readiness
READY | NOT READY
Reason:
...
```

---

# 30. M2 Completion Report Required From Codex

Developer/Orchestrator must produce:

```text
# M2 Completion Report

## Implemented
- ...

## Files Added
- ...

## Files Modified
- ...

## Architecture Decisions
- ...

## Build
- command
- result

## Tests
- total
- passed
- failed

## Release Gate Scenario
- result

## Reviewer
- agent/runtime identity if available
- independent sub-agent used: YES/NO
- decision
- score

## Known Limitations
- ...

## Deferred to M3
- ...

## Final Status
M2 COMPLETE | M2 NOT COMPLETE
```

If the runtime cannot spawn an independent Reviewer agent, it must state:

```text
Independent sub-agent review unavailable.
Sequential reviewer role was used.
```

Never claim independent review if none occurred.

---

# 31. Codex Execution Prompt

Copy the following prompt to Codex from the repository root:

```text
Implement Milestone M2 for this repository.

First read AGENTS.md and follow its orchestration rules.

Then read:
- docs/Architecture.md
- docs/CodingRules.md
- docs/Roadmap.md
- docs/OrchestratorGuide.md
- docs/AgentRoles.md
- docs/ReviewerGuide.md
- docs/ReviewChecklist.md
- docs/M2_Implementation_and_Evaluation.md

M2 scope is strictly:
1. document-local ObjectRegistry,
2. DocumentRecomputeEngine (or equivalent),
3. integration of the existing DependencyGraph into PartDocument,
4. stable ObjectId-based lookup and safe removal,
5. dirty propagation and topological incremental recompute,
6. failure propagation and deterministic retry,
7. parameter dirty-source integration,
8. persistence/restore required by the M2 architecture,
9. tests and ADR documentation.

Do NOT add Qt or OpenCASCADE to src/Core.
Do NOT implement real CAD geometry, Sketch solver, DXF, Assembly, Collision, Robot, Physics, or multithreaded recompute.
Do NOT make DependencyGraph CAD-specific.

Before coding, the Architect role must inspect the existing ownership model and decide:
- registry ownership/reference strategy,
- ParameterState vs ComputeState semantics,
- dependency persistence strategy.

Implement in the phased order defined by docs/M2_Implementation_and_Evaluation.md.

Add all mandatory M2 tests, including the required integration release-gate scenario.

Build from a clean configuration and run the complete test suite.

Then run an independent Reviewer agent if the environment supports sub-agents.
The Reviewer must use the scorecard and exact output format in the M2 document.
If sub-agents are unavailable, use the documented sequential fallback and explicitly say independent review was unavailable.

Do not declare M2 complete unless:
- build succeeds,
- all existing and new tests pass,
- no Critical finding remains,
- no Major architectural finding remains,
- the release-gate scenario passes,
- documentation and ADRs are updated.

Finally produce the M2 Completion Report required by the M2 document.
```

---

# 32. Recommended Commit Structure

Prefer several reviewable commits instead of one huge commit.

Example:

```text
M2: add document-local object registry
M2: add recompute contracts and report
M2: integrate document recompute engine
M2: connect parameter dirty propagation
M2: make deletion graph/registry safe
M2: restore dependency infrastructure after load
M2: add recompute integration tests
M2: update ADRs and milestone documentation
```

Do not force this exact history if the environment works differently, but keep conceptual changes reviewable.

---

# 33. What M2 Enables

After M2, the project should be able to support M3 without architectural redesign.

M3 can then introduce a geometry-kernel abstraction:

```text
Parameter
   ↓
Sketch/Profile model
   ↓
Feature
   ↓
Kernel Adapter
   ↓
Shape
   ↓
Mass Properties
```

M2 must make this possible while remaining independent of the actual kernel.

The eventual desired flow is:

```text
Width = 100 mm
        ↓
change to 120 mm
        ↓
Parameter dirty
        ↓
Sketch dirty
        ↓
Pad dirty
        ↓
MassProperties dirty
        ↓
incremental recompute
        ↓
only affected branch executes
```

M2 proves the dependency/recompute infrastructure before expensive CAD geometry is introduced.

---

# 34. Final M2 Gate Checklist

Codex and Reviewer should both check this before completion:

```text
[ ] ObjectRegistry exists and is document-local
[ ] ObjectId lookup does not scan unrelated containers
[ ] duplicate ID rejected
[ ] safe unregister/delete implemented
[ ] DependencyGraph remains generic
[ ] PartDocument integrates dependency/recompute infrastructure
[ ] dirty propagation works
[ ] topological recompute works
[ ] unrelated nodes are not recomputed
[ ] failures block downstream
[ ] retry after fix works
[ ] cycles rejected without graph corruption
[ ] suppressed behavior documented and tested
[ ] Parameter can act as dirty source
[ ] stable IDs survive save/load
[ ] registry rebuilt after load
[ ] dependencies survive/reconstruct after load
[ ] ParameterState vs ComputeState documented
[ ] Core has no Qt
[ ] Core public API has no OCC
[ ] all previous tests pass
[ ] all M2 tests pass
[ ] integration release-gate test passes
[ ] ADRs added
[ ] README updated
[ ] Roadmap updated
[ ] AGENTS current target updated
[ ] independent Reviewer completed where supported
[ ] Reviewer score >= 80
[ ] no Critical finding
[ ] no unresolved Major architectural finding
```

Only then:

```text
M2 = COMPLETE
M3 = READY
```
