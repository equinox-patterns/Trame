/************************************************************************
 **
 **  @file   SizeRestorer.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Restores individual size contours from placed footprints.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef SIZERESTORER_H
#define SIZERESTORER_H

#include <QHash>
#include <vector>

#include "model/Piece.h"
#include "model/Placement.h"
#include "model/TransformedPiece.h"
#include "nesting/Footprint.h"

namespace SizeRestorer {

/// Restore individual size contours by applying each footprint's placement
/// to the corresponding piece in every size layer.
///
/// For each placement, finds the piece matching the footprint key across all
/// SizeLayers and transforms its contour by (rotation + translation).
std::vector<TransformedPiece> restore(
    const std::vector<Placement>             &placements,
    const Footprint::Result                  &fp,
    const QVector<SizeLayer>                 &layers,
    const QHash<QString, QVector<MepPath>>   &mepPaths = {},
    const QHash<QString, QVector<MepLabel>>  &mepLabels = {},
    const QHash<QString, QVector<FlatLabel>> &flatLabels = {});

} // namespace SizeRestorer

#endif // SIZERESTORER_H
