#include "Solver/GaussNewtonSketchSolver.h"

#include <Eigen/Dense>
#include <Eigen/SVD>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace paramcad {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Angle wrapped into [-pi, pi].
double WrapToPi(double radians) noexcept { return std::remainder(radians, 2.0 * kPi); }

// Whether a residual's VALUE lives on a circle. Only the derivative of such a
// residual needs special handling, and only inside ComputeJacobian.
constexpr bool IsAngular(SolveResidual::Kind kind) noexcept {
    return kind == SolveResidual::Kind::Angle;
}

// Below this, two points are "the same point" as far as a distance residual's
// derivative is concerned.
constexpr double kDegenerateSeparationMm = 1e-12;


using Vec = Eigen::VectorXd;
using Mat = Eigen::MatrixXd;

bool AllFinite(const Vec& v) noexcept {
    for (int i = 0; i < v.size(); ++i)
        if (!std::isfinite(v[i])) return false;
    return true;
}

double ValueAt(const Vec& x, int index) noexcept {
    return index >= 0 && index < x.size() ? x[index] : 0.0;
}

// One residual's value at the given configuration.
//
// NOTE: the comment that stood here described `sin(actual - target)` for Angle,
// which ADR-M5-006 superseded -- and it went on describing it after the code
// changed. On a project that has already shipped a Critical because two
// comments described an algorithm the code did not implement, a comment left
// describing the REMOVED algorithm is the same hazard pointing the other way.
// Angle is now the wrapped angular difference; see the Angle case below.
double Evaluate(const SolveResidual& r, const Vec& x) {
    switch (r.kind) {
        case SolveResidual::Kind::PointsEqualU:
        case SolveResidual::Kind::PointsEqualV:
        case SolveResidual::Kind::LineHorizontal:
        case SolveResidual::Kind::LineVertical:
        case SolveResidual::Kind::RadiiEqual:
            return ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[0]);

        case SolveResidual::Kind::FixedU:
        case SolveResidual::Kind::FixedV:
            return ValueAt(x, r.vars[0]) - r.target;

        case SolveResidual::Kind::Radius:
        case SolveResidual::Kind::EllipseRotation:
            return ValueAt(x, r.vars[0]) - r.target;

        case SolveResidual::Kind::TangentLineEllipse: {
            // (p.u, p.v, q.u, q.v, c.u, c.v, a, b, rot)
            //
            // The line's UNIT NORMAL, rotated into the ellipse's own frame,
            // and the signed distance from the centre to the line along it.
            // Tangency is h^2 == a^2 nu^2 + b^2 nv^2 -- see the kind's comment
            // in ISketchSolver.h for why this form and not |h| - R.
            const double du = ValueAt(x, r.vars[2]) - ValueAt(x, r.vars[0]);
            const double dv = ValueAt(x, r.vars[3]) - ValueAt(x, r.vars[1]);
            const double length = std::sqrt(du * du + dv * dv);
            const double a = ValueAt(x, r.vars[6]);
            const double b = ValueAt(x, r.vars[7]);
            // A line with no direction has no normal, and an ellipse with no
            // extent has no tangent. Zero rather than a division: both are
            // refused before they get here, and a NaN would poison the whole
            // Jacobian rather than just this row.
            if (length < kDegenerateSeparationMm || std::fabs(a) < kDegenerateSeparationMm ||
                std::fabs(b) < kDegenerateSeparationMm)
                return 0.0;
            const double nu = -dv / length;
            const double nv = du / length;
            const double rot = ValueAt(x, r.vars[8]);
            const double cosR = std::cos(rot);
            const double sinR = std::sin(rot);
            // Into the ellipse's frame. The normal is a DIRECTION, so it
            // rotates by -rot and does not translate.
            const double nMajor = nu * cosR + nv * sinR;
            const double nMinor = -nu * sinR + nv * cosR;
            // From the centre to the line, along the normal. Measured from the
            // line's first end, which is a point on the line.
            const double toCentreU = ValueAt(x, r.vars[4]) - ValueAt(x, r.vars[0]);
            const double toCentreV = ValueAt(x, r.vars[5]) - ValueAt(x, r.vars[1]);
            const double h = toCentreU * nu + toCentreV * nv;
            const double reach = a * a * nMajor * nMajor + b * b * nMinor * nMinor;
            return (h * h - reach) / (a * a + b * b);
        }

        case SolveResidual::Kind::Distance:
        case SolveResidual::Kind::Length: {
            const double du = ValueAt(x, r.vars[2]) - ValueAt(x, r.vars[0]);
            const double dv = ValueAt(x, r.vars[3]) - ValueAt(x, r.vars[1]);
            return std::sqrt(du * du + dv * dv) - r.target;
        }

        case SolveResidual::Kind::Angle: {
            // Slots: A.start.u, A.start.v, A.end.u, A.end.v,
            //        B.start.u, B.start.v, B.end.u, B.end.v.
            const double duA = ValueAt(x, r.vars[2]) - ValueAt(x, r.vars[0]);
            const double dvA = ValueAt(x, r.vars[3]) - ValueAt(x, r.vars[1]);
            const double duB = ValueAt(x, r.vars[6]) - ValueAt(x, r.vars[4]);
            const double dvB = ValueAt(x, r.vars[7]) - ValueAt(x, r.vars[5]);

            // The WRAPPED angular difference, not sin(). sin() is smooth
            // everywhere, which is why an earlier revision chose it -- but its
            // period is pi, so it cannot tell 30 degrees from 210, and telling
            // those apart is the entire justification ADR-M5-006 gives for a
            // DIRECTED angle. A residual that is smooth but measures the wrong
            // thing is worse than one with a single removable discontinuity.
            //
            // wrap() is smooth except exactly at a half-turn from the target,
            // where it jumps between +pi and -pi. That point is a measure-zero
            // starting configuration and the solve converges from either side
            // of it; in exchange the residual is zero exactly at the requested
            // angle (mod 2*pi) and has derivative 1 there, which is what makes
            // the rank -- and therefore the DOF -- come out right.
            // std::remainder, not a while-loop. `while (d > kPi) d -= 2*kPi`
            // is unbounded, and DimensionValueValid accepts ANY finite angle:
            // an Angle Parameter of 1e300 rad made recompute() never return,
            // because 2*pi is below the ULP of 1e300. 1e9 rad took 21.8 s.
            // remainder is O(1) and exact for every finite input.
            // The TARGET is wrapped first, then subtracted. Subtracting the raw
            // value and wrapping the whole expression loses the geometry
            // entirely once the target is large: at 1e9 rad the ULP of the
            // target exceeds the angular signal, the residual stops depending
            // on the coordinates, and the solver reported "constraints are
            // contradictory: no configuration satisfies them" for a system that
            // solves perfectly when the mathematically identical pre-wrapped
            // target is used. ADR-M5-014's own rule -- a false accusation of
            // contradiction is the damaging kind of wrong answer -- applies to
            // the very fix that replaced the unbounded-loop hang.
            return WrapToPi(std::atan2(dvB, duB) - std::atan2(dvA, duA) -
                            WrapToPi(r.target));
        }

        case SolveResidual::Kind::PointsDeltaU:
        case SolveResidual::Kind::PointsDeltaV:
            // SIGNED: b - a - target, never |b - a| - target. The absolute
            // form has no derivative at zero, which is exactly where a
            // horizontal separation between two vertically-aligned points
            // starts.
            return ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[0]) - r.target;

        case SolveResidual::Kind::LinesParallel:
        case SolveResidual::Kind::LinesPerpendicular:
        case SolveResidual::Kind::LengthsEqual: {
            // (A.start.u, A.start.v, A.end.u, A.end.v, B.start.u, ...)
            const double duA = ValueAt(x, r.vars[2]) - ValueAt(x, r.vars[0]);
            const double dvA = ValueAt(x, r.vars[3]) - ValueAt(x, r.vars[1]);
            const double duB = ValueAt(x, r.vars[6]) - ValueAt(x, r.vars[4]);
            const double dvB = ValueAt(x, r.vars[7]) - ValueAt(x, r.vars[5]);
            const double lengthA = std::sqrt(duA * duA + dvA * dvA);
            const double lengthB = std::sqrt(duB * duB + dvB * dvB);
            // LengthsEqual is in MILLIMETRES and needs no normalising -- a line
            // of zero length is a legitimate thing for it to measure.
            if (r.kind == SolveResidual::Kind::LengthsEqual) return lengthB - lengthA;
            // The other two are NORMALISED by both lengths, so each is the sine
            // or cosine of the angle between the lines: dimensionless, bounded
            // by 1, and zero exactly at the relationship it names. The
            // un-normalised cross and dot products have units of mm^2 and a
            // magnitude that grows with the lines, so one tolerance could not
            // serve a 1 mm pair and a 100 mm pair at once.
            if (lengthA < kDegenerateSeparationMm || lengthB < kDegenerateSeparationMm)
                return 0.0;
            return r.kind == SolveResidual::Kind::LinesParallel
                       ? (duA * dvB - dvA * duB) / (lengthA * lengthB)
                       : (duA * duB + dvA * dvB) / (lengthA * lengthB);
        }

        case SolveResidual::Kind::SplineHandleTipU:
        case SolveResidual::Kind::SplineHandleTipV:
            // (tip, base, delta) in one component -- the tip of a spline handle
            // IS its point plus its tangent, and this is that sentence as an
            // equation. Linear, so its Jacobian row is constant and it never
            // goes rank-deficient.
            return ValueAt(x, r.vars[0]) - ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[2]);

        case SolveResidual::Kind::MidpointU:
        case SolveResidual::Kind::MidpointV:
            // (p, start, end) in one component.
            return ValueAt(x, r.vars[0]) -
                   0.5 * (ValueAt(x, r.vars[1]) + ValueAt(x, r.vars[2]));

        case SolveResidual::Kind::ArcTipU:
        case SolveResidual::Kind::ArcTipV: {
            // (tip, centre, radius, angle)
            const double tip = ValueAt(x, r.vars[0]);
            const double centre = ValueAt(x, r.vars[1]);
            const double radius = ValueAt(x, r.vars[2]);
            const double angle = ValueAt(x, r.vars[3]);
            const double along = r.kind == SolveResidual::Kind::ArcTipU ? std::cos(angle)
                                                                       : std::sin(angle);
            return tip - (centre + radius * along);
        }

        case SolveResidual::Kind::EllipseTipU:
        case SolveResidual::Kind::EllipseTipV: {
            // (tip, centre, a, b, rot, t)
            //
            // A rotated ellipse MIXES the axes, so unlike the circular arc's
            // tip these two are different equations rather than one with cos
            // and sin swapped:
            //
            //   u: c.u + a cos t cos rot - b sin t sin rot
            //   v: c.v + a cos t sin rot + b sin t cos rot
            const double tip = ValueAt(x, r.vars[0]);
            const double centre = ValueAt(x, r.vars[1]);
            const double major = ValueAt(x, r.vars[2]);
            const double minor = ValueAt(x, r.vars[3]);
            const double rotation = ValueAt(x, r.vars[4]);
            const double t = ValueAt(x, r.vars[5]);
            const double along = major * std::cos(t);
            const double across = minor * std::sin(t);
            const double c = std::cos(rotation);
            const double sn = std::sin(rotation);
            const double offset = r.kind == SolveResidual::Kind::EllipseTipU
                                      ? along * c - across * sn
                                      : along * sn + across * c;
            return tip - (centre + offset);
        }

        case SolveResidual::Kind::PointOnEllipseImplicit: {
            // (p.u, p.v, c.u, c.v, a, b, rot)
            const double du = ValueAt(x, r.vars[0]) - ValueAt(x, r.vars[2]);
            const double dv = ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[3]);
            const double major = ValueAt(x, r.vars[4]);
            const double minor = ValueAt(x, r.vars[5]);
            const double rotation = ValueAt(x, r.vars[6]);
            if (std::fabs(major) < kDegenerateSeparationMm ||
                std::fabs(minor) < kDegenerateSeparationMm)
                return 0.0;
            const double c = std::cos(rotation);
            const double sn = std::sin(rotation);
            // Into the ellipse's own frame.
            const double along = (du * c + dv * sn) / major;
            const double across = (-du * sn + dv * c) / minor;
            // THE SQUARE ROOT FIRST, then minus one. The obvious form --
            // (along^2 + across^2 - 1) -- is the same zero set and a much worse
            // residual: it grows as the SQUARE of how far off the point is, so
            // its gradient is enormous far away and its curvature dominates
            // every step. Levenberg-Marquardt stalled on it from a start 30 mm
            // off a 40x15 ellipse, having spent seven iterations rotating the
            // ellipse instead of moving the point.
            //
            // Rooted, the quantity is "how many times the ellipse's own radius
            // in this direction the point is", which grows LINEARLY -- and
            // scaled by the mean radius it is comparable to the millimetres
            // every other positional residual here is measured in, so one
            // tolerance serves a 1 mm ellipse and a 100 mm one.
            //
            // It is still NOT the true distance to the ellipse -- that is the
            // root of a quartic -- and it does not claim to be. It is zero
            // exactly on the curve, which is what a residual has to be.
            const double reach = std::sqrt(along * along + across * across);
            // The CENTRE is the one place the direction is undefined. Zero
            // there rather than a division; the point is pulled by whatever
            // else holds it, and a NaN would poison the whole Jacobian.
            if (reach < kDegenerateSeparationMm) return 0.0;
            // THE RADIAL DISTANCE: how far the point is from where the ray out
            // of the centre crosses the ellipse. `reach` is how many of that
            // ray's own radii the point sits at, so `d/reach` IS that crossing's
            // distance, and the difference is a length in millimetres.
            //
            // Scaling by the mean radius instead -- the previous version --
            // measures the same zero set in units that are only millimetres for
            // a circle. On a 40x15 ellipse it reads 31 for a point 12 mm off
            // the curve, and the solver spent its whole budget crawling.
            const double d = std::sqrt(du * du + dv * dv);
            return d - d / reach;
        }

        case SolveResidual::Kind::SymmetricAcross:
        case SolveResidual::Kind::SymmetricAlong: {
            const double du = ValueAt(x, r.vars[6]) - ValueAt(x, r.vars[4]);
            const double dv = ValueAt(x, r.vars[7]) - ValueAt(x, r.vars[5]);
            const double length = std::sqrt(du * du + dv * dv);
            // A mirror with no direction has no sides to be on. Zero rather
            // than a division: the constraint is refused when the line is
            // degenerate, and a NaN here would poison the whole Jacobian.
            if (length < kDegenerateSeparationMm) return 0.0;
            const double au = ValueAt(x, r.vars[0]) - ValueAt(x, r.vars[4]);
            const double av = ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[5]);
            const double bu = ValueAt(x, r.vars[2]) - ValueAt(x, r.vars[4]);
            const double bv = ValueAt(x, r.vars[3]) - ValueAt(x, r.vars[5]);
            if (r.kind == SolveResidual::Kind::SymmetricAcross) {
                // Signed distances SUM to zero: same size, opposite sides. The
                // difference would say "the same side", which is what a
                // coincidence already says better.
                return ((au * dv - av * du) + (bu * dv - bv * du)) / length;
            }
            // Square to the line: the two have the same projection ALONG it.
            return ((bu - au) * du + (bv - av) * dv) / length;
        }

        case SolveResidual::Kind::PointLineDistance:
        case SolveResidual::Kind::PointOnLine: {
            // (p.u, p.v, a.u, a.v, b.u, b.v)
            const double du = ValueAt(x, r.vars[4]) - ValueAt(x, r.vars[2]);
            const double dv = ValueAt(x, r.vars[5]) - ValueAt(x, r.vars[3]);
            const double pu = ValueAt(x, r.vars[0]) - ValueAt(x, r.vars[2]);
            const double pv = ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[3]);
            const double length = std::sqrt(du * du + dv * dv);
            if (length < kDegenerateSeparationMm) return 0.0;
            // Signed perpendicular distance, in mm -- the same units as every
            // other positional residual, so one tolerance covers them all.
            // `target` is 0 for PointOnLine, so ONE formula serves both and
            // there is no second place for the sign convention to drift.
            return (pu * dv - pv * du) / length - r.target;
        }

        case SolveResidual::Kind::PointOnCircle: {
            // (p.u, p.v, c.u, c.v, r)
            const double du = ValueAt(x, r.vars[0]) - ValueAt(x, r.vars[2]);
            const double dv = ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[3]);
            return std::sqrt(du * du + dv * dv) - ValueAt(x, r.vars[4]);
        }

        case SolveResidual::Kind::TangentLineCircle: {
            // (a.u, a.v, b.u, b.v, c.u, c.v, r)
            const double du = ValueAt(x, r.vars[2]) - ValueAt(x, r.vars[0]);
            const double dv = ValueAt(x, r.vars[3]) - ValueAt(x, r.vars[1]);
            const double cu = ValueAt(x, r.vars[4]) - ValueAt(x, r.vars[0]);
            const double cv = ValueAt(x, r.vars[5]) - ValueAt(x, r.vars[1]);
            const double length = std::sqrt(du * du + dv * dv);
            if (length < kDegenerateSeparationMm) return 0.0;
            const double distance = std::fabs(cu * dv - cv * du) / length;
            return distance - ValueAt(x, r.vars[6]);
        }

        case SolveResidual::Kind::TangentAtPoint: {
            // (touch.u, touch.v, far.u, far.v, c.u, c.v, r)
            //
            // PERPENDICULARITY, not distance. The touch point is already held
            // on the curve by a coincidence, so what is left to say is that the
            // line leaves it at a right angle to the radius -- and unlike the
            // distance form above, that has somewhere to go when it is wrong.
            const double du = ValueAt(x, r.vars[2]) - ValueAt(x, r.vars[0]);
            const double dv = ValueAt(x, r.vars[3]) - ValueAt(x, r.vars[1]);
            const double ru = ValueAt(x, r.vars[0]) - ValueAt(x, r.vars[4]);
            const double rv = ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[5]);
            const double length = std::sqrt(du * du + dv * dv);
            const double radius = ValueAt(x, r.vars[6]);
            // A line with no direction has no angle to make, and a circle with
            // no radius has no tangent. Zero rather than a division: the
            // constraint is refused before it gets here, and a NaN would poison
            // the whole Jacobian rather than just this row.
            if (length < kDegenerateSeparationMm || std::fabs(radius) < kDegenerateSeparationMm)
                return 0.0;
            return (du * ru + dv * rv) / (length * radius);
        }

        case SolveResidual::Kind::TangentCurvesAtPoint: {
            // (touch.u, touch.v, c1.u, c1.v, c2.u, c2.v, r1, r2)
            //
            // COLLINEAR RADII, which is what tangency is once the touch point
            // is known. The centre-distance forms below are true at the same
            // configuration and hold nothing there, because they grow as the
            // square of this angle rather than as the angle.
            const double au = ValueAt(x, r.vars[0]) - ValueAt(x, r.vars[2]);
            const double av = ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[3]);
            const double bu = ValueAt(x, r.vars[0]) - ValueAt(x, r.vars[4]);
            const double bv = ValueAt(x, r.vars[1]) - ValueAt(x, r.vars[5]);
            const double r1 = ValueAt(x, r.vars[6]);
            const double r2 = ValueAt(x, r.vars[7]);
            // A curve with no radius has no tangent. Zero rather than a
            // division, for the reason every other guard here gives: the
            // constraint is refused before it arrives, and a NaN would poison
            // the whole Jacobian instead of one row.
            if (std::fabs(r1) < kDegenerateSeparationMm ||
                std::fabs(r2) < kDegenerateSeparationMm)
                return 0.0;
            return (au * bv - av * bu) / (r1 * r2);
        }

        case SolveResidual::Kind::TangentCirclesOuter:
        case SolveResidual::Kind::TangentCirclesInner: {
            // (c1.u, c1.v, r1, c2.u, c2.v, r2)
            const double du = ValueAt(x, r.vars[3]) - ValueAt(x, r.vars[0]);
            const double dv = ValueAt(x, r.vars[4]) - ValueAt(x, r.vars[1]);
            const double centres = std::sqrt(du * du + dv * dv);
            const double r1 = ValueAt(x, r.vars[2]);
            const double r2 = ValueAt(x, r.vars[5]);
            return r.kind == SolveResidual::Kind::TangentCirclesOuter
                       ? centres - (r1 + r2)
                       : centres - std::fabs(r1 - r2);
        }
    }
    return 0.0;
}

