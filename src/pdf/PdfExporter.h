/************************************************************************
 **
 **  @file   PdfExporter.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  PDF exporter with OCG layers — writes a PDF-1.7 file with
 **          one Optional Content Group per size, from scratch (ISO 32000).
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef PDFEXPORTER_H
#define PDFEXPORTER_H

#include <QString>
#include <vector>

#include "model/TransformedPiece.h"

class PdfExporter
{
public:
    struct Config
    {
        double binWidthMm = 841.0;  ///< Strip width in millimetres
        QString svgFilePath;        ///< Original SVG file for label rendering
    };

    /// Write a PDF file with one OCG layer per unique size value.
    /// @param path    Output file path (*.pdf)
    /// @param pieces  Transformed pieces (each contains all sizes)
    /// @param cfg     Export configuration
    /// @return true on success
    bool write(const QString                          &path,
               const std::vector<TransformedPiece>    &pieces,
               const Config                           &cfg);

    /// Write a simple PDF (no OCG) using QPdfWriter + QPainter for correct rendering.
    bool writeSimple(const QString                       &path,
                     const std::vector<TransformedPiece> &pieces,
                     double binWidthMm,
                     double usedHeightMm);
};

#endif // PDFEXPORTER_H
