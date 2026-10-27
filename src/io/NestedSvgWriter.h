/************************************************************************
 **
 **  @file   NestedSvgWriter.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   26/05/2026
 **
 **  @brief  Applies nesting placements as SVG transforms on a graded SVG
 **          DOM, producing a structurally identical SVG with pieces at
 **          their nested positions.
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

#ifndef NESTEDSVGWRITER_H
#define NESTEDSVGWRITER_H

#include <QString>
#include <QVector>
#include <vector>

#include "model/Piece.h"
#include "model/Placement.h"
#include "nesting/Footprint.h"

class NestedSvgWriter
{
public:
    /// Apply nesting placements to a graded SVG and write the result.
    ///
    /// Each piece <g> in every size layer gets a transform="translate(x,y) rotate(deg)"
    /// that moves it from its original position to its nested position.
    /// The decor layer wrappers (grainline/fold, info blocks, annotations)
    /// are transformed identically with their piece.
    ///
    /// @param inputSvg   Original graded SVG from Ostara
    /// @param outputSvg  Output nested SVG
    /// @param placements Nesting result (indexed by footprint order)
    /// @param fp         Footprint result (polygons for bbox, keys for identity)
    /// @param binWidthMm Bin width in millimetres
    /// @param usedHeightMm Total used height in millimetres
    /// @param obstacleWidthMm Optional obstacle rectangle width (0 = none)
    /// @param obstacleHeightMm Optional obstacle rectangle height (0 = none)
    /// @return true on success
    bool write(const QString              &inputSvg,
               const QString              &outputSvg,
               const std::vector<Placement> &placements,
               const Footprint::Result    &fp,
               double                     binWidthMm,
               double                     usedHeightMm,
               double                     obstacleWidthMm  = 0.0,
               double                     obstacleHeightMm = 0.0);

private:
    struct PieceTransform
    {
        double px;       // placement X in SVG pixels
        double py;       // placement Y in SVG pixels
        double rotation; // degrees
        double origX;    // footprint bbox min X in SVG pixels
        double origY;    // footprint bbox min Y in SVG pixels
    };
};

#endif // NESTEDSVGWRITER_H