void EvaluateAll(const std::vector<SolveResidual>& residuals, const Vec& x, Vec& out) {
    for (std::size_t i = 0; i < residuals.size(); ++i)
        out[static_cast<int>(i)] = Evaluate(residuals[i], x);
}

// Jacobian by central differences.
//
// Analytic derivatives would be faster, but this milestone's systems are tiny
// (a constrained rectangle is 8 variables) and a hand-derived Jacobian is
// exactly the kind of code where a sign error produces a solver that converges
// to the wrong answer while every test that checks "did it converge" passes.
// Central differences are second-order accurate and cannot disagree with the
// residuals they are differentiating.
void ComputeJacobian(const std::vector<SolveResidual>& residuals, const Vec& x, Mat& jacobian) {
    Vec probe = x;
    Vec plus(static_cast<int>(residuals.size()));
    Vec minus(static_cast<int>(residuals.size()));

    for (int j = 0; j < x.size(); ++j) {
        // Step scaled to the variable's magnitude so it stays meaningful for
        // both a 0.001 mm coordinate and a 1000 mm one.
        const double step = 1e-7 * std::max(1.0, std::fabs(x[j]));
        probe[j] = x[j] + step;
        EvaluateAll(residuals, probe, plus);
        probe[j] = x[j] - step;
        EvaluateAll(residuals, probe, minus);
        probe[j] = x[j];

        for (std::size_t i = 0; i < residuals.size(); ++i) {
            const int row = static_cast<int>(i);
            double difference = plus[row] - minus[row];
            // An angular residual's VALUE jumps by 2*pi across the wrap point;
            // its GRADIENT does not. Differencing two independently-wrapped
            // values straddling that point yields a Jacobian entry of about
            // 2*pi/(2h) -- enormous and wrong -- so Levenberg-Marquardt rejects
            // every step and the solve stalls at iteration 1.
            //
            // Wrapping the DIFFERENCE instead recovers the true derivative: the
            // step is tiny, so the real change is tiny, and any 2*pi in it came
            // from the wrap rather than from the geometry.
            //
            // This replaces a nudge that rotated the starting guess off the
            // antipode. That nudge used a fixed 1e-6 rad guard band and a fixed
            // 1e-4 rad rotation, while the residual's usable angular resolution
            // is 1e-7*|coordinate|/length -- SCALE-DEPENDENT. Over a 760-case
            // grid it improved 131 configurations and BROKE 42, turning correct
            // Solved results into false "did not converge" failures for short
            // lines far from the sketch origin. Fixing the derivative instead
            // of dodging the point needs no tuning constant at all.
            if (IsAngular(residuals[i].kind)) difference = WrapToPi(difference);
            jacobian(row, j) = difference / (2.0 * step);
        }
    }
}

