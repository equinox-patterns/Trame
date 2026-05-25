/************************************************************************
 **
 **  @file   TransformedPiece.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Transformed piece data — the result of restoring individual
 **          sizes from placed footprints.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef TRANSFORMEDPIECE_H
#define TRANSFORMEDPIECE_H

#include <QString>
#include <QVector>

#include <clipper2/clipper.h>

#include "model/Piece.h" // PathStyle

struct TransformedSize
{
    double                       size    = 0;
    double                       stature = 0;
    Clipper2Lib::Path64          contour;
    QVector<Clipper2Lib::Path64> internalPaths; // passmarks, grainlines, etc.
    PathStyle                    style;
};

struct TransformedPiece
{
    QString                   pieceName;
    QString                   groupName;
    QVector<TransformedSize>  sizes;
    QVector<MepPath>          mepPaths;  // grainlines from Mise-en-page layer
    QVector<MepLabel>        mepLabels; // text + logo fragments from Mise-en-page layer (DEPRECATED)
    QVector<FlatLabel>       flatLabels; // native labels from .rabnest (replaces mepLabels)
};

#endif // TRANSFORMEDPIECE_H
