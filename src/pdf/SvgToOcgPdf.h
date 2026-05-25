/************************************************************************
 **
 **  @file   SvgToOcgPdf.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   25/05/2026
 **
 **  @brief  Convert a graded SVG to PDF with one OCG layer per size.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef SVGTOOCGPDF_H
#define SVGTOOCGPDF_H

#include <QByteArray>
#include <QDomElement>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

class SvgToOcgPdf
{
public:
    /// Convert a graded SVG to a layered PDF.
    /// @param svgPath  Input SVG file (with i:layer="yes" groups)
    /// @param pdfPath  Output PDF file
    /// @return true on success
    bool convert(const QString &svgPath, const QString &pdfPath);

private:
    struct PageInfo
    {
        QSizeF sizeMm;
        QRectF viewBox;
    };

    struct LayerEntry
    {
        QString    name;    // "Size_46", "Mise_en_page"
        bool       isMep;
        QDomElement element; // the <g> element
    };

    struct LayerContent
    {
        QString    name;
        bool       isMep;
        QByteArray stream;      // decompressed PDF content from QSvgRenderer
        QByteArray textStream;  // text operators (MEP only), separate CTM scope
    };

    QVector<LayerEntry> findLayers(const QDomDocument &doc) const;
    PageInfo            getPageInfo(const QDomDocument &doc) const;
    QByteArray          buildLayerSvg(const QDomDocument &origDoc,
                                      const QDomElement   &layerEl,
                                      const PageInfo      &page) const;
    QByteArray          renderToPdfBuffer(const QByteArray &svgData,
                                          const PageInfo   &page) const;
    QByteArray          extractContentStream(const QByteArray &pdfBuf);
    QByteArray          decompressFlate(const QByteArray &compressed);
    QByteArray          emitMepTextAsPdfOps(const QDomElement &root,
                                              double           pageHeightPt) const;
    bool                buildOcgPdf(const QString                &pdfPath,
                                    const PageInfo               &page,
                                    const QVector<LayerContent>  &layers);
};

#endif // SVGTOOCGPDF_H