// Numerical rank via a rank-revealing decomposition with an explicit threshold.
// This is what makes DOF a measurement rather than `variables - constraints`
// (ADR-M5-005): a redundant-but-consistent constraint adds a row without adding
// rank, and a doubly-constrained direction shows up as rank deficiency.
int NumericalRank(const Mat& jacobian) {
    if (jacobian.rows() == 0 || jacobian.cols() == 0) return 0;
    Eigen::ColPivHouseholderQR<Mat> qr(jacobian);
    qr.setThreshold(kSolveRankThreshold);
    return static_cast<int>(qr.rank());
}

// WHICH variables the constraints did not pin down (M17.29).
//
// The rank above says how many freedoms are left. This says which ones, and it
// has to be the SAME question or a sketch could report DOF 0 while an entity
// was still coloured loose: a variable is free exactly when the Jacobian's null
// space has a component along it, and the number of such directions IS the
// nullity that the rank measures.
//
// An SVD rather than the QR used for the rank, because the rank only needs a
// count and this needs the null space itself. Both use the same relative
// threshold on the same matrix, so they agree by construction.
//
// The threshold is on the COLUMN of V, not on the singular value alone: a
// variable with a tiny component in a null direction is not meaningfully free,
// and rounding noise puts a tiny component almost everywhere.
std::vector<bool> FreeVariables(const Mat& jacobian, int variableCount) {
    std::vector<bool> free(static_cast<std::size_t>(variableCount), false);
    if (variableCount == 0) return free;
    if (jacobian.rows() == 0) {
        // NOTHING CONSTRAINS ANYTHING, so everything is free. Not a special
        // case so much as the honest reading of an empty constraint set -- and
        // the SVD below has no columns to give.
        free.assign(static_cast<std::size_t>(variableCount), true);
        return free;
    }

    Eigen::JacobiSVD<Mat> svd(jacobian, Eigen::ComputeFullV);
    const Vec singular = svd.singularValues();
    const double largest = singular.size() > 0 ? singular(0) : 0.0;
    // The same relative cut the rank uses, so "how many are free" and "which
    // are free" cannot come to different answers.
    const double cut = kSolveRankThreshold * std::max(largest, 1.0);

    const Mat& v = svd.matrixV();
    for (int column = 0; column < v.cols(); ++column) {
        const bool nullDirection =
            column >= singular.size() || singular(column) <= cut;
        if (!nullDirection) continue;
        for (int row = 0; row < v.rows() && row < variableCount; ++row)
            if (std::fabs(v(row, column)) > 1e-6)
                free[static_cast<std::size_t>(row)] = true;
    }
    return free;
}

