#pragma once

#include "Core/Sketch/SketchConstraint.h"
#include <array>
#include "Core/Sketch/SketchTypes.h"
#include <cstddef>
#include <string>
#include <vector>

namespace paramcad {

// The solver boundary (ADR-M5-003). Everything in this header is Core-side and
// free of Eigen and of any backend type: the problem carries variables and
// residual definitions, the result carries values, a status and a DOF.
//
// Eigen appears in exactly one translation unit behind this interface. Swapping
// in a different backend (PlaneGCS, SolveSpace) means writing a second
// ISketchSolver and changing nothing else -- the same shape as IGeometryKernel
// and its OCCT implementation (ADR-M3-001).

// What a variable in the solve corresponds to. Solver variable INDICES are an
// internal numbering rebuilt on every solve and are never persisted
// (ADR-M5-001) -- this struct is how an index is mapped back to semantics
// afterwards.
struct SolveVariable {
    SketchEntityId entityId{kInvalidSketchEntityId};
    SketchSubElement subElement{SketchSubElement::Whole};
    // WHICH ONE, where the sub-element alone does not say -- a spline's handles
    // are all SplineHandle, and there is one per handled point (M18).
    //
    // Carried rather than recovered from the order the variables were made.
    // A spline's POINTS are recovered by order, and that works only because
    // there is one per point with no gaps; handles are sparse, so an order that
    // silently skipped one would write every later handle onto the wrong point.
    int index{0};
    // Which scalar of that sub-element: u/v for points, or the radius of a
    // circle or arc.
    // U/V for points, the radius of a circle or arc, and -- since M17 -- the
    // two angles that bound an arc's sweep.
    // WHICH SCALAR of the entity this variable is.
    //
    // An ellipse's MinorRadius and Rotation are their own components rather
    // than more Radius and more StartAngle, and its two ends carry StartParam /
    // EndParam rather than StartAngle / EndAngle. The names are not decoration:
    // this enum is what the solver's packing guard compares against, so a
    // parameter packed where an angle belongs is caught -- and for an ellipse
    // the two are genuinely different numbers (see SketchEllipticalArc).
    enum class Component {
        U,
        V,
        Radius,
        StartAngle,
        EndAngle,
        MinorRadius,
        Rotation,
        StartParam,
        EndParam,
        // M18 -- a spline handle. HandleU/V are the tangent as a VECTOR
        // relative to its point, which is the state that is stored and the
        // thing that stays put when the point moves.
        //
        // The handle's TIP -- the same tangent as an absolute point, p + m --
        // is an ordinary U/V variable, exactly as an arc's tips are, and for
        // the same reason: a tip that is an ordinary point is something every
        // constraint which already holds a point can hold, so a handle can be
        // made horizontal, or parallel to a line, or given an angle, without a
        // single new residual for each. It is told apart from a spline's actual
        // points by its SUB-ELEMENT, which is SplineHandle.
        //
        // Giving the tips components of their own was tried first and is what
        // the packing guard is for: every point constraint refused them,
        // because a Distance reads Component::V and found something else.
        HandleU,
        HandleV
    } component{Component::U};
};

// One scalar equation the solver must drive to zero. Residuals are supplied as
// definitions rather than callbacks so the problem stays a plain value type
// that can be built, inspected and compared in tests without a solver present.
// "Not measured". A failed solve learns nothing new about the sketch's freedom,
// and 0 is this project's signal for FULLY CONSTRAINED (spec 10, Gate A) -- so
// a sketch whose very first solve failed must not report 0, which is what a
// zero-initialised member did.
inline constexpr int kUnknownDegreesOfFreedom = -1;

struct SolveResidual {
    enum class Kind {
        PointsEqualU,   // a.u - b.u                                    (2 slots)
        PointsEqualV,   // a.v - b.v                                    (2 slots)
        LineHorizontal, // end.v - start.v                              (2 slots)
        LineVertical,   // end.u - start.u                              (2 slots)
        FixedU,         // point.u - target                             (1 slot)
        FixedV,         // point.v - target                             (1 slot)
        Distance,       // |b - a| - target                             (4 slots)
        Length,         // |end - start| - target                       (4 slots)
        Radius,         // radius - target                              (1 slot)
        // wrap(atan2(dvB, duB) - atan2(dvA, duA) - target) into (-pi, pi],
        // where each line's direction is end - start (ADR-M5-006). Needs BOTH
        // components of BOTH endpoints of BOTH lines: 8 slots.
        Angle,

