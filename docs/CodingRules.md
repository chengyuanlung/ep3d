# Coding Rules v0.1

1. C++20; no compiler extensions in Core unless documented.
2. Core headers may include only standard-library and Core headers.
3. No Qt types in Core.
4. No OpenCASCADE types in Core.
5. Persistent references use typed IDs, never container indices.
6. Prefer value types and RAII; avoid owning raw pointers.
7. Every mutating user operation should eventually be represented by a Command.
8. Derived caches must be invalidatable and reconstructible.
9. Geometry/kernel failures return structured errors; do not terminate the process.
10. Public APIs document coordinate frame and units.
11. Angles in Core APIs are radians unless the API name explicitly says Degrees.
12. Add a unit test for every geometry-independent Core bug fix.
13. Avoid hidden global state.
14. Serialization is versioned.
15. Commit small, reviewable changes.