// Constraints whose residuals are still violated, for diagnostics. Reported in
// ascending id order so the message is deterministic.
std::vector<SketchConstraintId> OffendersOf(const std::vector<SolveResidual>& residuals,
                                            const Vec& values) {
    std::set<ObjectId> ids;
    for (std::size_t i = 0; i < residuals.size(); ++i) {
        if (std::fabs(values[static_cast<int>(i)]) <= kSolveResidualTolerance) continue;
        if (residuals[i].sourceConstraint == kInvalidSketchConstraintId) continue;
        ids.insert(ToObjectId(residuals[i].sourceConstraint));
    }
    std::vector<SketchConstraintId> out;
    out.reserve(ids.size());
    for (ObjectId id : ids) out.push_back(static_cast<SketchConstraintId>(id));
    return out;
}

SketchSolveResult Failure(const SketchSolveProblem& problem, SketchSolveStatus status,
                          std::string message) {
    SketchSolveResult result;
    // Values left at the initial configuration: a caller that ignores the
    // status still cannot commit garbage, though the contract is that nothing
    // is committed unless the result converts to true (ADR-M5-004).
    result.values = problem.initialValues;
    result.status = status;
    result.message = std::move(message);
    return result;
}

} // namespace

SketchSolveResult GaussNewtonSketchSolver::solve(const SketchSolveProblem& problem) {
    const int variableCount = static_cast<int>(problem.variables.size());
    const int residualCount = static_cast<int>(problem.residuals.size());

    if (variableCount == 0)
        return Failure(problem, SketchSolveStatus::InvalidInput, "no variables to solve for");
    if (problem.initialValues.size() != problem.variables.size())
        return Failure(problem, SketchSolveStatus::InvalidInput,
                       "initial value count does not match variable count");

    Vec x(variableCount);
    for (int i = 0; i < variableCount; ++i) {
        x[i] = problem.initialValues[static_cast<std::size_t>(i)];
        if (!std::isfinite(x[i]))
            return Failure(problem, SketchSolveStatus::InvalidInput,
                           "initial configuration contains a non-finite value");
    }

    for (const SolveResidual& residual : problem.residuals) {
        if (!std::isfinite(residual.target))
            return Failure(problem, SketchSolveStatus::InvalidInput,
                           "a constraint carries a non-finite target value");

        // Every slot the kind's formula reads must actually be filled, and must
        // name a real variable. Without this, a residual packed with fewer
        // variables than its formula consumes reads index -1, silently
        // evaluates something that is not the constraint, and converges to a
        // wrong answer while reporting Solved -- which is exactly how the Angle
        // residual shipped broken and passed 444 tests.
        const int required = SlotsRequired(residual.kind);
        // A KIND NOBODY DECLARED AN ARITY FOR. This used to be indistinguishable
        // from "needs no variables": the loop below simply did not run, and the
        // guard quietly stopped guarding. Seven kinds lived in that gap.
        if (required == kUndeclaredArity)
            return Failure(problem, SketchSolveStatus::InvalidInput,
                           "a constraint uses an equation whose arity was never declared");
        for (int slot = 0; slot < required; ++slot) {
            const int index = residual.vars[static_cast<std::size_t>(slot)];
            if (index < 0 || index >= variableCount)
                return Failure(problem, SketchSolveStatus::InvalidInput,
                               "a constraint is missing a variable its equation needs");

            // ...and the variable must be the KIND of scalar the formula reads.
            // Arity alone let a mis-ORDERED Distance through -- all four slots
            // filled and in range -- which then reported Solved with a tiny
            // residual and geometry wrong by millimetres.
            if (!SlotAccepts(residual.kind, slot,
                             problem.variables[static_cast<std::size_t>(index)].component))
                return Failure(problem, SketchSolveStatus::InvalidInput,
                               "a constraint's variables are packed in the wrong order");
        }
    }

    // EVERYTHING BELOW INDEXES x[] BY SLOT, so it must come AFTER the guard.
    //
    // The degeneracy nudge was originally placed above it and read/wrote
    // `x[r.vars[2]]` raw. An under-packed Distance -- exactly what the guard
    // exists to refuse -- was dereferenced first: an assertion abort in Debug,
    // and a silent out-of-bounds read AND WRITE in Release, through an
    // interface whose header promises it never throws. The fix for one finding
    // re-opened the hole another finding's guard had just closed. Order is the
    // whole protection here, so it is stated rather than left to reading order.
    // Nudge a Distance/Length whose two points START exactly coincident.
    //
    // Its residual is sqrt(du^2 + dv^2) - target, whose central difference at
    // du = dv = 0 is |+h| - |-h| = 0: the entire Jacobian row is zeros, there
    // is no descent direction, and the rank test then reported CONFLICTING
    // ("no configuration satisfies them") for a system with an INFINITE
    // solution set -- any point at the requested distance will do. A false
    // accusation of contradiction is the damaging kind of wrong answer,
    // because no constraint is actually in conflict and the user has nothing
    // to act on. Two coincident points arrive by ordinary means: snapping,
    // duplication, a collapsed edit, an imported file.
    //
    // This perturbs the solver's STARTING GUESS, never stored geometry -- the
    // initial values are the solver's input, and any direction is as good as
    // any other for a configuration that constrains none.
    for (const SolveResidual& r : problem.residuals) {
        if (r.kind != SolveResidual::Kind::Distance && r.kind != SolveResidual::Kind::Length)
            continue;
        const double du = x[r.vars[2]] - x[r.vars[0]];
        const double dv = x[r.vars[3]] - x[r.vars[1]];
        if (std::hypot(du, dv) > kDegenerateSeparationMm) continue;
        const double nudge = std::max(kDegenerateSeparationMm, 1e-6 * std::fabs(r.target));
        x[r.vars[2]] += nudge; // break the tie along +u; direction is arbitrary
    }

    Vec residuals(residualCount);
    Mat jacobian(residualCount, variableCount);
    EvaluateAll(problem.residuals, x, residuals);
    if (!AllFinite(residuals))
        return Failure(problem, SketchSolveStatus::InvalidInput,
                       "the initial configuration produces a non-finite residual");

    // DIMENSIONLESS, and multiplied by the size of J'J below.
    //
    // It used to be an absolute number added straight to the diagonal, which
    // means nothing on its own: 1e-6 is heavy damping for a problem measured in
    // microns and none at all for one measured in metres. Worse, it could not
    // GROW enough. Under-constrained sketches are rank-deficient by
    // construction, so J'J is singular and LDLT hands back a step along the
    // null space that can be arbitrarily large; taming one needs damping
    // comparable to J'J, and twelve tenfold increases from a decayed 1e-12
    // could never reach it. A point constrained onto an ellipse stalled there
    // with a perfectly good descent direction available.
    double damping = 1e-9;
    int iteration = 0;
    bool stopEarly = false; // step size below tolerance, or no step accepted

    // Levenberg-Marquardt: Gauss-Newton with a damping term that grows when a
    // step is rejected. Plain Gauss-Newton diverges on rank-deficient systems,
    // which under-constrained sketches are BY CONSTRUCTION -- so the damping is
    // not a refinement here, it is what makes the ordinary case work.
    // NO INCREMENT IN THE HEADER. There is one at the bottom of the body, and
    // having both counted every pass TWICE: a solve that took three passes
    // reported six, and the limit of 100 was really a limit of fifty. The
    // second increment was added to stop the early exits skipping the header's
    // -- the fix for that is to count in one place, not in two.
    for (; iteration < kSolveMaxIterations;) {
        if (residuals.lpNorm<Eigen::Infinity>() <= kSolveResidualTolerance) break;

        ComputeJacobian(problem.residuals, x, jacobian);
        if (!jacobian.allFinite())
            return Failure(problem, SketchSolveStatus::NumericalFailure,
                           "the Jacobian became non-finite during iteration");

        const Mat jtj = jacobian.transpose() * jacobian;
        const Vec jtr = jacobian.transpose() * residuals;

        // The SIZE of the normal equations, so the damping below is a fraction
        // of them rather than a number in whatever units the sketch happens to
        // be in.
        const double normalScale = std::max(jtj.diagonal().maxCoeff(), 1e-12);

        bool stepAccepted = false;
        // TWENTY-FOUR attempts, not twelve: the search has to be able to span
        // from "no damping" to "so damped the step is a whisper", and each one
        // is a tenfold increase.
        for (int attempt = 0; attempt < 24 && !stepAccepted; ++attempt) {
            Mat damped = jtj;
            damped.diagonal().array() += damping * normalScale;
            const Vec step = damped.ldlt().solve(-jtr);
            if (!AllFinite(step)) {
                damping *= 10.0;
                continue;
            }

            const Vec candidate = x + step;
            if (!AllFinite(candidate)) {
                damping *= 10.0;
                continue;
            }

            Vec candidateResiduals(residualCount);
            EvaluateAll(problem.residuals, candidate, candidateResiduals);
            if (!AllFinite(candidateResiduals)) {
                damping *= 10.0;
                continue;
            }

            if (candidateResiduals.squaredNorm() <= residuals.squaredNorm()) {
                x = candidate;
                residuals = candidateResiduals;
                damping = std::max(damping / 10.0, 1e-12);
                stepAccepted = true;
                // Stop, but do not LIE about how far we got. Setting
                // iteration = kSolveMaxIterations to break out made a solve
                // that settled in 3 steps report 100, and made the failure
                // message blame an iteration limit the solver never reached.
                if (step.lpNorm<Eigen::Infinity>() <= kSolveStepTolerance) stopEarly = true;
            } else {
                damping *= 10.0;
            }
        }

        // Counted BEFORE the early-stop breaks, and ONLY here: a solve that
        // completed one accepted step used to report 0, because these exits
        // skipped an increment that lived in the loop header.
        ++iteration;
        if (!stepAccepted) {
            stopEarly = true; // no damping value improved the residual
            break;
        }
        if (stopEarly) break;
    }

    SketchSolveResult result;
    result.values.assign(x.data(), x.data() + x.size());
    result.iterations = std::min(iteration, kSolveMaxIterations);
    const bool hitIterationLimit = !stopEarly && iteration >= kSolveMaxIterations;
    result.maxResidual = residuals.lpNorm<Eigen::Infinity>();

    if (!AllFinite(x))
        return Failure(problem, SketchSolveStatus::NumericalFailure,
                       "the solved configuration contains a non-finite value");

    // Rank at the SOLVED configuration, which is where the DOF question is
    // actually being asked.
    ComputeJacobian(problem.residuals, x, jacobian);
    const int rank = NumericalRank(jacobian);
    result.degreesOfFreedom = std::max(0, variableCount - rank);
    result.variableIsFree = FreeVariables(jacobian, variableCount);

    const bool converged = result.maxResidual <= kSolveResidualTolerance;
    const bool redundant = rank < residualCount;

    if (!converged) {
        // The DOF measured above describes the configuration the solver stopped
        // at, which is not a solution -- so it says nothing about the sketch's
        // freedom, and 0 would read as "fully constrained". The header promises
        // a failed result carries kUnknownDegreesOfFreedom; only the Failure()
        // paths honoured it, and this one did not.
        result.degreesOfFreedom = kUnknownDegreesOfFreedom;
        // ...and WHICH variables are free is unknown for the same reason. An
        // empty list is the only honest answer; a stale one would colour the
        // sketch as though the failed solve had measured something.
        result.variableIsFree.clear();
        // Did not converge. Rank-deficient in the direction of the violation
        // means contradictory constraints; otherwise the solver simply failed
        // to get there. Borderline cases resolve toward Conflicting, because
        // reporting a real conflict as benign is the more damaging error
        // (ADR-M5-005).
        result.status =
            redundant ? SketchSolveStatus::Conflicting : SketchSolveStatus::NumericalFailure;
        result.offendingConstraints = OffendersOf(problem.residuals, residuals);
        // Say which one actually happened. Reporting an iteration limit for a
        // solver that stalled after one step sends the reader looking for a
        // slow solve that never occurred.
        result.message =
            redundant ? "constraints are contradictory: no configuration satisfies them"
                      : (hitIterationLimit
                             ? "the solver did not converge within the iteration limit"
                             : "the solver could not improve on its last step");
        return result;
    }

    // DOF OUTRANKS REDUNDANCY. A sketch that still has free degrees is
    // under-constrained, whatever else is true of it. Reporting OverConstrained
    // here -- as an earlier revision did, because the redundancy branch came
    // first -- told the user "too many constraints" about a sketch that needs
    // MORE, and hid the free degrees behind a status that implies none remain.
    // Redundancy only becomes the headline once there is no freedom left.
    if (result.degreesOfFreedom > 0) {
        result.status = SketchSolveStatus::UnderConstrained;
        if (redundant)
            result.message = "under-constrained, and some constraints are redundant";
        return result;
    }

    if (redundant) {
        // Converged despite redundant rows: the extra constraints agree with
        // the others. Accepted and committed, not rejected -- rejecting would
        // make ordinary modelling fail for no user-visible reason (ADR-M5-005).
        result.status = SketchSolveStatus::OverConstrained;
        result.message = "constraints are redundant but consistent";
        return result;
    }

    result.status = SketchSolveStatus::Solved;
    return result;
}

} // namespace paramcad
