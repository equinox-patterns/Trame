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

#ifndef SVGTOOCGPDF_H
#define SVGTOOCGPDF_H

#include <QByteArray>
#include <QDomElement>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

/// @brief An alpha state a content stream reaches by name, and that the Forms must declare.
///
/// A PDF carries no alpha in its colour operators. Opacity travels in a graphics state, and a
/// stream reaches one by NAME — `/GState11 gs` — never by value. A name that resolves to nothing
/// is not something a reader reports: the instruction is ignored, and the drawing comes out at
/// full strength. That silence is why this went unnoticed — a frame that prints solid looks like
/// a frame.
///
/// Two hands fill this list, and they never collide.
///
/// Qt names the states it allocated after the object they were in ITS file — `/GState11` is
/// object 11 of the PDF Qt rendered — so those are copied across under the very name the stream
/// already calls. Only the object number in our own file changes.
///
/// The re-emitted text has no such object behind it: it is drawn here, from a `QPainterPath`, so
/// nothing upstream ever gave it a state. Its states are therefore named by rank — `GSt1`, then
/// `GSt2` — which is ours to choose precisely because no stream came in expecting a name.
struct AlphaState
{
    QString    name; ///< without the leading slash: `GState11`, `GSt1`
    QByteArray body; ///< the object's own dictionary, ready to be written as it stands
};

using AlphaStates = QVector<AlphaState>;

/// @brief One image a rendered drawing draws, carried across with its own bytes.
///
/// **A picture is not something the copied stream can resolve on its own.** Qt renders a drawing
/// into a PDF of its own, where an image is an object the content stream calls by name —
/// `/Im13 Do` — and that name IS the object's number in that file. This module copies the content
/// stream and nothing else, so the call arrived with no object behind it: a reader ignores a name
/// it cannot resolve and says nothing, and the picture was simply absent from the plan.
///
/// **The name never changes.** A name resolves inside the resources of the form that invokes it,
/// so two layers may both call `/Im13` and mean two different pictures; only the object number,
/// new in our own file, differs. That is also why nothing has to be renamed in any stream.
///
/// Two references in the image's own dictionary do have to be rewritten, and both are easy to
/// forget: `/Length`, which Qt always writes as an indirect reference, and the soft mask, whose
/// object nothing else in the file names — it is reachable from this dictionary alone.
struct ImageObject
{
    QByteArray name;       ///< without the leading slash: `Im13`, the name the stream calls
    QByteArray dict;       ///< the dictionary, `/Length` still an indirect reference
    QByteArray stream;     ///< the bytes of the picture, copied as they came
    QByteArray maskKey;    ///< `/SMask` or `/Mask` — the name the dictionary gave it
    QByteArray maskDict;   ///< the mask's own dictionary, empty when there is none
    QByteArray maskStream; ///< and the mask's own bytes
    int        maskOld = 0; ///< the mask's object number in the rendered file, 0 when none
};

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

    /// @param payloadPath  A document to carry ALONGSIDE the pages, unread and unreported —
    ///                 the payload Rabnest relit. Empty — the default — is the closed export:
    ///                 a PDF that prints and that nothing else can reopen.
    ///
    ///                 It goes in under /PieceInfo, the standard place for service data, and
    ///                 never as an embedded file: an entry in the reader's attachment panel
    ///                 is a paperclip, and a paperclip is a click a client should never be
    ///                 offered. What the file contains is not this module's business — it is
    ///                 written byte for byte, compressed, and read back by the same contract.
    bool convert(const QString &svgPath, const QString &pdfPath, const DocumentInfo &info = {},
                 const QString &underlaySvgPath = QString(), bool mepIsLayer = true,
                 const QString &payloadPath = QString());

    /// @brief Write @p svgPath as ONE PDF, one page per rectangle in @p pages.
    ///
    /// This is how a tiled plan is delivered, and how a single-size run is delivered too:
    /// the drawing is written ONCE — one Form XObject per layer — and each page is a
    /// reference to those objects, offset so that its own rectangle of the plan lands on
    /// the sheet. The plan is therefore addressed N times rather than copied N times,
    /// which is what makes a tiled export cost the same whatever its page count, and what
    /// leaves the page's own content stream free for whatever a sheet must carry of its
    /// own.
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
    /// @param underlaySvgPath  A second document, drawn on every page UNDER the layers and
    ///                 outside every optional content group. Empty — the default — is a
    ///                 document with no underlay. It must be laid out on the plan's own
    ///                 page: same size, same viewBox, or it is refused rather than placed
    ///                 on a guess.
    /// @param mepIsLayer  Whether the decor layer is offered as one of the reader's layers.
    ///                 True — the default — is the historical behaviour: it appears in the
    ///                 panel beside the sizes and can be switched off.
    ///
    ///                 False takes the whole of it out of the optional content: no OCG object
    ///                 is allocated, it is named in no /Properties, it appears in neither
    ///                 /OCGs nor /Order, and its invocation in the page stream carries no /OC
    ///                 marker. It is DRAWN exactly the same — the Form is written and invoked
    ///                 as always, in the same place in the same order — and what disappears is
    ///                 only the reader's ability to take it off the page.
    ///
    ///                 That is the point of the switch. A plan whose decor can be switched off
    ///                 is a plan that can be printed without its cartouche, its folds or its
    ///                 annotations and look perfectly normal — the silent kind of wrong. The
    ///                 caller decides; this module only does as it is told.
    ///
    ///                 ⚠️ It also governs the underlay, and that is deliberate: an underlay is
    ///                 drawn on the paper, so it belongs to the decor layer when there is one.
    ///                 With this false the underlay is unconditional, as it has always been —
    ///                 which is why the safe setting is the one that has no layer at all.
    /// @return true on success
    /// @param coverSvgPath  A second document, drawn on every page OVER the underlay, UNDER the
    ///                 layers, and outside every optional content group. Empty — the default — is
    ///                 a document with no cover page.
    ///
    ///                 It is NOT laid out on the plan's page, unlike the underlay: a cover page is
    ///                 a page of its own size, which is exactly what its format gives it. Its own
    ///                 placement is therefore read from its streams and undone, and the plan's put
    ///                 back in its place, so that it lands where the caller's own coordinates say
    ///                 on every sheet — the sheet it is on, and the sheets it is not.
    ///
    ///                 Nothing in the reader's panel reaches it, whatever @p mepIsLayer says: the
    ///                 cover page is not the layout, and a document that can be delivered without
    ///                 its nomenclature is a document nobody should be able to make by accident.
    /// @param coverSheet  The sheet the cover page stands on when its format gives it one of its
    ///                 own, in the same coordinates as @p pages. Empty — the default — leaves the
    ///                 cover page on the plan's own sheets, which is where a format whose sheet can
    ///                 hold it puts it.
    ///
    ///                 **A grafted sheet carries the cover page and nothing else.** It is written
    ///                 FIRST, so the file opens on it. None of the plan is invoked there — not the
    ///                 layers, not the underlay — and that is not an economy: the plan's own frame
    ///                 straddles its outer edge, and a sheet placed against it would print the half
    ///                 of that line which bleeds. A cover page's sheet holds the cover page.
    /// @return true on success
    bool convert(const QString     &svgPath,
                 const QVector<QRectF> &pages,
                 const QString     &pdfPath,
                 const DocumentInfo &info = {},
                 const QString     &underlaySvgPath = QString(),
                 bool               mepIsLayer = true,
                 const QString     &coverSvgPath = QString(),
                 const QRectF      &coverSheet = QRectF(),
                 const QString     &payloadPath = QString());