        // --- M13: the geometric constraints -------------------------------
        //
        // The two line-pair kinds below are NORMALISED by both lengths, so each
        // is the sine or cosine of the angle between the lines: dimensionless,
        // bounded by 1, and zero exactly at the relationship it names. The
        // un-normalised cross and dot products would have units of mm^2 and a
        // magnitude that grows with the lines -- so a residual tolerance chosen
        // for millimetres would accept a 100 mm pair that is visibly not
        // parallel while rejecting a 1 mm pair that is.
        LinesParallel,       // cross(dirA, dirB) / (|A| |B|)          (8 slots)
        LinesPerpendicular,  // dot(dirA, dirB)  / (|A| |B|)           (8 slots)
        LengthsEqual,        // |B| - |A|                              (8 slots)
        RadiiEqual,          // rB - rA                                (2 slots)
        MidpointU,           // p.u - (start.u + end.u)/2              (3 slots)
        MidpointV,           // p.v - (start.v + end.v)/2              (3 slots)
        // Signed perpendicular distance from the point to the INFINITE line
        // through the segment's endpoints, normalised by the line's length.
        PointOnLine,         //                                        (6 slots)
        PointOnCircle,       // |P - C| - r                            (5 slots)
        // |signed distance from centre to line| - r. The absolute value has a
        // kink exactly where the centre lies ON the line, which is a
        // measure-zero configuration and never the solution (r > 0); using the
        // signed distance instead would silently pin the line to one SIDE of
        // the circle, turning "tangent" into "tangent from the left".
        TangentLineCircle,   //                                        (7 slots)

        // TANGENT WHERE THE TOUCH POINT IS ALREADY KNOWN (M17.21).
        //
        // TangentLineCircle above is the right equation when the line is free
        // to slide -- and the WRONG one the moment a coincidence has already
        // pinned an end of the line onto the curve. There the perpendicular
        // distance from the centre cannot EXCEED the radius: the residual sits
        // at a maximum, its derivative vanishes in every direction, and the
        // constraint removes no freedom at all while looking perfectly
        // satisfied. A slot built that way reported DOF 9 instead of 5, and a
        // fillet's tangency held its corner smooth by luck rather than by
        // constraint.
        //
        // Said at a KNOWN point, tangency is perpendicularity instead: the
        // line's direction is square to the radius drawn to that point.
        //
        //   dot(far - touch, touch - centre) / (|far - touch| * r)
        //
        // Normalised by both lengths, so it is the cosine of the angle between
        // them: dimensionless, bounded by 1, zero exactly at tangency -- and
        // with a gradient that does not vanish there.
        //
        // Slots: touch.u touch.v far.u far.v centre.u centre.v r. THE TOUCH
        // POINT COMES FIRST, which is the whole difference from the layout
        // above. Packing a line's other end where its touching end belongs
        // constrains the wrong corner, and SlotAccepts cannot catch it because
        // both are a U -- so the session reads which end from the constraint's
        // stored `at`, never from whichever end currently looks closer.
        TangentAtPoint,      //                                        (7 slots)

        TangentCirclesOuter, // |C1 - C2| - (r1 + r2)                  (6 slots)
        TangentCirclesInner, // |C1 - C2| - |r1 - r2|                  (6 slots)

        // TWO CURVES TOUCHING AT A KNOWN POINT (M17.22).
        //
        // The pair above has exactly the disease TangentAtPoint cures, for
        // exactly the same reason. Two circles that SHARE a point already obey
        // |r1 - r2| <= |C1 - C2| <= r1 + r2 -- that is the triangle inequality
        // on the touch point -- so the outer residual sits at a maximum and the
        // inner one at a minimum, and both have a vanishing gradient there.
        // Near tangency each grows only as the SQUARE of the angle between the
        // two radii, which is what a zero first derivative means.
        //
        // The angle itself is the honest measure. At a shared point, tangency
        // is the two radii being COLLINEAR:
        //
        //   cross(P - C1, P - C2) / (r1 * r2)
        //
        // the sine of the angle between them: dimensionless, bounded by 1, and
        // with a derivative of 1 exactly where it matters.
        //
        // NO INTERNAL/EXTERNAL BRANCH, and it does not need one. Collinear
        // covers both -- centres on the same side of P and on opposite sides --
        // and which of the two a sketch is in is settled by where it already
        // is, continuously, not by a flag the solver would have to obey. The
        // branch matters for the pair above precisely BECAUSE those residuals
        // cannot see the touch point.
        //
        // Slots: touch.u touch.v c1.u c1.v c2.u c2.v r1 r2 -- POSITIONS FIRST
        // and both radii at the end, rather than each radius beside its own
        // centre. The tidier-looking grouping puts a Radius at slot 4, which
        // flips the U/V parity of everything after it; the first draft of this
        // kind did exactly that and packed two slots against the wrong
        // component. The guard caught it, which is the only reason this comment
        // is about a layout choice rather than about a shape that came out
        // wrong.
        TangentCurvesAtPoint,//                                        (8 slots)

