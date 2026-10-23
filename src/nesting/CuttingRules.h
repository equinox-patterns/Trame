/************************************************************************
 **
 **  @file   CuttingRules.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   22/10/2026
 **
 **  @brief  Nesting — which rotations a piece may take, and where its fold
 **          must sit. The business rules of a cutting plan, in the socle.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **
 **  This program is free software: you can redistribute it and/or modify
 **  it under the terms of the GNU General Public License as published by
 **  the Free Software Foundation, either version 3 of the License, or
 **  (at your option) any later version.
 **
 **  This program is distributed in the hope that it will be useful,
 **  but WITHOUT ANY WARRANTY; without even the implied warranty of
 **  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 **  GNU General Public License for more details.
 **
 **  You should have received a copy of the GNU General Public License
 **  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **
 *************************************************************************/

#ifndef CUTTINGRULES_H
#define CUTTINGRULES_H

#include "Bin.h"

#include <cmath>
#include <vector>

/**
 * @brief The rotations a piece may take, and the edge its fold must sit on.
 *
 * Fabric has a grain, and a piece laid on it follows that grain. A cutting plan therefore cannot
 * let the nesting turn a piece however it likes — and it must not invent a direction either.
 *
 * **This lives in the socle and not in an application.** Ostara wrote these rules first, and a
 * second application that needed the same plan would have copied them; a sign written twice is a
 * sign that can be wrong in one place and right in the other, and nothing on screen shows which.
 * Here there is one reading, and both callers are its clients.
 *
 * No widget, no scene, no document: the arithmetic that decides an angle, and nothing else.
 */
namespace CuttingRules
{
/// The twelve arrow types a grainline may carry, in the order the pattern files store them.
enum class Arrows : int
{
    TwoWaysUpDown = 0,
    OneWayUp = 1,
    OneWayDown = 2,
    FourWays = 3,
    TwoWaysUpLeft = 4,
    TwoWaysUpRight = 5,
    TwoWaysDownLeft = 6,
    TwoWaysDownRight = 7,
    ThreeWaysUpDownLeft = 8,
    ThreeWaysUpDownRight = 9,
    ThreeWaysUpLeftRight = 10,
    ThreeWaysDownLeftRight = 11,
};

/// A line in the frame of the piece that owns it — the fold axis, in practice.
struct Line
{
    double x1{0.0};
    double y1{0.0};
    double x2{0.0};
    double y2{0.0};

    /// A line the pattern never declared reads as (0, 0)-(0, 0), which is how an absent fold is
    /// told from one that happens to pass through the origin.
    ///
    /// Written as two ordered comparisons rather than `==`, which the project refuses outright —
    /// an equality on a double is a decision nobody made, and this one is a decision: a degenerate
    /// line is exactly the pair of identical points, not an approximately flat one.
    bool IsNull() const { return !(x1 < x2 || x1 > x2 || y1 < y2 || y1 > y2); }
};

/// A piece's direction, and what it allows.
struct Direction
{
    /// False when the piece declares nothing — neither a grainline nor a fold. A cutting plan
    /// refuses to run on such a piece rather than invent a direction for it.
    bool known{false};

    /// The angle of the direction in the pattern's frame, in degrees, in **Qt's convention**:
    /// counter-clockwise as seen on screen, so 90° points up.
    double angle{0.0};

