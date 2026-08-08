#include "Core/Dependency/DependencyGraph.h"
#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace paramcad {

namespace {

void eraseId(std::vector<ObjectId>& ids, ObjectId id) {
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
}

bool containsId(const std::vector<ObjectId>& ids, ObjectId id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

DependencyGraph::Node* DependencyGraph::findNode(ObjectId id) noexcept {
    auto it = nodes_.find(id);
    return it != nodes_.end() ? &it->second : nullptr;
}

const DependencyGraph::Node* DependencyGraph::findNode(ObjectId id) const noexcept {
    auto it = nodes_.find(id);
    return it != nodes_.end() ? &it->second : nullptr;
}

GraphResult DependencyGraph::addNode(ObjectId id) {
    if (id == kInvalidObjectId) return {GraphError::NodeNotFound};
    if (nodes_.count(id) != 0) return {GraphError::NodeAlreadyExists};
    nodes_.emplace(id, Node{});
    insertionOrder_.push_back(id);
    return {};
}

GraphResult DependencyGraph::removeNode(ObjectId id) {
    Node* node = findNode(id);
    if (node == nullptr) return {GraphError::NodeNotFound};

    const std::vector<ObjectId> formerDependents = node->dependents;
    for (ObjectId prereq : node->prerequisites)
        eraseId(findNode(prereq)->dependents, id);
    for (ObjectId dependent : node->dependents)
        eraseId(findNode(dependent)->prerequisites, id);
    nodes_.erase(id);
    eraseId(insertionOrder_, id);

    for (ObjectId dependent : formerDependents)
        markDirty(dependent);
    return {};
}

bool DependencyGraph::hasNode(ObjectId id) const noexcept {
    return nodes_.count(id) != 0;
}

std::size_t DependencyGraph::nodeCount() const noexcept {
    return nodes_.size();
}

bool DependencyGraph::reaches(ObjectId from, ObjectId to) const {
    std::unordered_set<ObjectId> visited;
    std::vector<ObjectId> stack{from};
    while (!stack.empty()) {
        const ObjectId current = stack.back();
        stack.pop_back();
        if (current == to) return true;
        if (!visited.insert(current).second) continue;
        const Node* node = findNode(current);
        assert(node != nullptr);
        for (ObjectId dependent : node->dependents)
            stack.push_back(dependent);
    }
    return false;
}

GraphResult DependencyGraph::addDependency(ObjectId dependent, ObjectId prerequisite) {
    Node* dependentNode = findNode(dependent);
    Node* prerequisiteNode = findNode(prerequisite);
    if (dependentNode == nullptr || prerequisiteNode == nullptr)
        return {GraphError::NodeNotFound};
    if (dependent == prerequisite) return {GraphError::WouldCreateCycle};
    if (containsId(prerequisiteNode->dependents, dependent)) return {}; // duplicate: no-op
    // Adding prerequisite -> dependent closes a cycle iff dependent already
    // reaches prerequisite downstream.
    if (reaches(dependent, prerequisite)) return {GraphError::WouldCreateCycle};
    prerequisiteNode->dependents.push_back(dependent);
    dependentNode->prerequisites.push_back(prerequisite);
    // The dependent's inputs changed: invalidate it (propagates transitively).
    markDirty(dependent);
    return {};
}

GraphResult DependencyGraph::removeDependency(ObjectId dependent, ObjectId prerequisite) {
    Node* dependentNode = findNode(dependent);
    Node* prerequisiteNode = findNode(prerequisite);
    if (dependentNode == nullptr || prerequisiteNode == nullptr)
        return {GraphError::NodeNotFound};
    if (!containsId(prerequisiteNode->dependents, dependent)) return {}; // absent edge: no-op
    eraseId(prerequisiteNode->dependents, dependent);
    eraseId(dependentNode->prerequisites, prerequisite);
    // The dependent's inputs changed: invalidate it (propagates transitively).
    markDirty(dependent);
    return {};
}

std::vector<ObjectId> DependencyGraph::dependentsOf(ObjectId id) const {
    const Node* node = findNode(id);
    return node != nullptr ? node->dependents : std::vector<ObjectId>{};
}

std::vector<ObjectId> DependencyGraph::prerequisitesOf(ObjectId id) const {
    const Node* node = findNode(id);
    return node != nullptr ? node->prerequisites : std::vector<ObjectId>{};
}

ComputeState DependencyGraph::state(ObjectId id) const {
    const Node* node = findNode(id);
    assert(node != nullptr && "DependencyGraph::state: unknown node id");
    if (node == nullptr) return ComputeState::Failed;
    return node->state;
}

GraphResult DependencyGraph::setState(ObjectId id, ComputeState s) {
    Node* node = findNode(id);
    if (node == nullptr) return {GraphError::NodeNotFound};
    node->state = s;
    return {};
}

GraphResult DependencyGraph::markDirty(ObjectId id) {
    Node* node = findNode(id);
    if (node == nullptr) return {GraphError::NodeNotFound};

    if (node->state != ComputeState::Suppressed)
        node->state = ComputeState::Dirty;

    // Mark all transitive dependents Dirty, passing through Suppressed nodes
    // without changing their state.
    std::unordered_set<ObjectId> visited{id};
    std::vector<ObjectId> stack{id};
    while (!stack.empty()) {
        const ObjectId current = stack.back();
        stack.pop_back();
        for (ObjectId dependent : findNode(current)->dependents) {
            if (!visited.insert(dependent).second) continue;
            Node* dependentNode = findNode(dependent);
            if (dependentNode->state != ComputeState::Suppressed)
                dependentNode->state = ComputeState::Dirty;
            stack.push_back(dependent);
        }
    }
    return {};
}

GraphResult DependencyGraph::setSuppressed(ObjectId id, bool suppressed) {
    Node* node = findNode(id);
    if (node == nullptr) return {GraphError::NodeNotFound};
    node->state = suppressed ? ComputeState::Suppressed : ComputeState::Dirty;
    return {};
}

std::vector<ObjectId> DependencyGraph::topologicalOrder() const {
    // Kahn-style order; among ready nodes, the earliest-inserted is chosen so
    // ties are broken deterministically by insertion order.
    std::unordered_map<ObjectId, std::size_t> remaining;
    remaining.reserve(nodes_.size());
    for (ObjectId id : insertionOrder_)
        remaining[id] = findNode(id)->prerequisites.size();

    std::vector<ObjectId> order;
    order.reserve(insertionOrder_.size());
    std::unordered_set<ObjectId> placed;
    while (order.size() < insertionOrder_.size()) {
        bool found = false;
        for (ObjectId id : insertionOrder_) {
            if (placed.count(id) != 0 || remaining[id] != 0) continue;
            order.push_back(id);
            placed.insert(id);
            for (ObjectId dependent : findNode(id)->dependents)
                --remaining[dependent];
            found = true;
            break;
        }
        assert(found && "DependencyGraph: cycle detected in topologicalOrder");
        if (!found) break; // unreachable if the cycle-free invariant holds
    }
    return order;
}

RecomputeReport DependencyGraph::recompute(const ComputeFn& fn) {
    assert(fn && "DependencyGraph::recompute: null compute function");
    RecomputeReport report;
    if (!fn) return report;

    // touched: node is Dirty or has a touched prerequisite (affected region).
    // failing: failure flows out of the node this pass (Failed here, or a
    // Suppressed node passing an upstream failure through).
    std::unordered_set<ObjectId> touched;
    std::unordered_set<ObjectId> failing;

    for (ObjectId id : topologicalOrder()) {
        Node* node = findNode(id);
        bool upstreamFailed = false;
        bool upstreamTouched = false;
        for (ObjectId prereq : node->prerequisites) {
            // A prerequisite gates this node when it failed this pass OR its
            // persisted state is Failed from an earlier pass: a Failed node is
            // a downstream barrier until it is re-marked Dirty.
            const Node* prereqNode = findNode(prereq);
            if (failing.count(prereq) != 0 || prereqNode->state == ComputeState::Failed)
                upstreamFailed = true;
            if (touched.count(prereq) != 0) upstreamTouched = true;
        }
        const bool isTouched = node->state == ComputeState::Dirty || upstreamTouched;
        if (isTouched) touched.insert(id);

        if (node->state == ComputeState::Suppressed) {
            // Never invoked, stays Suppressed; failure passes through.
            if (upstreamFailed) failing.insert(id);
            if (isTouched) report.skippedSuppressed.push_back(id);
            continue;
        }
        if (upstreamFailed) {
            // Always cascade the gate; report/change state only when the node
            // actually transitions (it was Dirty), so idle passes do not
            // re-report long-standing failures.
            failing.insert(id);
            if (node->state == ComputeState::Dirty) {
                node->state = ComputeState::Failed;
                report.failed.push_back(id);
            }
            continue;
        }
        if (node->state != ComputeState::Dirty) continue; // Valid/Failed: not invoked

        bool success = false;
        try {
            success = fn(id);
        } catch (...) {
            success = false; // never let a callback exception escape recompute
        }
        if (success) {
            node->state = ComputeState::Valid;
            report.recomputed.push_back(id);
        } else {
            node->state = ComputeState::Failed;
            report.failed.push_back(id);
            failing.insert(id);
        }
    }
    return report;
}

} // namespace paramcad
