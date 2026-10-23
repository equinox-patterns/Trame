/************************************************************************
 **
 **  @file   CuttingRules.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   22/10/2026
 **
 **  @brief  Nesting — the business rules of a cutting plan.
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

#include "CuttingRules.h"

#include <cmath>

namespace
{
constexpr double kPi = 3.14159265358979323846;

//---------------------------------------------------------------------------------------------------------------------
// An angle in [0, 360), against 0: the engine compares angles modulo a full turn, and a negative
// one would only ever be normalised there anyway.
auto Normalized(double degrees) -> double
{
    double angle = std::fmod(degrees, 360.0);
    if (angle < 0.0)
    {
        angle += 360.0;
    }
    return angle;
}

//---------------------------------------------------------------------------------------------------------------------
// The angle of a line, in Qt's convention — counter-clockwise as seen on screen, so a line pointing
// up reads 90°. `y` grows downwards, hence the minus on `dy`.
auto LineAngle(const CuttingRules::Line &line) -> double
{
    return Normalized(
        std::atan2(-(line.y2 - line.y1), line.x2 - line.x1) * 180.0 / kPi);
}

//---------------------------------------------------------------------------------------------------------------------
// The turn the engine applies, applied here: `x' = x·c − y·s`, `y' = x·s + y·c`. Written out rather
// than taken from a graphics matrix, because this file must be readable on its own — a sign error
// here lays every piece across the grain and nothing on screen shows it.
struct Turned
{
    double x{0.0};
    double y{0.0};
};

auto Turn(const CuttingRules::Line &line, double degrees) -> std::pair<Turned, Turned>
{
    const double radians = degrees * kPi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    return {Turned{line.x1 * c - line.y1 * s, line.x1 * s + line.y1 * c},
            Turned{line.x2 * c - line.y2 * s, line.x2 * s + line.y2 * c}};
}
}  // namespace

//---------------------------------------------------------------------------------------------------------------------
auto CuttingRules::AllowedAngles(const Direction &direction, bool oneWay) -> std::vector<double>
{
    if (!direction.known)
    {
        return {};
    }

    // ⚠️ THE sign of this whole feature. Qt's angle() is counter-clockwise on screen, the engine's
    // rotatePath() is clockwise. Laying the direction along the fabric's length takes `g − 90`;
    // `g + 90` is that same orientation turned a half-turn.
    const double base = direction.angle - 90.0;

    // Only two of the twelve values are ever authored — « Simple » (twoWaysUpDown) and « Double »
    // (fourWays). The ten others come from imported files and are read the way the dock reads
    // them: as two ways.
    if (direction.arrows == Arrows::FourWays)
    {
        // The cross — TWO axes: droit-fil, and contre-fil. Both are legitimate, so the one-way
        // option keeps them both and takes one sense of each. That is what the cross means here.
        if (oneWay)
        {
            return {Normalized(base), Normalized(base + 90.0)};
        }

        return {Normalized(base), Normalized(base + 90.0), Normalized(base + 180.0),
                Normalized(base + 270.0)};
    }

    // One axis only, so one-way leaves a single orientation: the piece is laid the way it was
    // drawn, and no copy of it is turned a half-turn.
    if (oneWay)
    {
        return {Normalized(base)};
    }

    return {Normalized(base), Normalized(base + 180.0)};
}

//---------------------------------------------------------------------------------------------------------------------
auto CuttingRules::FoldAngle(const std::vector<std::pair<double, double>> &contour, const Line &fold,
                             double *angle, bool *foldOnTheRight) -> bool
{
    if (fold.IsNull())
    {
        return false;
    }

    // Les deux rotations qui mettent le pli vertical — celles qu'`AllowedAngles` rend déjà pour un
    // pli. Le signe est celui du chantier : `QLineF::angle()` tourne dans un sens, `rotatePath` dans
    // l'autre, d'où le `− 90` et non le `+ 90`.
    double alignment = std::fmod(LineAngle(fold), 180.0);
    if (alignment < 0.0)
    {
        alignment += 180.0;
    }
    const double base = alignment - 90.0;

    // ⚠️ **Laquelle des deux ? Celle qui ne retourne pas la pièce.** Elles diffèrent d'un
    // demi-tour : l'une laisse la pièce dans son sens, l'autre la couche tête en bas — et les deux
    // sont posables, la seconde simplement à l'envers. Un demi-tour, c'est un cosinus négatif : la
    // rotation qui ne retourne rien est celle dont le cosinus est positif.
    const double candidates[2] = {base, base + 180.0};
    double chosen = candidates[0];
    for (const double candidate : candidates)
    {
        if (std::cos(candidate * kPi / 180.0) > 0.0)
        {
            chosen = candidate;
            break;
        }
    }

    // Un dixième de millième de pixel. La rotation est une transformation de coordonnées, et deux
    // points que le calcul sépare de ça ne sont pas séparés : c'est du bruit, pas une pente.
    constexpr double kTolerance = 1e-4;

    const auto [first, second] = Turn(fold, chosen);
    const double dx = second.x - first.x;
    if (std::abs(dx) > kTolerance)
    {
        return false;
    }

    // Le corps tombe d'un côté du pli — ou de l'autre. Le pli doit être le bord extrême de la pièce
    // dans ce sens-là, sinon il n'est pas un bord de sa propre pièce et rien ne le rendra posable :
    // le dire est le travail de l'appelant.
    const double radians = chosen * kPi / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const double edge = first.x;

    bool bodyOnTheRight = true;
    bool bodyOnTheLeft = true;
    for (const auto &point : contour)
    {
        const double x = point.first * c - point.second * s;
        bodyOnTheRight = bodyOnTheRight && (x >= edge - kTolerance);
        bodyOnTheLeft = bodyOnTheLeft && (x <= edge + kTolerance);
        if (!bodyOnTheRight && !bodyOnTheLeft)
        {
            break;
        }
    }

    if (bodyOnTheRight == bodyOnTheLeft)
    {
        return false;
    }

    *angle = Normalized(chosen);
    // Le corps à gauche veut un pli sur le bord DROIT pour l'accueillir, et réciproquement.
    *foldOnTheRight = bodyOnTheLeft;
    return true;
}

//---------------------------------------------------------------------------------------------------------------------
auto CuttingRules::SeatMargins(bool foldOnTheLeft, double edgePx) -> Bin::Margins
{
    Bin::Margins out;
    out.left = foldOnTheLeft ? 0.0 : edgePx;
    out.right = foldOnTheLeft ? edgePx : 0.0;
    out.top = edgePx;
    out.bottom = edgePx;
    return out;
}