private:
    struct PageInfo
    {
        QSizeF sizeMm;
        QRectF viewBox;
    };

    /// @brief What a layer IS on the page — the third, fourth and fifth thing a layered
    ///        document can hold besides a size and the decor.
    ///
    /// ⚠️ **The painting order follows it, and the reader's panel too.** A background goes
    /// under everything, calibration grids under the pieces (the plan is drawn in strokes, so
    /// a grid below it reads through it, where a grid above it would hide it), and the decor
    /// stays on top. Before this existed the split was binary and "not the decor" meant "a
    /// size" — see OExportInk in Ostara and the projector's overlays.
    enum class Role
    {
        Size,        ///< une taille du patron — le cas de tous les fichiers d'avant
        Decor,       ///< `ostara:layout="yes"` : le cartouche, les annotations, la mise en page
        Background,  ///< un aplat plein page, sous tout
        Calibration, ///< une grille de calibration, sous les pièces
    };

    struct LayerEntry
    {
        QString    name;        // PDF key, from the id: "Size_46", "Layout". Has to be a name.
        QString    displayName; // what the reader sees: inkscape:label, else <title>, else the id
        bool       isMep;
        Role       role = Role::Size;
        QDomElement element;    // the <g> element
    };

    struct LayerContent
    {
        QString    name;
        QString    displayName;
        bool       isMep;
        Role       role = Role::Size;
        QByteArray stream;        // decompressed PDF content from QSvgRenderer
        QByteArray textStream;    // text operators (MEP only), separate CTM scope
        bool       plain = false; // the whole document: no layer, so no optional content
        QVector<ImageObject> images; // the pictures the stream calls, in the order it calls them
    };

    /// @brief A second document, drawn on every page under every layer, and never optional.
    ///
    /// It is not a layer and never becomes one: no entry in the reader's panel switches it,
    /// and no combination of layer states can hide it. That is the whole point of it. The
    /// frame of a sheet, its registration marks and its numbering belong to the paper the
    /// sheets are printed on, not to what is drawn on them — a sheet whose marks can be
    /// switched off is a sheet that can be assembled wrongly.
    ///
    /// It is read from a document of its own, laid out in the plan's own coordinates, so
    /// that each page shows of it exactly what falls inside that page: the same crop that
    /// decides what the page shows of the plan decides what it shows of this.
    struct Underlay
    {
        QByteArray stream;     // the shapes, as the renderer drew them
        QByteArray textStream; // the text, re-emitted as path operators, its own CTM
        QVector<ImageObject> images; // the pictures it draws — a frame's logo among them

        bool isEmpty() const { return stream.isEmpty() && textStream.isEmpty(); }
    };

    /// @brief The cover page's document, drawn on every sheet and outside every layer.
    ///
    /// It shares the underlay's first property and not its second. Like the underlay it is not a
    /// layer and never becomes one: no entry in the reader's panel switches it, and no combination
    /// of layer states can hide it. A cover page that can be switched off is a plan delivered
    /// without its nomenclature, and it looks perfectly normal that way — which is the silent kind
    /// of wrong, and the whole reason this is not optional content.
    ///
    /// **Unlike the underlay, its page is its own.** The cover page is 19 × 25.9 cm wherever its
    /// format puts it, which is not the plan's size; forcing it onto the plan's page is what the
    /// underlay does and what would then crop it. So its rendered streams carry the placement of
    /// ITS page, and the page stream re-places them against the plan's own frame — read, not
    /// assumed, the way the underlay's placement is compared rather than trusted.
    struct Cover
    {
        /// Its own page, which is what its placement is read against when the stream's cm cannot
        /// be read — the same page the caller laid it out on.
        PageInfo   page;
        QByteArray stream;     // the shapes, as the renderer drew them
        QByteArray textStream; // the text, re-emitted as path operators, its own CTM
        QVector<ImageObject> images; // the pictures it draws — the nomenclature's logo

        bool isEmpty() const { return stream.isEmpty() && textStream.isEmpty(); }
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
        Underlay              underlay;     ///< empty when the caller asked for none
        Cover                 cover;        ///< empty when the caller asked for none

        /// Every alpha state the content above calls by name, whichever layer or document it
        /// came from. Document-wide rather than per layer, because a Form's /Resources is the
        /// only place a name resolves and every Form is written from this one list — sharing it
        /// also means two layers asking for the same state get one object between them.
        AlphaStates           alphaStates;
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
    /// @brief A document of its own, whole, laid out on the page it is given.
    ///
    /// The root a layer's would have — that page's size and viewBox — with every top-level child
    /// carried over, not one group of them: neither an underlay nor a cover page is authored with
    /// layer groups. The page is the caller's, and the two callers differ on purpose — the
    /// underlay's is the plan's, because the two land on the paper together, and the cover page's
    /// is its own, because a cover page is a page.
    QByteArray          buildWholeSvg(const QDomDocument &origDoc, const PageInfo &page) const;
    /// @brief Read the underlay document into the streams a page will invoke it by.
    ///
    /// Refuses a document laid out to other figures than the plan's: it is drawn through
    /// the plan's own translation, so a frame authored at another size would land off its
    /// sheets, and a wrong frame is worse than a missing export.
    bool                readUnderlay(const QString &svgPath, const PageInfo &plan,
                                     Underlay &out, AlphaStates &alpha);
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
    /// @brief The text of @p root, as path operators, in a scope of its own.
    ///
    /// Stayed `const` while gaining @p alpha: it touches no member, and the states it turns out
    /// to need are not its to allocate — the file is not being built yet at the time it runs.
    /// It appends what it used and names it, and buildOcgPdf allocates the lot later.
    QByteArray          emitMepTextAsPdfOps(const QDomElement &root,
                                              const PageInfo   &page,
                                              AlphaStates     &alpha) const;
    /// @brief Read one SVG into its page and the rendered content of each of its layers.
    ///
    /// The whole of what convert() used to do for a single document, lifted out so that a
    /// tiled conversion can call it once per sheet. The caller gets a value it can hold;
    /// nothing here depends on there being exactly one document.
    SvgContent          readSvg(const QString &svgPath,
                                const QString &underlaySvgPath = QString(),
                                const QString &coverSvgPath = QString());

    /// @brief Read the cover page's document into the streams a page will invoke it by.
    ///
    /// No page is compared to the plan's, and that is deliberate: the cover page has a page of its
    /// own, and demanding the plan's would refuse the very document this is for. What takes the
    /// underlay's place here is the placement correction in buildOcgPdf — the streams are drawn
    /// through the plan's translation, so their own placement has to be undone first.
    bool                readCover(const QString &svgPath, Cover &out, AlphaStates &alpha);

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
                                    const DocumentInfo    &info,
                                    bool                   mepIsLayer,
                                    const QRectF          &coverSheet = QRectF(),
                                    const QByteArray      &payload = QByteArray());
};

#endif // SVGTOOCGPDF_H