    Arrows arrows{Arrows::TwoWaysUpDown};
};

/// @brief The direction of a piece mirrored about a **vertical** axis.
///
/// ⚠️ **Mirroring a piece changes the angle of its direction, and nothing said so.** The mirror is
/// put on the geometry — in the matrix — while the angle is read on the stored grainline, which the
/// matrix does not touch. A 45° bias became 135° on the copy, and the copy was turned as though it
/// were still at 45°: its grain read horizontal where it had to read vertical.
inline Direction Mirrored(const Direction &direction)
{
    Direction mirrored = direction;
    double angle = std::fmod(180.0 - direction.angle, 360.0);
    if (angle < 0.0)
    {
        angle += 360.0;
    }
    mirrored.angle = angle;
    return mirrored;
}

/**
 * @brief The rotations that lay @p direction along the fabric's length, in the engine's convention.
 *
 * The fabric is sold on a bolt: its grain runs the length of it, which is the bin's unbounded
 * direction. So the piece's direction has to come out **vertical**.
 *
 * ⚠️ **The sign is the whole of it.** `QLineF::angle()` is counter-clockwise as seen on screen; the
 * engine's `rotatePath()` is the other way round (`y' = x * sin + y * cos`, so a positive angle
 * turns clockwise on screen). Laying a direction at @p g along the vertical therefore takes
 * `g − 90`, and `g + 90` is the same orientation turned a half-turn.
 *
 * An empty result means the piece has no direction at all — the nesting then keeps its freedom.
 * @param oneWay the run's own option: no piece is turned a half-turn. **It removes a SENSE, not an
 *        axis** — on a napped or printed cloth every piece takes the grain the same way. So a
 *        direction that allowed two senses keeps one, and a direction that allowed two AXES keeps
 *        both, one sense each: a « Double » piece may still be cut droit-fil or contre-fil, it
 *        simply may not be laid upside down.
 */
auto AllowedAngles(const Direction &direction, bool oneWay = false) -> std::vector<double>;

/**
 * @brief The one rotation that lays a piece cut ON THE FOLD on folded fabric, when there is one.
 *
 * A folded plan lays the half, and its fold line has to sit exactly on the fabric's fold — so the
 * only rotation that will do is the one that brings that line **vertical** with the piece's body on
 * the fabric's side. Of the two rotations a fold axis allows, **one** does: the other lays the body
 * outside the cloth.
 *
 * The fold wins over the grainline here, which is the opposite of a plain direction — and it is not
 * a contradiction: a piece cut on the fold is cut ON THE FOLD, so it is the fold of the fabric that
 * carries it.
 *
 * ⚠️ **The sense belongs to the pattern, not to the nesting.** The two rotations that put the fold
 * line vertical differ by a half-turn, and **both fit**: one has the body on the cloth's side, the
 * other turns the piece over — but turned over it is still in the cloth, merely inside out.
 * Choosing « the one that fits » chooses nothing: **there is one right answer, the one that does not
 * turn the piece.** On a printed cloth every piece takes the cloth the same way, and a piece on the
 * fold first. Forgetting this turned every fold piece of a plan inside out, and nothing said so: a
 * piece inside out looks like a piece the right way round.
 *
 * The side follows, it is not chosen: the piece being the right way round, its body falls on one
 * side of its fold line or the other, and **that side is what says which fold can carry it**.
 *
 * @param contour       the piece's outline, in the frame the rotation will be applied to
 * @param fold          the fold line, in the same frame
 * @param angle         the rotation, in the engine's convention, when the answer is true
 * @param foldOnTheRight true when the body falls to the LEFT of the fold line — so when the fold has
 *        to be on the right-hand edge to receive it
 * @return false when the fold line is not an edge of the piece's own outline — in which case there
 *         is no rotation to find, and the caller has to say so rather than lay the piece somewhere
 */
auto FoldAngle(const std::vector<std::pair<double, double>> &contour, const Line &fold, double *angle,
               bool *foldOnTheRight) -> bool;

/**
 * @brief The margins of one rectangle of folded cloth: **the fold's edge pays nothing**.
 *
 * A piece cut on the fold has its fold line ON the fold, and a margin there would push it away from
 * it — the unfolded piece would then come out `2 m` too narrow. The three other edges pay, the top
 * and the bottom included: a plan reads as a stack of rectangles, and each keeps its space on
 * everything around it, or the empty band would be missing exactly where the eye looks for it.
 */
auto SeatMargins(bool foldOnTheLeft, double edgePx) -> Bin::Margins;
}  // namespace CuttingRules

#endif  // CUTTINGRULES_H