        // --- M17: the two legs of a point-to-point distance ---------------
        //
        // SIGNED, deliberately: `(b - a) - target`, not `|b - a| - target`.
        // The absolute form's derivative does not exist at zero, which is
        // exactly the configuration a solver is in when it is asked for a
        // horizontal separation between two points that are currently aligned
        // vertically. The signed form is smooth everywhere and its Jacobian row
        // is the constant (-1, +1).
        //
        // The consequence is that the SIGN of the target means something: the
        // UI orders the pair so a freshly created dimension is positive, and a
        // negative value moves b to the other side of a rather than being
        // refused.
        PointsDeltaU,        // b.u - a.u - target                     (2 slots)
        PointsDeltaV,        // b.v - a.v - target                     (2 slots)
        // PointOnLine with a target instead of zero: the SIGNED perpendicular
        // distance from a point to the infinite line, minus what it should be.
        // Same formula, so the two can never disagree about which side is
        // positive.
        PointLineDistance,   //                                        (6 slots)

        // --- M17: symmetry about a line -----------------------------------
        //
        // TWO residuals, because "mirror images" is two independent facts and a
        // single number cannot carry both:
        //
        //   SymmetricAcross  the two are the same distance from the line on
        //                    OPPOSITE sides -- the signed distances sum to zero
        //   SymmetricAlong   the segment joining them is SQUARE to the line
        //
        // Without the second, two points equidistant from the line but slid
        // along it satisfy the first perfectly and are not mirror images at
        // all. Both are in millimetres, like every other positional residual,
        // so one tolerance covers them.
        //
        // Slots for both: a.u a.v b.u b.v line.start.u line.start.v line.end.u
        // line.end.v.
        SymmetricAcross,     //                                        (8 slots)
        SymmetricAlong,      //                                        (8 slots)

        // --- M17: an arc's tips, as variables -----------------------------
        //
        // An arc's endpoints ARE its centre, radius and angle -- so they are
        // carried as variables of their own, tied to those three by one
        // residual per component:
        //
        //   ArcTipU   tip.u - (centre.u + r cos(angle))
        //   ArcTipV   tip.v - (centre.v + r sin(angle))
        //
        // Two variables and two residuals per tip, so the tip adds no freedom
        // of its own -- and every constraint that already knows how to hold a
        // POINT now holds an arc's end without knowing an arc exists.
        //
        // The alternative was a tip-aware variant of Coincident, Distance,
        // Fix, PointLineDistance, Symmetric and everything after them. This is
        // four slots and one idea.
        //
        // Slots: tip.u|v, centre.u|v, radius, angle.
        ArcTipU,             //                                        (4 slots)
        ArcTipV,             //                                        (4 slots)

        // --- M17.25: an ellipse -------------------------------------------
        //
        // An ELLIPTICAL ARC's tips, the same idea as ArcTipU/V and a bigger
        // formula, because a rotated ellipse mixes the two axes:
        //
        //   tip = centre + R(rot) * (a cos t, b sin t)
        //
        // so BOTH components need the centre component that matches, plus both
        // radii, the rotation and the parameter. Six slots rather than four,
        // and unlike the circular case the u equation and the v equation are
        // genuinely different -- neither is the other with cos swapped for sin.
        //
        // Slots: tip, centre (matching component), a, b, rot, t.
        EllipseTipU,         //                                        (6 slots)
        EllipseTipV,         //                                        (6 slots)

