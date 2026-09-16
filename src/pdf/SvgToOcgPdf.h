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
#include <QStringList>
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

    /// Convert several SVGs to ONE PDF, one page per SVG, sharing a single set of OCG
    /// layers.
    ///
    /// This is how a tiled plan is delivered: the plan is cut into sheets, and a sheet is
    /// a page. Each SVG is a sheet — its own width and height give that page's /MediaBox —
    /// and every sheet is written from the same content, so they all carry the same layer
    /// ids. The layer set therefore belongs to the DOCUMENT, not to the page: one OCG
    /// object per layer, referenced by every page that draws it. A reader's panel then
    /// shows one entry per layer, and switching a size off hides it on every sheet at
    /// once, which is the only reading of it that makes sense when the sheets are taped
    /// back together.
    ///
    /// @param svgPaths Input SVG files, in page order
    /// @param pdfPath  Output PDF file
    /// @return true on success
    bool convert(const QStringList &svgPaths, const QString &pdfPath,
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
    /// Everything the writer needs about one sheet, and nothing else — so the page loop of
    /// a tiled document has no file left to open and no XML left to parse.
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

    /// @brief Write @p pages as one PDF, one page each, over a single set of OCG layers.
    ///
    /// The layer set — and therefore the OCG objects — is decided once, from the first
    /// page, and shared by every page afterwards. Allocating them per page would put one
    /// entry per layer PER SHEET in the reader's panel.
    bool                buildOcgPdf(const QString             &pdfPath,
                                    const QVector<SvgContent> &pages,
                                    const DocumentInfo        &info);
};

#endif // SVGTOOCGPDF_H
