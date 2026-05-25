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
 **  All rights reserved.
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