        // A POINT ON AN ELLIPSE. Implicit, not a distance:
        //
        //   (sqrt((x'/a)^2 + (y'/b)^2) - 1) * (a + b) / 2
        //
        // where (x', y') is the point in the ellipse's own frame. There is no
        // closed form for the distance from a point to an ellipse -- it is a
        // quartic -- and iterating for one inside a residual would make the
        // Jacobian's finite differences fight a second solver.
        //
        // The scale factor is what makes the number COMPARABLE TO MILLIMETRES,
        // which every other positional residual here is measured in: the
        // implicit form alone is dimensionless and its magnitude depends on how
        // eccentric the ellipse is, so one tolerance could not serve a 1 mm
        // ellipse and a 100 mm one. Near the curve it is approximately the
        // distance; far from it, it is not, and it does not claim to be.
        //
        // Slots: p.u p.v c.u c.v a b rot.
        PointOnEllipseImplicit,//                                       (7 slots)

        // WHERE AN ELLIPSE'S MAJOR AXIS POINTS. `value - target`, the same
        // shape as Radius -- but its own kind rather than a widening of that
        // one, because the slot it reads is an ANGLE and Radius's is a length.
        // Sharing would mean the packing guard could no longer tell a rotation
        // packed where a radius belongs from a deliberate choice.
        EllipseRotation,     //                                        (1 slot)

        // A SPLINE HANDLE'S TIP, tied to its point and its tangent (M18):
        // tip - base - delta, in one component. The same shape as ArcTipU, and
        // for the same reason -- see Component::HandleTipU.
        //
        // Slots: tip base delta.
        SplineHandleTipU,    //                                       (3 slots)
        SplineHandleTipV,    //                                       (3 slots)

        // A LINE TANGENT TO AN ELLIPSE (M18).
        //
        // In the ellipse's own frame a line with unit normal (nu, nv) touches
        // it exactly when its signed distance h from the centre satisfies
        //
        //     h^2 = a^2 nu^2 + b^2 nv^2
        //
        // -- the right-hand side being the distance from the centre to the
        // tangent line with that normal. No contact point appears, which is
        // why a line is the one ellipse tangency with a closed form: a circle
        // or another ellipse needs the touch parameter solved for, and that is
        // a variable no constraint owns yet.
        //
        // SQUARED, not |h| - R. The absolute value has a corner at h = 0 --
        // the line through the centre -- and a residual whose derivative flips
        // sign there would let a drag through the middle jump the line to the
        // other side. The squared form is smooth everywhere, and at a solution
        // h = +-R != 0 its derivative is 2h, so it is not the rank-deficient
        // kind ADR-M17-044 was about.
        //
        // Divided by (a^2 + b^2) to be dimensionless, like every other
        // normalised residual here: one tolerance has to serve a 1 mm ellipse
        // and a 100 mm one.
        //
        // Slots: p.u p.v q.u q.v c.u c.v a b rot -- the line's two ends, then
        // the ellipse's centre, semi-axes and rotation.
        TangentLineEllipse   //                                        (9 slots)
    };

    Kind kind{Kind::PointsEqualU};

    // Variable indices into SketchSolveProblem::variables, in the order the
    // residual's formula consumes them; -1 for a slot this kind does not use.
    //
    // EIGHT slots, not four. Four is how many a line-to-line angle needs for
    // each line alone, and an earlier revision -- unable to express the
    // constraint in the four slots it had -- packed only the two lines' v
    // components and evaluated sin(dvB - dvA - target). That subtracts a
    // millimetre difference from a radian target: it is not the angle equation,
    // it converged to a residual of 4e-11, and it reported Solved while
    // producing an angle wrong by up to 260 degrees. The type could not say
    // what the constraint meant, so the code said something else.
    //
    // SlotsRequired() below states each kind's arity, and the solver REJECTS a
    // problem whose residual is missing a slot it needs, so a future kind
    // cannot quietly compute nonsense from whatever happens to be packed.
    // NINE since M18: a line tangent to an ellipse reads the line's two ends,
    // the ellipse's centre, both semi-axes and its rotation. Widening the array
    // is safe precisely because SlotsRequired() states each kind's arity and
    // the solver refuses a residual that is short -- the extra slot cannot be
    // read by a kind that did not ask for it.
    static constexpr int kMaxSlots = 9;
    std::array<int, kMaxSlots> vars{{-1, -1, -1, -1, -1, -1, -1, -1, -1}};

