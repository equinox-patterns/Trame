/************************************************************************
 **
 **  @file   RabnestReader.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   24/05/2026
 **
 **  @brief  Reader for the .rabnest XML exchange format.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef RABNESTREADER_H
#define RABNESTREADER_H

#include <QHash>
#include <QString>
#include <QVector>

#include "model/Piece.h"

struct RangeInfo
{
    int index = 0;
    double minSize = 0;
    double maxSize = 0;
    double stature = 0;
};

class RabnestReader
{
public:
    static constexpr double kScale = 1e6; ///< px → Clipper2 int64 (same as SvgReader)

    /// List all size ranges in a .rabnest file without loading geometry.
    static QVector<RangeInfo> listRanges(const QString &filePath);

    /// Read the first size range from a .rabnest file.
    /// @param filePath Path to the .rabnest file.
    /// @param layers   Output: one SizeLayer per size value in the first range.
    /// @return true on success.
    bool read(const QString &filePath, QVector<SizeLayer> &layers);

    /// Read a specific size range by index (0-based).
    bool readRange(const QString &filePath, int rangeIndex, QVector<SizeLayer> &layers);

    /// Number of ranges found in the last-read file.
    int rangeCount() const { return m_rangeCount; }

    /// Mise-en-page grainline paths, keyed by "GroupName::PieceName" or "PieceName".
    QHash<QString, QVector<MepPath>> mepPaths;

    /// Flat labels from Mise-en-page, same key scheme as mepPaths.
    QHash<QString, QVector<FlatLabel>> flatLabels;

private:
    int m_rangeCount = 0;

    Clipper2Lib::Point64 toPoint64(double x, double y) const;
    void interpolateBezier(Clipper2Lib::Point64 p1, Clipper2Lib::Point64 cp1,
                           Clipper2Lib::Point64 cp2, Clipper2Lib::Point64 p4,
                           Clipper2Lib::Path64 &out) const;
    Clipper2Lib::Path64 parsePathData(const QString &d) const;
};

#endif // RABNESTREADER_H
