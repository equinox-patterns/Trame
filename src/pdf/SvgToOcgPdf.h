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
    /// Convert an SVG to PDF, with one OCG layer per layer group.
    ///
    /// A document that carries layer groups — a graded export, one layer per size plus
    /// the decor — comes out with optional content, one entry per group. A document that
    /// carries none — a single size — comes out flat: there is nothing to switch on and
    /// off, and that is a result, not a failure.
    ///
    /// @param svgPath  Input SVG file
    /// @param pdfPath  Output PDF file
    /// @return true on success
    /// @brief What the document says about itself, for the reader's information panel.
    ///
    /// Four of the standard fields, and they are the caller's to fill: this module knows
    /// how to write a PDF, not what the document is.
    struct DocumentInfo
    {
        QString title;
        QString author;
        QString creator;   ///< the application the content came from
        QString producer;  ///< the tool that wrote the file. Empty: this module names itself.
    };

    bool convert(const QString &svgPath, const QString &pdfPath, const DocumentInfo &info = {});

    /// @brief Write @p svgPath as ONE PDF, one page per rectangle in @p pages.
    ///
    /// This is how a tiled plan is delivered, and how a single-size run is delivered too:
    /// the drawing is written ONCE — one Form XObject per layer — and each page is a
    /// reference to those objects, offset so that its own rectangle of the plan lands on
    /// the sheet. The plan is therefore addressed N times rather than copied N times,
    /// which is what makes a tiled export cost the same whatever its page count, and what
    /// leaves the page's own content stream free for the decor that belongs to a sheet
    /// rather than to the plan.
    ///
    /// The layer set belongs to the DOCUMENT, since there is only one: one OCG object per
    /// layer, referenced by every page. A reader's panel shows one entry per layer, and
    /// switching a size off hides it on every sheet at once — the only reading of it that
    /// makes sense when the sheets are taped back together.
    ///
    /// @param svgPath  Input SVG, spanning the whole plan
    /// @param pages    The page rectangles, in page order, in the SVG's own user
    ///                 coordinates — the same frame as its viewBox. They are expected at
    ///                 96 dpi, which is the resolution the graded writer assumes and the
    ///                 one kPixelToPt converts from.
    /// @param pdfPath  Output PDF file
    /// @return true on success
    bool convert(const QString     &svgPath,
                 const QVector<QRectF> &pages,
                 const QString     &pdfPath,
                 const DocumentInfo &info = {});

private:
    struct PageInfo
    {
        QSizeF sizeMm;
        QRectF viewBox;
    };

    struct LayerEntry
    {
        QString    name;        // PDF key, from the id: "Size_46", "Layout". Has to be a name.
        QString    displayName; // what the reader sees: inkscape:label, else <title>, else the id
        bool       isMep;
        QDomElement element;    // the <g> element
    };

    struct LayerContent
    {
        QString    name;
        QString    displayName;
        bool       isMep;
        QByteArray stream;        // decompressed PDF content from QSvgRenderer
        QByteArray textStream;    // text operators (MEP only), separate CTM scope
        bool       plain = false; // the whole document: no layer, so no optional content
    };

    /// @brief One SVG, read back: its page, and the rendered content of each of its layers.
    ///
    /// Everything the writer needs, and nothing else: once this value exists the document
    /// has no file left to open and no XML left to parse, and every page is written from
    /// it. Reading it once is the whole point — it is the step that used to be repeated
    /// for every sheet.
    struct SvgContent
    {
        bool                  ok{false}; ///< false when reading or rendering failed; logged
        PageInfo              page;
        QVector<LayerContent> layers;
        bool                  plain{false}; ///< no layer group at all: a single-size document
    };

    QVector<LayerEntry> findLayers(const QDomDocument &doc) const;

    /// @brief The layer's name as the reader must read it.
    ///
    /// The id is what the PDF addresses the layer by and is safe as a name; the label is
    /// what the user typed, and it is the label that answers for the layer. Reading the
    /// id back was losing accents and spaces on the way.
    static QString displayNameOf(const QDomElement &element);

    PageInfo            getPageInfo(const QDomDocument &doc) const;
    QByteArray          buildLayerSvg(const QDomDocument &origDoc,
                                      const QDomElement   &layerEl,
                                      const PageInfo      &page) const;
    QByteArray          renderToPdfBuffer(const QByteArray &svgData,
                                          const PageInfo   &page) const;
    /// @brief The page's drawing, decompressed and ready to be wrapped in BDC/EMC.
    ///
    /// Reads every part the page declares and puts them back in order — a big drawing is
    /// written in several streams — and inflates each one. The name says extraction only;
    /// the decompression is in here because a part is no use until it is inflated, and
    /// splitting the two let a truncated part reach the caller as if it were whole.
    QByteArray          extractContentStream(const QByteArray &pdfBuf);
    QByteArray          decompressFlate(const QByteArray &compressed);
    QByteArray          emitMepTextAsPdfOps(const QDomElement &root,
                                              double           pageHeightPt) const;
    /// @brief Read one SVG into its page and the rendered content of each of its layers.
    ///
    /// The whole of what convert() used to do for a single document, lifted out so that a
    /// tiled conversion can call it once per sheet. The caller gets a value it can hold;
    /// nothing here depends on there being exactly one document.
    SvgContent          readSvg(const QString &svgPath);

    /// @brief Write @p doc as one PDF, one page per rectangle, over a single layer set.
    ///
    /// Every page draws the same layers and differs only in which part of the plan it
    /// shows, so the layers become Form XObjects written once and invoked N times, and an
    /// OCG object per layer is allocated once and referenced by every page. Allocating
    /// either per page would put one entry per layer per sheet in the reader's panel, and
    /// would put the drawing itself in the file once per sheet.
    bool                buildOcgPdf(const QString         &pdfPath,
                                    const SvgContent      &doc,
                                    const QVector<QRectF> &pages,
                                    const DocumentInfo    &info);
};

#endif // SVGTOOCGPDF_H