    double target{0.0};
    // Which constraint produced this residual, so a failure can name it
    // (spec 25's Gate E requires a useful constraint diagnostic).
    SketchConstraintId sourceConstraint{kInvalidSketchConstraintId};
};

// How many leading slots of SolveResidual::vars a kind actually reads. Declared
// here, next to the enum, so the arity is part of the interface rather than an
// assumption each backend makes privately.
// Which COMPONENT each slot of a kind may name. The arity check alone cannot
// tell a correctly-packed Distance from one packed (a.u, b.u, a.v, b.v): all
// four slots are filled and in range, so it passes -- and then reports Solved
// with a residual of 2.7e-12 while the geometry is wrong by 2.8 mm. That is
// character-for-character the failure that shipped as C1.
//
// Every variable already carries its Component, so the solver can check the
// packing means what the formula assumes rather than trusting the caller.
//
// A PREDICATE, not a lookup, because one slot legitimately accepts two
// components: an arc tip's angle is the arc's START angle for one tip and its
// END angle for the other, and the same four-slot formula serves both. The
// earlier lookup form had no way to say that -- which is why ArcTipU/V, and
// six more kinds beside them, were left out of the table altogether and
// silently checked against nothing (see kUndeclaredArity).
//
// AN UNLISTED KIND ACCEPTS NOTHING, so forgetting one here fails loudly at the
// first solve instead of quietly switching the guard off for it.
constexpr bool SlotAccepts(SolveResidual::Kind kind, int slot,
                           SolveVariable::Component component) noexcept {
    using C = SolveVariable::Component;
    // (a.u, a.v, b.u, b.v, ...) -- the layout most kinds use.
    const bool alternating = component == ((slot % 2 == 0) ? C::U : C::V);
    switch (kind) {
        case SolveResidual::Kind::PointsEqualU:
        case SolveResidual::Kind::LineVertical:
        case SolveResidual::Kind::FixedU:
        case SolveResidual::Kind::PointsDeltaU:
        case SolveResidual::Kind::MidpointU:
            return component == C::U;
        case SolveResidual::Kind::PointsEqualV:
        case SolveResidual::Kind::LineHorizontal:
        case SolveResidual::Kind::FixedV:
        case SolveResidual::Kind::PointsDeltaV:
        case SolveResidual::Kind::MidpointV:
            return component == C::V;
        case SolveResidual::Kind::Radius:
        case SolveResidual::Kind::RadiiEqual:
            // EITHER SEMI-AXIS. `value - target` is the same equation whichever
            // radius-like scalar it is given, and an ellipse's MINOR one is a
            // legitimate thing to dimension -- EllipseAxisConstraint chooses
            // which, and it stores that choice rather than deriving it.
            //
            // This was caught the first time an axis dimension was solved: the
            // constraint packed the minor slot and the guard refused the whole
            // problem, which is the guard being right about the packing and
            // wrong about the vocabulary.
            return component == C::Radius || component == C::MinorRadius;
        case SolveResidual::Kind::Distance:
        case SolveResidual::Kind::Length:
        case SolveResidual::Kind::Angle:
        case SolveResidual::Kind::LinesParallel:
        case SolveResidual::Kind::LinesPerpendicular:
        case SolveResidual::Kind::LengthsEqual:
        case SolveResidual::Kind::PointOnLine:
        case SolveResidual::Kind::PointLineDistance:
        case SolveResidual::Kind::SymmetricAcross:
        case SolveResidual::Kind::SymmetricAlong:
            return alternating;
        case SolveResidual::Kind::PointOnCircle:
            // (p.u, p.v, c.u, c.v, r)
            return slot == 4 ? component == C::Radius : alternating;
        case SolveResidual::Kind::TangentLineCircle:
            // (a.u, a.v, b.u, b.v, c.u, c.v, r)
        case SolveResidual::Kind::TangentAtPoint:
            // (touch.u, touch.v, far.u, far.v, c.u, c.v, r)
            return slot == 6 ? component == C::Radius : alternating;
        case SolveResidual::Kind::TangentCirclesOuter:
        case SolveResidual::Kind::TangentCirclesInner:
            // (c1.u, c1.v, r1, c2.u, c2.v, r2)
            if (slot == 2 || slot == 5) return component == C::Radius;
            return component == ((slot % 3 == 0) ? C::U : C::V);
        case SolveResidual::Kind::TangentCurvesAtPoint:
            // (touch.u, touch.v, c1.u, c1.v, c2.u, c2.v, r1, r2)
            return slot >= 6 ? component == C::Radius : alternating;
        case SolveResidual::Kind::ArcTipU:
        case SolveResidual::Kind::ArcTipV:
            // (tip, centre, radius, angle), all in ONE component.
            if (slot == 2) return component == C::Radius;
            // EITHER angle: this is the slot a single-answer table could not
            // express, and the reason these two went unchecked.
            if (slot == 3) return component == C::StartAngle || component == C::EndAngle;
            return component == (kind == SolveResidual::Kind::ArcTipU ? C::U : C::V);
        case SolveResidual::Kind::EllipseTipU:
        case SolveResidual::Kind::EllipseTipV:
            // (tip, centre, a, b, rot, t)
            if (slot == 2) return component == C::Radius;
            if (slot == 3) return component == C::MinorRadius;
            if (slot == 4) return component == C::Rotation;
            // EITHER end's parameter, for the reason the arc tip gives.
            if (slot == 5) return component == C::StartParam || component == C::EndParam;
            return component == (kind == SolveResidual::Kind::EllipseTipU ? C::U : C::V);
        case SolveResidual::Kind::PointOnEllipseImplicit:
            // (p.u, p.v, c.u, c.v, a, b, rot)
            if (slot == 4) return component == C::Radius;
            if (slot == 5) return component == C::MinorRadius;
            if (slot == 6) return component == C::Rotation;
            return alternating;
        case SolveResidual::Kind::EllipseRotation:
            return component == C::Rotation;
        case SolveResidual::Kind::SplineHandleTipU:
            // (tip, base, delta) -- the first two are both ordinary U's, and
            // which is which is decided by the packing, not by the component.
            if (slot == 2) return component == C::HandleU;
            return component == C::U;
        case SolveResidual::Kind::SplineHandleTipV:
            if (slot == 2) return component == C::HandleV;
            return component == C::V;
        case SolveResidual::Kind::TangentLineEllipse:
            // (p.u, p.v, q.u, q.v, c.u, c.v, a, b, rot)
            if (slot == 6) return component == C::Radius;
            if (slot == 7) return component == C::MinorRadius;
            if (slot == 8) return component == C::Rotation;
            return alternating;
    }
    return false;
}

// A kind whose arity nobody declared. Returned instead of 0 because 0 reads as
// "needs no variables" and made the solver's guard skip the kind entirely --
// which is what happened to every kind M17 added: the check written so that a
// future kind could not quietly compute nonsense from whatever happened to be
// packed was switched off for each of them, in silence, by a missing case.
inline constexpr int kUndeclaredArity = -1;

// How many leading slots of SolveResidual::vars a kind actually reads. Declared
// here, next to the enum, so the arity is part of the interface rather than an
// assumption each backend makes privately.
constexpr int SlotsRequired(SolveResidual::Kind kind) noexcept {
    switch (kind) {
        case SolveResidual::Kind::FixedU:
        case SolveResidual::Kind::FixedV:
        case SolveResidual::Kind::Radius:
        case SolveResidual::Kind::EllipseRotation:
            return 1;
        case SolveResidual::Kind::PointsEqualU:
        case SolveResidual::Kind::PointsEqualV:
        case SolveResidual::Kind::LineHorizontal:
        case SolveResidual::Kind::LineVertical:
        case SolveResidual::Kind::RadiiEqual:
        case SolveResidual::Kind::PointsDeltaU:
        case SolveResidual::Kind::PointsDeltaV:
            return 2;
        case SolveResidual::Kind::MidpointU:
        case SolveResidual::Kind::MidpointV:
            return 3;
        case SolveResidual::Kind::Distance:
        case SolveResidual::Kind::Length:
        case SolveResidual::Kind::ArcTipU:
        case SolveResidual::Kind::ArcTipV:
            return 4;
        case SolveResidual::Kind::PointOnCircle:
            return 5;
        case SolveResidual::Kind::EllipseTipU:
        case SolveResidual::Kind::EllipseTipV:
            return 6;
        case SolveResidual::Kind::PointOnLine:
        case SolveResidual::Kind::PointLineDistance:
        case SolveResidual::Kind::TangentCirclesOuter:
        case SolveResidual::Kind::TangentCirclesInner:
            return 6;
        case SolveResidual::Kind::TangentLineCircle:
        case SolveResidual::Kind::TangentAtPoint:
        case SolveResidual::Kind::PointOnEllipseImplicit:
            return 7;
        case SolveResidual::Kind::TangentLineEllipse:
            return 9;
        case SolveResidual::Kind::SplineHandleTipU:
        case SolveResidual::Kind::SplineHandleTipV:
            return 3;
        case SolveResidual::Kind::Angle:
        case SolveResidual::Kind::LinesParallel:
        case SolveResidual::Kind::LinesPerpendicular:
        case SolveResidual::Kind::LengthsEqual:
        case SolveResidual::Kind::SymmetricAcross:
        case SolveResidual::Kind::SymmetricAlong:
        case SolveResidual::Kind::TangentCurvesAtPoint:
            return 8;
    }
    return kUndeclaredArity;
}

struct SketchSolveProblem {
    std::vector<SolveVariable> variables;
    std::vector<double> initialValues; // parallel to variables
    std::vector<SolveResidual> residuals;
};

// Mapping documented in ADR-M5-005 so it can be checked rather than inferred.
enum class SketchSolveStatus {
    Solved,            // converged, DOF == 0
    UnderConstrained,  // converged, DOF > 0
    OverConstrained,   // redundant but CONSISTENT constraints
    Conflicting,       // residuals do not converge; contradictory constraints
    InvalidInput,      // detected before solving: bad reference, parameter or value
    NumericalFailure   // iteration limit, or a non-finite value during iteration
};

const char* SolveStatusName(SketchSolveStatus status) noexcept;

// A variable the solve did NOT pin down (M17.29).
//
// The DOF number says HOW MANY freedoms are left; this says WHICH. Without it a
// sketch can only be coloured as a whole, so one loose spline paints a
// fully-dimensioned rectangle beside it as loose too -- and a sketch with a
// spline in it, which always keeps some freedom, could never go black at all.
//
// Measured, not counted: a variable is free when the Jacobian's null space at
// the solution has a component along it. That is the same question the rank
// answers, asked per column instead of in total, so the two can never disagree
// about whether a sketch is finished.
struct SketchSolveResult {
    std::vector<double> values; // parallel to the problem's variables
    SketchSolveStatus status{SketchSolveStatus::InvalidInput};
    // variables - rank(Jacobian), taken numerically at the solved
    // configuration. NEVER variables - residual count (ADR-M5-005).
    // Starts "not measured", not 0: 0 means FULLY CONSTRAINED here, and a
    // failed solve has measured nothing. Failure() leaves it at this value.
    int degreesOfFreedom{kUnknownDegreesOfFreedom};
    // Parallel to `variables`, and EMPTY when the solve failed -- a failed
    // solve has measured nothing, exactly as its DOF reports nothing. Never
    // read it without checking the size.
    std::vector<bool> variableIsFree;
    int iterations{0};
    double maxResidual{0.0};
    // Constraints implicated in a failure, for diagnostics. Empty on success.
    std::vector<SketchConstraintId> offendingConstraints;
    std::string message;

    explicit operator bool() const noexcept {
        return status == SketchSolveStatus::Solved ||
               status == SketchSolveStatus::UnderConstrained ||
               status == SketchSolveStatus::OverConstrained;
    }
};

class ISketchSolver {
public:
    virtual ~ISketchSolver() = default;

    // Never throws. A problem the solver cannot handle returns a result whose
    // status says why, with `values` left equal to the initial values so a
    // caller that ignores the status still cannot commit garbage -- though the
    // documented contract is that a caller commits nothing unless the result
    // converts to true (ADR-M5-004).
    virtual SketchSolveResult solve(const SketchSolveProblem& problem) = 0;
};

// Numerical policy (ADR-M5-003, spec 12). Documented here, applied by the
// implementation, and never enlarged merely to make a test pass.
inline constexpr double kSolveResidualTolerance = 1e-9;
inline constexpr double kSolveStepTolerance = 1e-12;
inline constexpr int kSolveMaxIterations = 100;
inline constexpr double kSolveRankThreshold = 1e-9;


} // namespace paramcad
