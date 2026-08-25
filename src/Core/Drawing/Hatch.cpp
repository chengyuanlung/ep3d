#include "Core/Drawing/Hatch.h"

#include <algorithm>
#include <cmath>

namespace paramcad {

namespace {

// WHAT COUNTS AS AN AREA, in ONE place.
//
// It was written twice -- once in empty(), once in the loop that builds the
// edges -- and two hand-copied answers to the same question is the shape of
// defect this project keeps closing. Split, they can disagree: a region
// empty() calls fillable, whose edge builder then skips every loop, comes back
// ok with nothing drawn, and an unhatched section looks exactly like an
// ordinary view of the inside of a part.
// It is worth saying that a mutation loosening this to two SURVIVES: a
// two-point loop then builds two edges that cross at the same place, every
// span between them has no width, and the empty-fill refusal at the end
// catches it anyway. The line stays because it says the rule at the point the
// rule is about, not because a test is standing behind it.
bool EnclosesArea(const std::vector<Vec2>& loop) noexcept { return loop.size() >= 3; }

} // namespace

bool HatchRegion::empty() const noexcept {
    for (const std::vector<Vec2>& loop : loops)
        if (EnclosesArea(loop)) return false;
    return true;
}

Box2D HatchRegion::bounds() const {
    Box2D box;
    for (const std::vector<Vec2>& loop : loops)
        for (const Vec2 point : loop) box.grow(point);
    return box;
}

HatchLines HatchTheRegion(const HatchRegion& region, const HatchStyle& style) {
    HatchLines out;
    if (!style.usable()) {
        out.why = "a hatch spacing of zero would draw an infinite number of lines";
        return out;
    }
    if (region.empty()) {
        out.why = "there is no closed area to hatch";
        return out;
    }

    // THE PROBLEM IS ROTATED SO THE LINES ARE HORIZONTAL, filled, and rotated
    // back. One scanline routine instead of one per angle -- and the angle
    // then costs nothing, which matters because adjacent parts in an assembly
    // section have to be hatched at different angles to be told apart.
    const double c = std::cos(-style.angleRad);
    const double s = std::sin(-style.angleRad);
    const auto intoLineSpace = [&](Vec2 p) { return Vec2{p.x * c - p.y * s, p.x * s + p.y * c}; };
    const auto backToSheet = [&](Vec2 p) {
        // The inverse rotation: the same angle the other way.
        return Vec2{p.x * c + p.y * s, -p.x * s + p.y * c};
    };

    struct Edge {
        double yLow = 0.0;
        double yHigh = 0.0;
        double xAtLow = 0.0;
        double slope = 0.0; // dx/dy
    };
    std::vector<Edge> edges;
    Box2D box;
    for (const std::vector<Vec2>& loop : region.loops) {
        if (!EnclosesArea(loop)) continue;
        for (std::size_t i = 0; i < loop.size(); ++i) {
            const Vec2 a = intoLineSpace(loop[i]);
            const Vec2 b = intoLineSpace(loop[(i + 1) % loop.size()]);
            box.grow(a);
            // A HORIZONTAL EDGE IS SKIPPED, and this is DEFENSIVE rather than
            // load-bearing -- said plainly because the comment that used to be
            // here claimed otherwise.
            //
            // It is not what stops a scanline lying along one from counting
            // twice: the half-open test below already does that, since such an
            // edge has yLow == yHigh and so is never inside [yLow, yHigh).
            // Nor is it what refuses a loop that is nothing BUT horizontal
            // edges: nothing gets filled either way, and an empty fill is
            // refused at the end. What it buys is that the table never holds a
            // slope that is a division by zero.
            if (std::fabs(a.y - b.y) < 1e-12) continue;
            Edge edge;
            edge.yLow = std::min(a.y, b.y);
            edge.yHigh = std::max(a.y, b.y);
            edge.xAtLow = a.y < b.y ? a.x : b.x;
            edge.slope = (b.x - a.x) / (b.y - a.y);
            edges.push_back(edge);
        }
    }
    if (edges.empty()) {
        out.why = "the loops given enclose no area";
        return out;
    }

    // THE LINES ARE PLACED ON AN ABSOLUTE GRID, not from the region's own
    // bottom edge. Two parts hatched separately then have their lines in step
    // where they touch, instead of each starting its pattern wherever it
    // happens to begin -- which is what makes a sectioned assembly readable.
    const double first =
        std::ceil((box.min.y - style.offsetMm) / style.spacingMm) * style.spacingMm +
        style.offsetMm;

    std::vector<double> crossings;
    for (double y = first; y <= box.max.y; y += style.spacingMm) {
        crossings.clear();
        for (const Edge& edge : edges) {
            // HALF-OPEN: [yLow, yHigh). A vertex sitting exactly on the
            // scanline belongs to one of its two edges and not both, so the
            // parity flips once. Counted twice, the fill inverts from there on
            // and the hatch appears to leak out of the part.
            if (y < edge.yLow || y >= edge.yHigh) continue;
            crossings.push_back(edge.xAtLow + (y - edge.yLow) * edge.slope);
        }
        if (crossings.size() < 2) continue;
        std::sort(crossings.begin(), crossings.end());
        // EVEN-ODD: inside between the first and second crossing, outside
        // between the second and third, and so on -- which is what makes a
        // hole a hole without anybody having to say which loop is which.
        for (std::size_t i = 0; i + 1 < crossings.size(); i += 2) {
            const double from = crossings[i];
            const double to = crossings[i + 1];
            // A SEGMENT OF NO LENGTH IS NOT A LINE. It happens where a
            // scanline grazes a corner, and drawing it leaves a dot on the
            // paper nobody put there.
            if (to - from < 1e-9) continue;
            out.segments.emplace_back(backToSheet(Vec2{from, y}), backToSheet(Vec2{to, y}));
        }
    }

    // OK MEANS THERE IS HATCH ON THE PAPER.
    //
    // Returning success with an empty list makes the caller's alarm useless:
    // the painter counts sections it could not fill so a self test can assert
    // the warning reached the screen, and an unhatched section looks exactly
    // like an ordinary view of the inside of a part. A degenerate loop -- one
    // flat line, three collinear points -- or an area narrower than the
    // spacing both land here, and both are cases where the reader would see
    // nothing and be told nothing.
    if (out.segments.empty()) {
        out.why = "nothing was filled -- the loops enclose no area at this spacing";
        return out;
    }

    out.ok = true;
    return out;
}

} // namespace paramcad
