/************************************************************************
 **
 **  @file   Footprint.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Multi-size footprint computation via Clipper2 Union.
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

#ifndef FOOTPRINT_H
#define FOOTPRINT_H

#include <QString>
#include <QVector>

#include <clipper2/clipper.h>

#include "model/Piece.h"

namespace Footprint {

struct Result
{
    std::vector<Clipper2Lib::Path64> polygons;   // one footprint per unique piece
    std::vector<QString>             names;       // display name per footprint
    std::vector<QString>             keys;        // lookup key (groupName::pieceName or pieceName)
    std::vector<int>                 sizeCounts;  // number of sizes unioned
};

/// Compute the footprint (union of all size contours) for each unique piece
/// across all size layers.
///
/// Pieces are identified by (groupName, pieceName). The union of all graded
/// contours yields the envelope polygon — the nesting engine never sees
/// individual sizes.
Result compute(const QVector<SizeLayer> &layers);

} // namespace Footprint

#endif // FOOTPRINT_H
