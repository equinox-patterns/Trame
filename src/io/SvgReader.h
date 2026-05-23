/************************************************************************
 **
 **  @file   SvgReader.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Reads an Ostara-exported graded SVG file and extracts size
 **          layers with piece contours.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef SVGREADER_H
#define SVGREADER_H

#include "model/Piece.h"

class SvgReader
{
public:
    static constexpr double kScale = 1e6;

    SvgReader() = default;

    /// Parse a graded SVG and return one SizeLayer per size.
    bool read(const QString &filePath, QVector<SizeLayer> &layers);

    /// Parse and return a single size layer by index (0 = smallest).
    /// Convenience for V0 single-size nesting.
    bool readSingleLayer(const QString &filePath, SizeLayer &layer, int index = 0);

private:
    // SVG path data parser
    Clipper2Lib::Path64 parsePathData(const QString &d) const;
    void interpolateBezier(Clipper2Lib::Point64 p1, Clipper2Lib::Point64 cp1,
                           Clipper2Lib::Point64 cp2, Clipper2Lib::Point64 p4,
                           Clipper2Lib::Path64 &out) const;
    Clipper2Lib::Point64 toPoint64(double x, double y) const;
};

#endif // SVGREADER_H
