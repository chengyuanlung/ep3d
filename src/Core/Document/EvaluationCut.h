#pragma once

#include <cstddef>

namespace paramcad {

// AN ORDERED LIST'S EVALUATION POSITION (M26, ADR-M26-001).
//
// "How far down this list are we?" -- the rule behind a feature chain's
// rollback bar, and now behind an exploded view's step preview.
//
// It is extracted because roadmap §49 point 2 names the pattern outright: the
// explode panel's own rollback bar is the THIRD appearance of "ordered steps
// plus an evaluation position", after the feature chain and feature editing,
// "which strongly suggests extracting it into a reusable mechanism rather than
// implementing it once each". This is that extraction, taken at the third
// occurrence rather than the fourth.
//
// What is shared is small and is exactly the part that is easy to get subtly
// different each time:
//
//   * a POSITION, not an edit -- nothing is removed and nothing is modified,
//     so moving it back restores what it hid, byte for byte;
//   * "all of it" is the default, and is a value rather than a special case
//     the caller has to remember to write;
//   * anything past the end CLAMPS, so a list that shrank under a stored cut
//     does not have to be found and fixed -- it simply means "all of it"
//     again, which is what it now is.
//
// What is NOT here is the list. A Body owns features; an ExplodeView owns
// steps; neither is this class's business, and holding them here would make it
// a container with a cut rather than a cut.
class EvaluationCut {
public:
    // "Evaluate everything." The default, and a value the caller can store and
    // compare rather than a flag it has to check for.
    static constexpr std::size_t kAll = static_cast<std::size_t>(-1);

    constexpr EvaluationCut() noexcept = default;
    explicit constexpr EvaluationCut(std::size_t cut) noexcept : cut_(cut) {}

    void set(std::size_t cut) noexcept { cut_ = cut; }
    // What was stored, unclamped -- what a save writes, so a cut of kAll comes
    // back as kAll rather than as however long the list happened to be.
    std::size_t stored() const noexcept { return cut_; }

    // How many of `count` are evaluated. Anything at or past the end is the
    // whole list.
    std::size_t effective(std::size_t count) const noexcept {
        return cut_ > count ? count : cut_;
    }
    // Is the item at `index` on the far side of the cut -- hidden, not run?
    bool isPast(std::size_t index, std::size_t count) const noexcept {
        return index >= effective(count);
    }

    friend bool operator==(EvaluationCut a, EvaluationCut b) noexcept { return a.cut_ == b.cut_; }
    friend bool operator!=(EvaluationCut a, EvaluationCut b) noexcept { return !(a == b); }

private:
    std::size_t cut_ = kAll;
};

} // namespace paramcad
