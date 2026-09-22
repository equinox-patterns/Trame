/************************************************************************
 **
 **  @file   SvgToOcgPdf.cpp
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

#include "SvgToOcgPdf.h"
#include "PdfBuilder.h"

#include <QBuffer>
#include <QDebug>
#include <QDomDocument>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QPageSize>
#include <QPainter>
#include <QDateTime>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSvgRenderer>
#include <QTextStream>

#include <zlib.h>

#include <cctype>
#include <cmath>
#include <cstdint>

// ── Constants ─────────────────────────────────────────────────────────────

static constexpr double kPointsPerMm = 72.0 / 25.4;  // 1 mm = 2.8346 pt
static constexpr double kPixelsPerMm = 96.0 / 25.4;  // 1 mm = 3.7795 px @ 96 DPI
static constexpr double kPixelToPt   = 72.0 / 96.0;   // SVG pixel → PDF points

/// An OCG name, written as a PDF text string.
///
/// A literal string is read as PDFDocEncoding, so anything above ASCII comes out
/// mangled — and these names are the user's, typed in her own language. The format
/// provides a UTF-16BE hex string for exactly that case, BOM included.
static QByteArray pdfTextString(const QString &text)
{
    bool ascii = true;
    for (const QChar &ch : text) {
        if (ch.unicode() < 0x20 || ch.unicode() > 0x7E) {
            ascii = false;
            break;
        }
    }

    if (!ascii) {
        QByteArray hex("FEFF");
        for (const QChar &ch : text)
            hex += QByteArray::number(static_cast<uint>(ch.unicode()), 16)
                       .rightJustified(4, '0').toUpper();
        return '<' + hex + '>';
    }

    QByteArray literal = text.toLatin1();
    literal.replace('\\', "\\\\");
    literal.replace('(', "\\(");
    literal.replace(')', "\\)");
    return '(' + literal + ')';
}

// ── 2D affine transform ───────────────────────────────────────────────────

struct Affine2D
{
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    Affine2D() = default;
    Affine2D(double a_, double b_, double c_, double d_, double e_, double f_)
        : a(a_), b(b_), c(c_), d(d_), e(e_), f(f_) {}

    void apply(double &x, double &y) const
    {
        const double nx = a * x + c * y + e;
        y = b * x + d * y + f;
        x = nx;
    }
};

/// Parse a single SVG transform function.
static bool parseOneTransform(const QString &fn, Affine2D &m)
{
    const int paren = fn.indexOf(QLatin1Char('('));
    if (paren < 0) return false;
    const QString name = fn.left(paren).trimmed().toLower();
    const int rparen = fn.lastIndexOf(QLatin1Char(')'));
    if (rparen < 0) return false;

    QString argStr = QStringView(fn).mid(paren + 1, rparen - paren - 1).toString();
    argStr.replace(QLatin1Char(','), QLatin1Char(' '));
    const QStringList parts = argStr.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QVector<double> vals;
    vals.reserve(parts.size());
    for (const auto &p : parts) {
        bool ok = false;
        double v = p.toDouble(&ok);
        if (!ok) return false;
        vals.append(v);
    }

    if (name == QStringLiteral("translate")) {
        m = Affine2D{1, 0, 0, 1, vals.value(0), vals.value(1)};
        return true;
    }
    if (name == QStringLiteral("scale")) {
        if (vals.isEmpty()) return false;
        const double sx = vals[0];
        const double sy = vals.size() >= 2 ? vals[1] : sx;
        m = Affine2D{sx, 0, 0, sy, 0, 0};
        return true;
    }
    if (name == QStringLiteral("rotate")) {
        if (vals.isEmpty()) return false;
        const double rad = vals[0] * M_PI / 180.0;
        const double c = std::cos(rad), s = std::sin(rad);
        if (vals.size() >= 3) {
            const double cx = vals[1], cy = vals[2];
            m = Affine2D{c, s, -s, c, -cx * c + cy * s + cx, -cx * s - cy * c + cy};
        } else {
            m = Affine2D{c, s, -s, c, 0, 0};
        }
        return true;
    }
    if (name == QStringLiteral("matrix")) {
        if (vals.size() < 6) return false;
        m = Affine2D{vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]};
        return true;
    }
    return false;
}

/// Parse a full SVG transform attribute string.
/// SVG applies transforms right-to-left; returns the composed matrix.
static Affine2D parseSvgTransform(const QString &tx)
{
    Affine2D result;
    if (tx.isEmpty()) return result;

    QStringList funcs;
    int depth = 0, fnStart = 0;
    for (int i = 0; i < tx.size(); ++i) {
        const QChar ch = tx[i];
        if (ch == QLatin1Char('(')) {
            if (depth == 0) {
                fnStart = i - 1;
                while (fnStart >= 0 && tx[fnStart] != QLatin1Char(' '))
                    fnStart--;
                fnStart++;
            }
            depth++;
        } else if (ch == QLatin1Char(')')) {
            depth--;
            if (depth == 0)
                funcs.append(tx.mid(fnStart, i - fnStart + 1));
        }
    }

    if (funcs.isEmpty()) return result;

    // Compose right-to-left: SVG transform="A B C" applies C first, then B, then A.
    // The matrix is: A × B × C. Iterate in reverse so the rightmost function
    // is composed first.
    for (int i = funcs.size() - 1; i >= 0; --i) {
        Affine2D m;
        if (parseOneTransform(funcs[i], m)) {
            // Pre-multiply: result = m × result
            result = Affine2D{
                m.a * result.a + m.c * result.b,
                m.b * result.a + m.d * result.b,
                m.a * result.c + m.c * result.d,
                m.b * result.c + m.d * result.d,
                m.a * result.e + m.c * result.f + m.e,
                m.b * result.e + m.d * result.f + m.f
            };
        }
    }

    return result;
}

/// Parse an SVG colour string to PDF RGB components (0..1).
static bool parseColor(const QString &s, double &r, double &g, double &b)
{
    if (s.isEmpty() || s == QStringLiteral("none"))
        return false;

    if (s.startsWith(QLatin1Char('#'))) {
        const QStringView hex = QStringView(s).mid(1);
        auto hexVal = [](QChar c) -> int {
            const ushort u = c.unicode();
            if (u >= '0' && u <= '9') return u - '0';
            if (u >= 'a' && u <= 'f') return u - 'a' + 10;
            if (u >= 'A' && u <= 'F') return u - 'A' + 10;
            return 0;
        };
        if (hex.size() == 3) {
            r = hexVal(hex[0]) / 15.0;
            g = hexVal(hex[1]) / 15.0;
            b = hexVal(hex[2]) / 15.0;
            return true;
        }
        if (hex.size() == 6) {
            r = (hexVal(hex[0]) * 16 + hexVal(hex[1])) / 255.0;
            g = (hexVal(hex[2]) * 16 + hexVal(hex[3])) / 255.0;
            b = (hexVal(hex[4]) * 16 + hexVal(hex[5])) / 255.0;
            return true;
        }
        return false;
    }

    if (s.startsWith(QStringLiteral("rgb(")) && s.endsWith(QLatin1Char(')'))) {
        const QStringView inner = QStringView(s).mid(4, s.size() - 5);
        const QStringList parts = inner.toString().split(QLatin1Char(','));
        if (parts.size() == 3) {
            r = parts[0].trimmed().toDouble() / 255.0;
            g = parts[1].trimmed().toDouble() / 255.0;
            b = parts[2].trimmed().toDouble() / 255.0;
            return true;
        }
    }

    return false;
}

// ── Public API ────────────────────────────────────────────────────────────

bool SvgToOcgPdf::convert(const QString &svgPath, const QString &pdfPath,
                          const DocumentInfo &info, const QString &underlaySvgPath,
                          bool mepIsLayer)
{
    // One page, and the document is that page: the rectangle covers everything the SVG
    // draws. This is what the svg2ocgpdf tool asks for, and it is the shape a single-size
    // run takes — a document with nothing to cut up is a document with one sheet.
    const SvgContent doc = readSvg(svgPath, underlaySvgPath);
    if (!doc.ok)
        return false;

    const QRectF whole(0.0, 0.0, doc.page.sizeMm.width() * kPixelsPerMm,
                       doc.page.sizeMm.height() * kPixelsPerMm);

    return buildOcgPdf(pdfPath, doc, QVector<QRectF>{whole}, info, mepIsLayer);
}

bool SvgToOcgPdf::convert(const QString &svgPath, const QVector<QRectF> &pages,
                          const QString &pdfPath, const DocumentInfo &info,
                          const QString &underlaySvgPath, bool mepIsLayer,
                          const QString &coverSvgPath, const QRectF &coverSheet)
{
    if (pages.isEmpty()) {
        qWarning() << "SvgToOcgPdf: no page to write";
        return false;
    }

    for (const QRectF &page : pages) {
        // PDF 32000-1 §14.11.2 asks a /MediaBox to measure at least 3 by 3 units, and a
        // page with no surface is not a page whatever the spec says. Refusing beats
        // emitting a sheet nobody can print.
        if (page.width() <= 0.0 || page.height() <= 0.0) {
            qWarning() << "SvgToOcgPdf: a page rectangle has no surface" << page;
            return false;
        }
    }

    const SvgContent doc = readSvg(svgPath, underlaySvgPath, coverSvgPath);
    if (!doc.ok) {
        // An unreadable document is not a plan with one sheet missing: it is every sheet
        // wrong, because they all draw the same thing. Handing over nothing beats handing
        // over a plan that cannot be reassembled. The underlay reads the same way, and for
        // the same reason: it is on every sheet or on none.
        return false;
    }

    return buildOcgPdf(pdfPath, doc, pages, info, mepIsLayer, coverSheet);
}

namespace
{
/// Declared here and defined with the other readers of the rendered document, further down.
/// readSvg comes first in this file and is one of the two callers: the alpha states have to be
/// lifted out of the buffer while it is still alive, and that is inside readSvg, not after it.
void CollectAlphaStates(const QByteArray &pdfBuf, const QByteArray &stream, AlphaStates &into);
}  // namespace

SvgToOcgPdf::SvgContent SvgToOcgPdf::readSvg(const QString &svgPath,
                                              const QString &underlaySvgPath,
                                              const QString &coverSvgPath)
{
    SvgContent result;

    QFile svgFile(svgPath);
    if (!svgFile.open(QIODevice::ReadOnly)) {
        qWarning() << "SvgToOcgPdf: cannot open" << svgPath;
        return result;
    }
    const QByteArray svgData = svgFile.readAll();
    svgFile.close();

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorCol;
    if (!doc.setContent(svgData, &errorMsg, &errorLine, &errorCol)) {
        qWarning() << "SvgToOcgPdf: XML parse error at line" << errorLine
                   << ":" << errorMsg;
        return result;
    }

    result.page = getPageInfo(doc);

    // The underlay is read before anything else is decided, and it is read for every path
    // out of here — the layered document and the flat one alike. It is on every sheet or on
    // none: a frame that made it onto half the paper would be worse than no export at all.
    if (!underlaySvgPath.isEmpty()
        && !readUnderlay(underlaySvgPath, result.page, result.underlay, result.alphaStates)) {
        return result; // ok stays false
    }

    // The cover page is read the same way and for the same reason — on every sheet or on none —
    // and refuses nothing of the plan's: it has a page of its own by design. What stands in for
    // the underlay's size check is the placement correction in buildOcgPdf.
    if (!coverSvgPath.isEmpty()
        && !readCover(coverSvgPath, result.cover, result.alphaStates)) {
        return result; // ok stays false
    }

    const QVector<LayerEntry> layers = findLayers(doc);

    // No layer group at all: a single-size document. It is still owed a PDF — one plain
    // content stream and no optional content, since there is nothing to switch on and
    // off. Refusing it would have left the commonest case with no export at all.
    if (layers.isEmpty()) {
        // A progress line, not a fault — qDebug, never qWarning: a qWarning opens a modal
        // box in Ostara, and a tiled single-size run would open one PER SHEET.
        qDebug() << "SvgToOcgPdf: no layer group, writing a flat page";

        const QByteArray pdfBuf = renderToPdfBuffer(svgData, result.page);
        if (pdfBuf.isEmpty()) {
            qWarning() << "SvgToOcgPdf: failed to render the document";
            return result;
        }

        const QByteArray stream = extractContentStream(pdfBuf);
        if (stream.isEmpty()) {
            qWarning() << "SvgToOcgPdf: failed to extract the content stream";
            return result;
        }

        // The states the drawing reaches for, taken while the rendered document is still here:
        // the buffer does not outlive this function, and buildOcgPdf — the only place the
        // output file exists — will need the bodies long after it is gone.
        CollectAlphaStates(pdfBuf, stream, result.alphaStates);

        LayerContent whole;
        whole.name = QStringLiteral("Document");
        whole.stream = stream;
        whole.plain = true;

        result.layers.append(whole);
        result.plain = true;
        result.ok = true;
        return result;
    }

    qDebug() << "Found" << layers.size() << "layers, page"
               << result.page.sizeMm.width() << "x" << result.page.sizeMm.height() << "mm";

    result.layers.reserve(layers.size());

    for (const auto &layer : layers) {
        qDebug() << "  Rendering layer:" << layer.name;

        const QByteArray layerSvg = buildLayerSvg(doc, layer.element, result.page);
        const QByteArray pdfBuf = renderToPdfBuffer(layerSvg, result.page);
        if (pdfBuf.isEmpty()) {
            qWarning() << "  Failed to render layer:" << layer.name;
            continue;
        }

        const QByteArray stream = extractContentStream(pdfBuf);
        if (stream.isEmpty()) {
            qWarning() << "  Failed to extract stream for layer:" << layer.name;
            continue;
        }

        CollectAlphaStates(pdfBuf, stream, result.alphaStates);

        QByteArray textStream;
        // For MEP layer, generate text as PDF path operators
        // (kept separate — must not be affected by QSvgRenderer's CTM)
        if (layer.isMep) {
            textStream = emitMepTextAsPdfOps(layer.element, result.page, result.alphaStates);
        }

        result.layers.append({layer.name, layer.displayName, layer.isMep, stream, textStream});
    }

    if (result.layers.isEmpty()) {
        qWarning() << "SvgToOcgPdf: no layer content generated";
        return result;
    }

    result.ok = true;
    return result;
}

// ── Private methods ───────────────────────────────────────────────────────

QVector<SvgToOcgPdf::LayerEntry> SvgToOcgPdf::findLayers(const QDomDocument &doc) const
{
    QVector<LayerEntry> result;

    const QDomElement root = doc.documentElement();
    for (QDomElement el = root.firstChildElement(); !el.isNull();
         el = el.nextSiblingElement()) {

        if (el.tagName() != QStringLiteral("g"))
            continue;

        const QString id = el.attribute(QStringLiteral("id"));
        const bool hasAiLayer = el.attribute(QStringLiteral("i:layer")) == QStringLiteral("yes");
        const bool hasInkscapeLayer = el.attribute(QStringLiteral("inkscape:groupmode")) == QStringLiteral("layer");

        if (!hasAiLayer && !hasInkscapeLayer)
            continue;

        // The decor layer says so itself. It used to be recognised by its name — it had
        // to contain "Mise" or "en_page" — which could only work while the name was
        // imposed. It is not: the user types it in the size range dialog. The marker
        // exists for that, and a layer renamed in her own words is still the decor.
        const bool isMep = el.attribute(QStringLiteral("ostara:layout")) == QStringLiteral("yes");

        LayerEntry entry;
        // The id addresses the layer inside the PDF, so it has to be a name whatever the
        // writer gave us.
        entry.name = id.isEmpty() ? QStringLiteral("layer_%1").arg(result.size()) : id;
        entry.displayName = displayNameOf(el);
        entry.isMep = isMep;
        entry.element = el;

        result.append(entry);
        qDebug() << "  Layer:" << entry.name << (isMep ? "(decor)" : "");
    }

    return result;
}

QString SvgToOcgPdf::displayNameOf(const QDomElement &element)
{
    const QString label = element.attribute(QStringLiteral("inkscape:label")).trimmed();
    if (!label.isEmpty())
        return label;

    const QString title = element.firstChildElement(QStringLiteral("title")).text().trimmed();
    if (!title.isEmpty())
        return title;

    return element.attribute(QStringLiteral("id"));
}

SvgToOcgPdf::PageInfo SvgToOcgPdf::getPageInfo(const QDomDocument &doc) const
{
    PageInfo info;
    const QDomElement root = doc.documentElement();

    // Parse width/height (may have "mm" suffix)
    auto mmVal = [](const QString &s) -> double {
        if (s.endsWith(QStringLiteral("mm"), Qt::CaseInsensitive))
            return s.chopped(2).trimmed().toDouble();
        // If no unit, assume mm (already in mm from Valentina export)
        return s.toDouble();
    };

    info.sizeMm = QSizeF(mmVal(root.attribute(QStringLiteral("width"))),
                          mmVal(root.attribute(QStringLiteral("height"))));

    // Parse viewBox
    const QStringList vb = root.attribute(QStringLiteral("viewBox"))
                                .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (vb.size() >= 4) {
        info.viewBox = QRectF(vb[0].toDouble(), vb[1].toDouble(),
                               vb[2].toDouble(), vb[3].toDouble());
    }

    return info;
}

/// @brief A root for a document holding one drawing, laid out on the plan's own page.
///
/// Every drawing this module renders — each layer, and the underlay — is rendered this way,
/// and they have to share it: they are placed on the paper through the same arithmetic, so
/// a root that measured differently would put one of them off its sheets.
static QDomElement makeSvgRoot(QDomDocument &doc, const QSizeF &sizeMm, const QRectF &viewBox)
{
    QDomElement svgRoot = doc.createElement(QStringLiteral("svg"));
    svgRoot.setAttribute(QStringLiteral("xmlns"), QStringLiteral("http://www.w3.org/2000/svg"));
    svgRoot.setAttribute(QStringLiteral("viewBox"),
                         QStringLiteral("%1 %2 %3 %4")
                             .arg(viewBox.x(), 0, 'f', 2)
                             .arg(viewBox.y(), 0, 'f', 2)
                             .arg(viewBox.width(), 0, 'f', 2)
                             .arg(viewBox.height(), 0, 'f', 2));
    svgRoot.setAttribute(QStringLiteral("width"),
                         QStringLiteral("%1mm").arg(sizeMm.width(), 0, 'f', 4));
    svgRoot.setAttribute(QStringLiteral("height"),
                         QStringLiteral("%1mm").arg(sizeMm.height(), 0, 'f', 4));
    return svgRoot;
}

/// @brief Drop every <text> descendant, before the drawing is rendered.
///
/// QSvgRenderer draws text with a font, and a form's resources carry none: the text would
/// reach the page as a reference to a font that is not there. The shapes go through the
/// renderer and the text comes back beside them as path operators.
static void removeTextElements(QDomElement root)
{
    QVector<QDomElement> toRemove;
    std::function<void(QDomElement)> walk = [&](QDomElement el) {
        for (QDomElement child = el.firstChildElement(); !child.isNull();
             child = child.nextSiblingElement()) {
            if (child.tagName() == QStringLiteral("text"))
                toRemove.append(child);
            else
                walk(child);
        }
    };
    walk(root);
    for (auto &el : toRemove)
        el.parentNode().removeChild(el);
}

QByteArray SvgToOcgPdf::buildLayerSvg(const QDomDocument &origDoc,
                                       const QDomElement   &layerEl,
                                       const PageInfo      &page) const
{
    QDomDocument layerDoc;

    // Processing instruction
    QDomNode pi = layerDoc.createProcessingInstruction(
        QStringLiteral("xml"), QStringLiteral("version=\"1.0\" encoding=\"UTF-8\""));
    layerDoc.appendChild(pi);

    QDomElement svgRoot = makeSvgRoot(layerDoc, page.sizeMm, page.viewBox);

    // Deep-clone the layer element; its text goes out and comes back as path operators.
    QDomNode cloned = layerDoc.importNode(layerEl, true);
    removeTextElements(cloned.toElement());

    svgRoot.appendChild(cloned);
    layerDoc.appendChild(svgRoot);

    return layerDoc.toByteArray(-1); // -1 = no indent, compact XML
}

QByteArray SvgToOcgPdf::buildWholeSvg(const QDomDocument &origDoc, const PageInfo &page) const
{
    QDomDocument underlayDoc;

    QDomNode pi = underlayDoc.createProcessingInstruction(
        QStringLiteral("xml"), QStringLiteral("version=\"1.0\" encoding=\"UTF-8\""));
    underlayDoc.appendChild(pi);

    QDomElement svgRoot = makeSvgRoot(underlayDoc, page.sizeMm, page.viewBox);

    // Every top-level child, not one group of them: an underlay is not authored with layer
    // groups, and the namespaces the original root declared are no use to a document that
    // carries nothing but plain SVG.
    for (QDomElement child = origDoc.documentElement().firstChildElement(); !child.isNull();
         child = child.nextSiblingElement()) {
        svgRoot.appendChild(underlayDoc.importNode(child, true));
    }
    removeTextElements(svgRoot);

    underlayDoc.appendChild(svgRoot);

    return underlayDoc.toByteArray(-1);
}

bool SvgToOcgPdf::readUnderlay(const QString &svgPath, const PageInfo &plan,
                               Underlay &out, AlphaStates &alpha)
{
    QFile file(svgPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "SvgToOcgPdf: cannot open the underlay" << svgPath;
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorCol;
    if (!doc.setContent(data, &errorMsg, &errorLine, &errorCol)) {
        qWarning() << "SvgToOcgPdf: XML parse error in the underlay at line" << errorLine
                   << ":" << errorMsg;
        return false;
    }

    // Drawn through the plan's own translation, so it has to be laid out to the plan's own
    // figures. Read them and compare rather than assume: a frame authored at another size
    // would land half off its sheets, and would say nothing.
    const PageInfo own = getPageInfo(doc);
    if (own.sizeMm != plan.sizeMm || own.viewBox != plan.viewBox) {
        qWarning() << "SvgToOcgPdf: the underlay is not laid out like the plan —"
                   << own.sizeMm << own.viewBox << "against" << plan.sizeMm << plan.viewBox;
        return false;
    }

    const QByteArray pdfBuf = renderToPdfBuffer(buildWholeSvg(doc, plan), plan);
    if (pdfBuf.isEmpty()) {
        qWarning() << "SvgToOcgPdf: failed to render the underlay";
        return false;
    }

    out.stream = extractContentStream(pdfBuf);
    if (out.stream.isEmpty()) {
        qWarning() << "SvgToOcgPdf: failed to extract the underlay stream";
        return false;
    }

    // Its text goes the way the layout layer's text goes, and for the same reason: the
    // renderer cannot draw it into a form that carries no font, so it is re-emitted here as
    // path operators, in a scope of its own.
    CollectAlphaStates(pdfBuf, out.stream, alpha);

    out.textStream = emitMepTextAsPdfOps(doc.documentElement(), plan, alpha);
    return true;
}

bool SvgToOcgPdf::readCover(const QString &svgPath, Cover &out, AlphaStates &alpha)
{
    QFile file(svgPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "SvgToOcgPdf: cannot open the cover page" << svgPath;
        return false;
    }
    const QByteArray data = file.readAll();
    file.close();

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorCol;
    if (!doc.setContent(data, &errorMsg, &errorLine, &errorCol)) {
        qWarning() << "SvgToOcgPdf: XML parse error in the cover page at line" << errorLine
                   << ":" << errorMsg;
        return false;
    }

    // **Its own page, and nothing of the plan's is compared to it.** The underlay is refused when
    // its page is not the plan's because it is drawn through the plan's translation and has no
    // other answer; the cover page is the opposite case, and demanding the plan's figures would
    // refuse the very document this exists for. Where the underlay is checked, the cover is
    // re-placed — see buildOcgPdf, which reads what Qt actually wrote.
    out.page = getPageInfo(doc);

    // The same two steps the underlay takes, on the page this document declares rather than on the
    // plan's: rendered at its own size, and with its text re-emitted as path operators, because a
    // form carries no font.
    const QByteArray pdfBuf = renderToPdfBuffer(buildWholeSvg(doc, out.page), out.page);
    if (pdfBuf.isEmpty()) {
        qWarning() << "SvgToOcgPdf: failed to render the cover page";
        return false;
    }

    out.stream = extractContentStream(pdfBuf);
    if (out.stream.isEmpty()) {
        qWarning() << "SvgToOcgPdf: failed to extract the cover page stream";
        return false;
    }

    CollectAlphaStates(pdfBuf, out.stream, alpha);

    // The page goes with it, and not merely its height: this document's viewBox does not begin at
    // the origin, and the re-emission reads both axes off the page it is given.
    out.textStream = emitMepTextAsPdfOps(doc.documentElement(), out.page, alpha);
    return true;
}

QByteArray SvgToOcgPdf::renderToPdfBuffer(const QByteArray &svgData,
                                            const PageInfo   &page) const
{
    QSvgRenderer renderer(svgData);
    if (!renderer.isValid())
        return {};

    QBuffer buffer;
    if (!buffer.open(QIODevice::WriteOnly))
        return {};

    QPdfWriter writer(&buffer);
    writer.setResolution(96);
    writer.setPdfVersion(QPagedPaintDevice::PdfVersion_1_4);
    writer.setPageSize(QPageSize(page.sizeMm, QPageSize::Millimeter));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));

    const double pageWPx = page.sizeMm.width() * kPixelsPerMm;
    const double pageHPx = page.sizeMm.height() * kPixelsPerMm;

    QPainter painter(&writer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    renderer.render(&painter, QRectF(0, 0, pageWPx, pageHPx));
    painter.end();

    buffer.close();
    return buffer.data();
}

namespace
{
/// @brief The /Length an object declares for its stream, or -1 when it declares none.
///
/// The value is often an indirect reference — /Length 12 0 R — in which case the number is
/// read from the object it names. Guessing the length from where "endstream" happens to sit
/// is what this replaces: the last byte of compressed data is as likely to be a space as
/// any other, and a whitespace trim ate one whenever it was.
auto DeclaredStreamLength(const QByteArray &pdfBuf, const QByteArray &objectHeader) -> int
{
    const QByteArray marker = "/Length";
    int cursor = objectHeader.indexOf(marker);
    if (cursor < 0)
        return -1;

    cursor += static_cast<int>(marker.size());
    while (cursor < objectHeader.size() && objectHeader[cursor] == ' ')
        ++cursor;

    int value = 0;
    bool digits = false;
    while (cursor < objectHeader.size() && objectHeader[cursor] >= '0' && objectHeader[cursor] <= '9') {
        value = value * 10 + (objectHeader[cursor] - '0');
        ++cursor;
        digits = true;
    }
    if (!digits)
        return -1;

    // "12 0 R" — the number names an object that holds the length, it is not the length.
    int probe = cursor;
    while (probe < objectHeader.size() && objectHeader[probe] == ' ')
        ++probe;
    if (probe >= objectHeader.size() || objectHeader[probe] != '0')
        return value;

    ++probe;
    while (probe < objectHeader.size() && objectHeader[probe] == ' ')
        ++probe;
    if (probe >= objectHeader.size() || objectHeader[probe] != 'R')
        return value;

    const QByteArray holder = QByteArray::number(value) + " 0 obj";
    const int holderAt = pdfBuf.indexOf(holder);
    if (holderAt < 0)
        return -1;

    int at = holderAt + static_cast<int>(holder.size());
    while (at < pdfBuf.size() && (pdfBuf[at] == '\n' || pdfBuf[at] == '\r' || pdfBuf[at] == ' '))
        ++at;

    int resolved = 0;
    digits = false;
    while (at < pdfBuf.size() && pdfBuf[at] >= '0' && pdfBuf[at] <= '9') {
        resolved = resolved * 10 + (pdfBuf[at] - '0');
        ++at;
        digits = true;
    }

    return digits ? resolved : -1;
}

/// @brief The alpha states a rendered stream reaches for, in the order it first reaches for them.
///
/// The names are read out of the stream itself. Qt writes `/GState11 gs`, and 11 is the number of
/// the object that held that state in ITS file — so the stream already carries everything needed
/// to find it again, and the source's /Resources dictionary never has to be parsed.
///
/// Read from the stream rather than from the resources on purpose: a state the drawing never
/// invokes is one there is no reason to carry across, and the stream is the only authority on
/// what it will actually call.
auto ReferencedAlphaStates(const QByteArray &stream) -> QVector<int>
{
    const QByteArray marker = "/GState";
    QVector<int> numbers;

    int at = 0;
    while (true) {
        const int found = stream.indexOf(marker, at);
        if (found < 0)
            break;
        at = found + static_cast<int>(marker.size());

        int cursor = at;
        int number = 0;
        bool digits = false;
        while (cursor < stream.size() && stream[cursor] >= '0' && stream[cursor] <= '9') {
            number = number * 10 + (stream[cursor] - '0');
            ++cursor;
            digits = true;
        }

        // What has to follow is "gs", the operator that invokes the state. `/GState11` spelled
        // without it is how an entry is NAMED in a /Resources dictionary — a declaration, not an
        // invocation — and there is none of those in a content stream anyway.
        while (cursor < stream.size() && stream[cursor] == ' ')
            ++cursor;
        const bool invoked = digits && cursor + 1 < stream.size()
                             && stream[cursor] == 'g' && stream[cursor + 1] == 's';

        if (invoked && !numbers.contains(number))
            numbers.append(number);
    }

    return numbers;
}

/// @brief The body of object @p num as @p pdfBuf wrote it, or empty when it is not there.
///
/// This is the piece the module never had: everything else here reads STREAMS, and a graphics
/// state is a plain dictionary, so there was no way to lift one out of the rendered document.
///
/// The search is bounded the way the rest of this file has learned to bound it. `N 0 obj` occurs
/// inside compressed data often enough that a bare `indexOf` has already put a slice in the
/// middle of a stream once, and the decor layer paid for it — see the warning in
/// extractContentStream. Two things keep it honest here: the header must not be the tail of a
/// longer number, and a dictionary must follow it. A byte inside an image spells neither.
auto ExtractObjectBody(const QByteArray &pdfBuf, int num) -> QByteArray
{
    const QByteArray header = QByteArray::number(num) + " 0 obj";
    int from = 0;

    while (true) {
        const int at = pdfBuf.indexOf(header, from);
        if (at < 0)
            return {};
        from = at + 1;

        // Not the tail of a longer number: "11 0 obj" is spelled inside "111 0 obj".
        if (at > 0 && pdfBuf[at - 1] != '\n' && pdfBuf[at - 1] != '\r' && pdfBuf[at - 1] != ' ')
            continue;

        int cursor = at + static_cast<int>(header.size());
        while (cursor < pdfBuf.size()
               && (pdfBuf[cursor] == ' ' || pdfBuf[cursor] == '\n' || pdfBuf[cursor] == '\r'
                   || pdfBuf[cursor] == '\t')) {
            ++cursor;
        }

        if (cursor + 1 >= pdfBuf.size() || pdfBuf[cursor] != '<' || pdfBuf[cursor + 1] != '<')
            continue;

        const int end = pdfBuf.indexOf("endobj", cursor);
        if (end < 0)
            return {};

        return pdfBuf.mid(cursor, end - cursor);
    }
}

/// @brief The state that carries @p opacity, added to @p alpha the first time it is asked for.
///
/// The re-emitted text has no object behind it in the rendered document — nothing upstream ever
/// gave it a state, since it is drawn here from a `QPainterPath` — so its state has to be made.
/// The name is therefore ours, and it is the alpha itself in thousandths: `GSt400` is 40 %.
///
/// By value rather than by rank, so that the name does not depend on how many texts came before
/// it or in what order. Two numbers at the same opacity share one state, and reading the same
/// document twice writes the same file.
///
/// A thousandth is the resolution, and it is far finer than any of this is looked at: the field
/// the user sets is a whole percent, and `fill-opacity` arrives from it.
auto AlphaNameFor(AlphaStates &alpha, double opacity) -> QString
{
    const int perMille = qBound(1, qRound(opacity * 1000.0), 999);
    const QString name = QStringLiteral("GSt%1").arg(perMille);

    for (const AlphaState &state : alpha) {
        if (state.name == name)
            return name;
    }

    // Shaped like the one Qt writes for itself, so that a reader meets one kind of state in this
    // file rather than two.
    alpha.append({name, QStringLiteral("<< /Type /ExtGState /SA true /SM 0.02"
                                       " /ca %1 /CA %1 /AIS false /SMask /None >>\n")
                            .arg(perMille / 1000.0, 0, 'f', 3).toUtf8()});
    return name;
}

/// @brief Adds every alpha state @p stream reaches for, copied out of the document it came from.
///
/// Silent about a state it has already taken: two layers invoking the same state share one
/// object, exactly as they shared one in the file Qt wrote.
void CollectAlphaStates(const QByteArray &pdfBuf, const QByteArray &stream, AlphaStates &into)
{
    for (int num : ReferencedAlphaStates(stream)) {
        const QString name = QStringLiteral("GState%1").arg(num);

        bool known = false;
        for (const AlphaState &state : into) {
            if (state.name == name) {
                known = true;
                break;
            }
        }
        if (known)
            continue;

        QByteArray body = ExtractObjectBody(pdfBuf, num);
        if (body.isEmpty()) {
            // Worth saying out loud: the drawing will print solid there, and nothing else in
            // the file will admit it.
            qWarning() << "SvgToOcgPdf: the alpha state" << name
                       << "is invoked but was not found in the rendered document";
            continue;
        }

        // Qt writes these objects as `<< /ca … /CA … >>`, with no /Type — and the specification
        // requires one on a graphics state dictionary. Copied as it stands, the entry would be a
        // dictionary that never says what it is: taken by a forgiving reader, refused by a strict
        // one. Put in just after the opening braces, where a dictionary's first entry belongs.
        body.insert(2, " /Type /ExtGState");

        into.append({name, body});
    }
}
}  // namespace

QByteArray SvgToOcgPdf::extractContentStream(const QByteArray &pdfBuf)
{
    // The page names its content either by one reference — /Contents 5 0 R — or by an array
    // of them, 5 0 R 6 0 R, when the drawing was written in several parts. Both have to be
    // read: taking only the first number drops the rest of the drawing on the floor.
    //
    // Each part is then located by searching for its header from the part before it, never
    // from the start of the file. "6 0 obj" occurs inside compressed data often enough, and
    // a header found there starts the slice in the middle of a stream: inflate then gives
    // up partway with Z_BUF_ERROR and the layer comes out empty. That is what happened to
    // the decor layer, whose buffer is the largest and so the likeliest to contain one.
    const QByteArray marker = "/Contents";
    const int at = pdfBuf.indexOf(marker);
    if (at < 0) {
        return {};
    }

    int cursor = at + static_cast<int>(marker.size());
    QVector<int> parts;
    while (true) {
        while (cursor < pdfBuf.size()
               && (pdfBuf[cursor] == ' ' || pdfBuf[cursor] == '\n' || pdfBuf[cursor] == '\r'
                   || pdfBuf[cursor] == '\t' || pdfBuf[cursor] == '[')) {
            ++cursor;
        }
        if (cursor >= pdfBuf.size() || pdfBuf[cursor] < '0' || pdfBuf[cursor] > '9') {
            break;
        }

        int number = 0;
        while (cursor < pdfBuf.size() && pdfBuf[cursor] >= '0' && pdfBuf[cursor] <= '9') {
            number = number * 10 + (pdfBuf[cursor] - '0');
            ++cursor;
        }

        // What must follow is " 0 R" — anything else and this was not a reference.
        int probe = cursor;
        while (probe < pdfBuf.size() && pdfBuf[probe] == ' ') ++probe;
        if (probe >= pdfBuf.size() || pdfBuf[probe] != '0') break;
        ++probe;
        while (probe < pdfBuf.size() && pdfBuf[probe] == ' ') ++probe;
        if (probe >= pdfBuf.size() || pdfBuf[probe] != 'R') break;

        if (number > 0) {
            parts.append(number);
        }
        cursor = probe + 1;
    }

    if (parts.isEmpty()) {
        return {};
    }

    QByteArray content;
    int from = 0;
    for (int number : parts) {
        const QByteArray header = QByteArray::number(number) + " 0 obj";
        const int headerAt = pdfBuf.indexOf(header, from);
        if (headerAt < 0) continue;

        int start = pdfBuf.indexOf("stream", headerAt + static_cast<int>(header.size()));
        if (start < 0) continue;
        start += 6;  // past "stream"
        if (start < pdfBuf.size() && pdfBuf[start] == '\r') ++start;
        if (start < pdfBuf.size() && pdfBuf[start] == '\n') ++start;

        const int endAt = pdfBuf.indexOf("endstream", start);
        if (endAt < 0) continue;

        // The object says how long its stream is; take it at its word. Working back from
        // "endstream" instead — which is what this did — removes a real byte whenever the
        // last one of the compressed data happens to be a space, a newline or a carriage
        // return. inflate then consumes everything, asks for one more, and gives up with
        // Z_BUF_ERROR. Eight layers got away with it; the decor layer did not.
        int length = DeclaredStreamLength(pdfBuf, pdfBuf.mid(headerAt, start - headerAt));
        if (length <= 0 || start + length > endAt) {
            // Nothing usable declared: fall back to a single line break before "endstream".
            length = endAt - start;
            if (length > 0 && pdfBuf[start + length - 1] == '\n') --length;
            if (length > 0 && pdfBuf[start + length - 1] == '\r') --length;
        }

        content += decompressFlate(pdfBuf.mid(start, length));
        from = endAt;
    }

    if (parts.size() > 1) {
        qDebug() << "  Content written in" << parts.size() << "parts";
    }

    return content;
}

QByteArray SvgToOcgPdf::decompressFlate(const QByteArray &compressed)
{
    if (compressed.isEmpty())
        return {};

    z_stream strm = {};
    // Use default window bits (15 + 32 for auto-detect zlib/gzip header)
    int ret = inflateInit2(&strm, 15 + 32);
    if (ret != Z_OK) {
        qWarning() << "decompressFlate: inflateInit2 failed with" << ret;
        return {};
    }

    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
    strm.avail_in = static_cast<uInt>(compressed.size());

    QByteArray result;
    constexpr size_t kChunkSize = 32768;
    QByteArray chunk(kChunkSize, Qt::Uninitialized);

    do {
        strm.next_out = reinterpret_cast<Bytef *>(chunk.data());
        strm.avail_out = kChunkSize;
        const uInt inBefore = strm.avail_in;
        ret = inflate(&strm, Z_NO_FLUSH);

        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            qWarning() << "decompressFlate: inflate failed with" << ret;
            inflateEnd(&strm);
            return {};
        }

        result.append(chunk.data(), kChunkSize - strm.avail_out);

        // Z_BUF_ERROR: inflate could neither consume nor produce — the input ran out before
        // the end of the stream. It is not a hard error, so the guard above let it through,
        // and the loop only ever left on Z_STREAM_END. A truncated stream therefore span-
        // ned for ever, appending nothing, and the caller — the UI thread — froze with it.
        // A malformed stream has to fail here, not hang: whoever hands us one can be told,
        // a frozen application can only be force-quit.
        //
        // Z_STREAM_END is excluded from that guard, and it matters: a stream that ends
        // exactly on a chunk boundary makes one last call that consumes nothing and
        // produces nothing. That is a complete stream, not a stalled one — guarding on
        // "no progress" alone threw it away, and a layer whose stream comes back empty is
        // dropped by the caller. A whole decor layer disappeared from the panel that way.
        const bool stalled = strm.avail_in == inBefore && strm.avail_out == kChunkSize;
        if (ret != Z_STREAM_END && (ret == Z_BUF_ERROR || stalled)) {
            qWarning() << "decompressFlate: truncated stream —" << compressed.size()
                       << "bytes in," << result.size() << "out, inflate returned" << ret;
            inflateEnd(&strm);
            return {};
        }
    } while (ret != Z_STREAM_END);

    inflateEnd(&strm);
    return result;
}

QByteArray SvgToOcgPdf::emitMepTextAsPdfOps(const QDomElement &root, const PageInfo &page,
                                            AlphaStates &alpha) const
{
    QByteArray buf;

    // **The page the text was laid out on, and not only its height.** A baseline lands at
    // `pageHeightPt − y` and an origin at `x · scl`, which is right only while the document's
    // viewBox starts at the origin — which is the case for a plan, for the underlay, and for
    // nothing else. A cover page carries its own place in its viewBox, so BOTH axes are read off
    // the page here: one of them ignored moves a whole page of text aside, silently, and the file
    // looks perfectly ordinary from the outside.
    //
    // For a document whose viewBox does start at the origin — every caller but the cover page —
    // `k` is 1 and both origins are 0, so the arithmetic collapses to what it has always been.
    const double k = (page.viewBox.width() > 0.0)
                         ? page.sizeMm.width() * kPixelsPerMm / page.viewBox.width()
                         : 1.0;
    const double pageHeightPt = page.sizeMm.height() * kPointsPerMm;

    // Walk recursively and find all <text> elements
    std::function<void(const QDomElement &, const Affine2D &)> walk =
        [&](const QDomElement &el, const Affine2D &parentTx) {

        // Accumulate transform from <g> elements (not text — text
        // handles its own transform in the branch below).
        Affine2D currentTx = parentTx;
        const bool isText = el.tagName() == QStringLiteral("text");
        if (!isText && el.hasAttribute(QStringLiteral("transform"))) {
            const Affine2D elTx = parseSvgTransform(
                el.attribute(QStringLiteral("transform")));
            // Compose: parentTx then elTx (row-vector: p' = p × parentTx × elTx)
            currentTx = Affine2D{
                parentTx.a * elTx.a + parentTx.c * elTx.b,
                parentTx.b * elTx.a + parentTx.d * elTx.b,
                parentTx.a * elTx.c + parentTx.c * elTx.d,
                parentTx.b * elTx.c + parentTx.d * elTx.d,
                parentTx.a * elTx.e + parentTx.c * elTx.f + parentTx.e,
                parentTx.b * elTx.e + parentTx.d * elTx.f + parentTx.f
            };
        }

        if (isText) {
            // Get position from x/y attributes
            double px = el.attribute(QStringLiteral("x"), QStringLiteral("0")).toDouble();
            double py = el.attribute(QStringLiteral("y"), QStringLiteral("0")).toDouble();

            // Apply text's own transform if present
            Affine2D textTx = currentTx;
            if (el.hasAttribute(QStringLiteral("transform"))) {
                const Affine2D selfTx = parseSvgTransform(
                    el.attribute(QStringLiteral("transform")));
                // Compose: currentTx (parent chain) then selfTx
                // (row-vector: p' = p × currentTx × selfTx)
                textTx = Affine2D{
                    currentTx.a * selfTx.a + currentTx.c * selfTx.b,
                    currentTx.b * selfTx.a + currentTx.d * selfTx.b,
                    currentTx.a * selfTx.c + currentTx.c * selfTx.d,
                    currentTx.b * selfTx.c + currentTx.d * selfTx.d,
                    currentTx.a * selfTx.e + currentTx.c * selfTx.f + currentTx.e,
                    currentTx.b * selfTx.e + currentTx.d * selfTx.f + currentTx.f
                };
            }

            textTx.apply(px, py);

            // Extract text attributes
            const QString text = el.text().trimmed();
            if (text.isEmpty()) return;

            const QString fontFamily = el.attribute(QStringLiteral("font-family"),
                                                     QStringLiteral("Jost"));
            const double fontSize = el.attribute(QStringLiteral("font-size"),
                                                  QStringLiteral("12")).toDouble();
            const QString fontWeight = el.attribute(QStringLiteral("font-weight"),
                                                     QStringLiteral("400"));
            QString fillColor = el.attribute(QStringLiteral("fill"),
                                              QStringLiteral("black"));

            // Build QFont
            QFont font(fontFamily);
            font.setPixelSize(static_cast<int>(std::round(fontSize)));

            const QString fw = fontWeight.toLower();
            if (fw == QStringLiteral("bold") || fw == QStringLiteral("700")
                || fw == QStringLiteral("800") || fw == QStringLiteral("900")) {
                font.setBold(true);
            } else if (fw == QStringLiteral("thin") || fw == QStringLiteral("100")) {
                font.setWeight(QFont::Thin);
            } else if (fw == QStringLiteral("light") || fw == QStringLiteral("300")) {
                font.setWeight(QFont::Light);
            } else if (fw == QStringLiteral("medium") || fw == QStringLiteral("500")) {
                font.setWeight(QFont::Medium);
            } else {
                bool ok;
                const int num = fontWeight.toInt(&ok);
                if (ok) font.setWeight(static_cast<QFont::Weight>(num));
            }
            // **The face the document names, when it names one.** Ostara writes the style name of
            // the face it drew with — « SemiBold », « Book », whatever that family's designer
            // called it — because a weight NUMBER names no face: the same 300 is « Light » in one
            // family and « ExtraLight » in the next, and asking for 900 draws « ExtraBold ».
            // Measured on the fonts installed here. Carried by name, the page keeps the very face
            // the application used, whatever font the user has.
            //
            // A document that names none — every file written before this — keeps the weight it
            // declares, and the style name is cleared so that Qt does not resolve against a name
            // the drawing never carried.
            font.setStyleName(el.attribute(QStringLiteral("ostara:font-face")));

            // Generate text outline path
            QPainterPath textPath;
            textPath.addText(0.0, 0.0, font, text);
            if (textPath.isEmpty()) return;

            // Parse fill color
            double fr = 0, fg = 0, fb = 0;
            parseColor(fillColor, fr, fg, fb);

            // Compute rotation from transform matrix
            const double netRot = -std::atan2(textTx.b, textTx.a) * 180.0 / M_PI;
            const double rad = netRot * M_PI / 180.0;
            const double c = std::cos(rad), sn = std::sin(rad);

            // PDF cm matrix
            const double scl = kPixelToPt;
            const double a  = scl * c;
            const double b_coord = scl * sn;
            const double cc = scl * sn;
            const double d  = -scl * c;
            // The user point brought to points through the page's own mapping, viewBox origin
            // included — the same mapping the renderer used for the shapes beside this text.
            const double xPt = (px - page.viewBox.left()) * k * scl;
            const double yPt =
                scl * (page.sizeMm.height() * kPixelsPerMm - (py - page.viewBox.top()) * k);

            buf.append("q\n");
            buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 cm\n")
                           .arg(a, 0, 'f', 6).arg(b_coord, 0, 'f', 6)
                           .arg(cc, 0, 'f', 6).arg(d, 0, 'f', 6)
                           .arg(xPt, 0, 'f', 6).arg(yPt, 0, 'f', 6).toUtf8());

            // The strength this text is filled at, when it is less than full. `fill-opacity` is
            // where the SVG carries it — a pen colour with an alpha becomes that attribute and
            // nothing else — and a PDF has no alpha in its colour operators, so it can only be a
            // graphics state. Inside the q/Q of this text, so that it does not outlive it.
            //
            // Absent means full strength, which is the common case and writes nothing at all:
            // a page of opaque numbers must come out exactly as it did before there were states.
            const double fillOpacity =
                el.attribute(QStringLiteral("fill-opacity"), QStringLiteral("1")).toDouble();
            if (fillOpacity < 1.0)
            {
                buf.append(QStringLiteral("/%1 gs\n")
                               .arg(AlphaNameFor(alpha, fillOpacity)).toUtf8());
            }

            buf.append(QStringLiteral("%1 %2 %3 rg\n")
                           .arg(fr, 0, 'f', 4).arg(fg, 0, 'f', 4).arg(fb, 0, 'f', 4)
                           .toUtf8());

            // Emit path elements
            for (int i = 0; i < textPath.elementCount(); ++i) {
                const QPainterPath::Element e = textPath.elementAt(i);
                switch (e.type) {
                case QPainterPath::MoveToElement:
                    buf.append(QStringLiteral("%1 %2 m\n")
                                   .arg(e.x, 0, 'f', 3).arg(e.y, 0, 'f', 3).toUtf8());
                    break;
                case QPainterPath::LineToElement:
                    buf.append(QStringLiteral("%1 %2 l\n")
                                   .arg(e.x, 0, 'f', 3).arg(e.y, 0, 'f', 3).toUtf8());
                    break;
                case QPainterPath::CurveToElement: {
                    const QPainterPath::Element cp2 = textPath.elementAt(i + 1);
                    const QPainterPath::Element endPt = textPath.elementAt(i + 2);
                    buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 c\n")
                                   .arg(e.x, 0, 'f', 3).arg(e.y, 0, 'f', 3)
                                   .arg(cp2.x, 0, 'f', 3).arg(cp2.y, 0, 'f', 3)
                                   .arg(endPt.x, 0, 'f', 3).arg(endPt.y, 0, 'f', 3)
                                   .toUtf8());
                    i += 2;
                    break;
                }
                default: break;
                }
            }
            buf.append("f\n");
            buf.append("Q\n");
        } else {
            // Recurse into child elements
            for (QDomElement child = el.firstChildElement(); !child.isNull();
                 child = child.nextSiblingElement()) {
                walk(child, currentTx);
            }
        }
    };

    walk(root, Affine2D{});
    return buf;
}

namespace
{
/// @brief The /Info dictionary: what the document says about itself.
///
/// A reader shows these in its information panel, and shows an empty panel when there are
/// none — which is what the hand-built envelope did before this. Qt's own writer fills them
/// in for free; ours has to be told.
auto BuildInfoObject(const SvgToOcgPdf::DocumentInfo &info) -> QByteArray
{
    const QString producer = info.producer.isEmpty()
                                 ? QStringLiteral("Trame %1").arg(QString::fromLatin1(TRAME_VERSION))
                                 : info.producer;

    // The form the PDF spec asks for: D:YYYYMMDDHHmmSSOHH'mm'. Local time, offset included,
    // so a reader can show it as the clock read when the file was written.
    const QDateTime now = QDateTime::currentDateTime();
    const int offsetSeconds = now.offsetFromUtc();
    const int absOffset = qAbs(offsetSeconds);
    const QString stamp = QStringLiteral("D:%1%2%3'%4'")
                              .arg(now.toString(QStringLiteral("yyyyMMddHHmmss")))
                              .arg(offsetSeconds < 0 ? u'-' : u'+')
                              .arg(absOffset / 3600, 2, 10, QLatin1Char('0'))
                              .arg((absOffset % 3600) / 60, 2, 10, QLatin1Char('0'));

    QByteArray body = "<<";
    if (!info.title.isEmpty())   body += " /Title "   + pdfTextString(info.title);
    if (!info.author.isEmpty())  body += " /Author "  + pdfTextString(info.author);
    if (!info.creator.isEmpty()) body += " /Creator " + pdfTextString(info.creator);
    body += " /Producer " + pdfTextString(producer);
    body += " /CreationDate " + pdfTextString(stamp);
    body += " >>\n";
    return body;
}

/// @brief The six numbers of the cm QPdfWriter puts at the head of every stream it renders.
///
/// Each stream opens with `/GSa gs /CSp cs /CSp CS` and then `0.75 0 0 -0.75 0 <h> cm`,
/// which carries the three facts that place a page over the plan: the pixel-to-point
/// scale, the Y flip, and the height the flip turns about. All three are READ off the
/// stream rather than assumed, for two reasons, and both of them are about an answer that
/// would look right and be wrong:
///
///  - the flip constant is Qt's page height in points, ROUNDED TO A WHOLE POINT. It is not
///    recoverable by arithmetic from anything the caller knows — only from the stream.
///  - the scale is 72/resolution only while Qt reports no /UserUnit. QPdfEngine starts
///    dividing by userUnit as soon as the PDF version reaches 1.6 AND a page's long side
///    passes 14400 pt (5080 mm), which an A0 frame five rows deep already does. This module
///    asks for PdfVersion_1_4, so that branch is dormant today — but a hard-coded 0.75
///    would place a silently SHRUNKEN plan the day somebody raises the version to match the
///    %PDF-1.7 this file already declares.
///
/// Reading the cm costs a scan of the first few tokens and is right in both worlds.
struct Placement
{
    double a{kPixelToPt};   ///< x scale, pixels → points
    double b{0.0};
    double c{0.0};
    double d{-kPixelToPt};  ///< y scale: negative, which is the flip
    double e{0.0};
    double f{0.0};          ///< the height the flip turns about, in points
};

/// @brief Read the first cm of @p stream into @p out. False when there is none to read.
auto ReadPlacement(const QByteArray &stream, Placement &out) -> bool
{
    QVector<double> numbers;
    int             i = 0;
    const int       n = stream.size();

    while (i < n) {
        const char ch = stream.at(i);
        if (ch == '%') {  // a comment runs to the end of its line
            while (i < n && stream.at(i) != '\n')
                ++i;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            ++i;
            continue;
        }

        const int start = i;
        while (i < n && !std::isspace(static_cast<unsigned char>(stream.at(i))))
            ++i;

        const QByteArray token = stream.mid(start, i - start);

        bool         isNumber = false;
        const double value    = token.toDouble(&isNumber);
        if (isNumber) {
            numbers.append(value);
            continue;
        }

        if (token == "cm" && numbers.size() >= 6) {
            const int at = numbers.size() - 6;
            out.a        = numbers.at(at);
            out.b        = numbers.at(at + 1);
            out.c        = numbers.at(at + 2);
            out.d        = numbers.at(at + 3);
            out.e        = numbers.at(at + 4);
            out.f        = numbers.at(at + 5);
            return true;
        }

        // Any other operator ends the run: the operands of a cm are adjacent to it.
        numbers.clear();
    }

    return false;
}
}  // namespace

bool SvgToOcgPdf::buildOcgPdf(const QString         &pdfPath,
                               const SvgContent      &doc,
                               const QVector<QRectF> &pages,
                               const DocumentInfo    &info,
                               bool                   mepIsLayer,
                               const QRectF          &coverSheet)
{
    if (pages.isEmpty()) {
        qWarning() << "SvgToOcgPdf: nothing to write";
        return false;
    }

    PdfBuilder pdf;
    pdf.header = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";

    // Allocate and add in one step, always. The xref is written positionally, one entry
    // per object, so a number that is allocated and never added — or added twice — shifts
    // every offset after it and produces a file that opens to garbage. There is nothing in
    // PdfBuilder that would say so.
    const auto addObject = [&pdf](const QByteArray &body) {
        const int num = pdf.alloc();
        pdf.add(num, body);
        return num;
    };

    // Both of these are added at the end, once every object they point at exists. What
    // matters is that they are ALLOCATED first: PdfBuilder takes the trailer's /Root to be
    // the lowest-numbered object in the file, and the catalog has to be it.
    const int catalogObj = pdf.alloc();
    const int pagesObj   = pdf.alloc();

    // The information dictionary, which the trailer points at. Without it a reader has
    // nothing to show — no title, no author, no producer.
    const int infoObj = pdf.alloc();
    pdf.infoObj = infoObj;
    pdf.add(infoObj, BuildInfoObject(info));

    // The layer set belongs to the DOCUMENT, and there is now only one document: every
    // page draws the same layers and differs only in which part of the plan it shows. One
    // OCG object per layer, referenced by every page, keeps the reader's panel to one
    // entry per layer rather than one per layer per sheet.
    const bool plain = doc.plain;
    const bool withOcg = !plain;

    // Separate the size layers from the decor, and tell a layered document from a plain
    // one: optional content only means something when there is something to switch.
    QVector<const LayerContent *> sizeLayers;
    const LayerContent *mepLayer = nullptr;
    for (const auto &lc : doc.layers) {
        if (lc.plain)
            continue;
        if (lc.isMep)
            mepLayer = &lc;
        else
            sizeLayers.append(&lc);
    }

    // Painting order, bottom first — the sizes, then the decor over them. It is the order
    // the forms are written in, and the order every page invokes them in.
    QVector<const LayerContent *> drawing;
    if (plain) {
        if (!doc.layers.isEmpty())
            drawing.append(&doc.layers.first());
    } else {
        drawing = sizeLayers;
        if (mepLayer != nullptr)
            drawing.append(mepLayer);
    }

    // OCG objects — one per size. The name is the one the user typed; the id is what
    // the PDF addresses the layer by, and it is the id that goes in /Properties.
    QVector<int> ocgObjs;
    if (withOcg) {
        ocgObjs.reserve(sizeLayers.size());
        for (const auto *sz : sizeLayers) {
            QByteArray body = "<< /Type /OCG /Name " + pdfTextString(sz->displayName) + " >>\n";
            const int objNum = pdf.alloc();
            ocgObjs.append(objNum);
            pdf.add(objNum, body);
        }
    }

    // Whether the decor is one of the reader's layers at all. Its content is drawn either way —
    // what this decides is whether the reader is offered a way to take it off the page, and with
    // it the underlay, which belongs to the paper the decor is printed on.
    //
    // One name short throughout: allocate no object, name it in no /Properties, list it in
    // neither /OCGs nor /Order, and mark its invocation with nothing.
    const bool mepOcg = withOcg && mepIsLayer && mepLayer != nullptr;

    // Decor OCG (always ON when it is one)
    int mepOcgObj = 0;
    if (mepOcg) {
        mepOcgObj = pdf.alloc();
        pdf.add(mepOcgObj,
                "<< /Type /OCG /Name " + pdfTextString(mepLayer->displayName) + " >>\n");
    }

    // ExtGState object referenced by QPdfWriter content streams as /GSa gs
    const int gsaObj = pdf.alloc();
    pdf.add(gsaObj,
            QStringLiteral("<< /Type /ExtGState /SA true /SM 0.02 /ca 1.0 /CA 1.0"
                           " /AIS false /SMask /None >>\n").toUtf8());

    // The states the content actually reaches for, allocated here and nowhere else. This is the
    // first point at which the output file exists — which is the whole reason the streams carried
    // their names up out of readSvg.
    //
    // Qt expressed an alpha as a graphics state and had the stream call it by name. Extracting
    // only the stream left the state behind in the rendered document, so every one of those names
    // resolved to nothing: a reader ignores the instruction, the drawing keeps the strength it
    // had, and a frame that prints solid looks like a frame.
    //
    // /GSa is kept whatever happens. It is the state each stream OPENS with, so a file whose only
    // entry was a partial one would leave the rest of the drawing at whatever it inherited.
    QByteArray extGStates;
    extGStates.append(QStringLiteral("/GSa %1 0 R").arg(gsaObj).toUtf8());
    for (const AlphaState &state : doc.alphaStates) {
        // Allocated and added in one step, as everywhere in this function: the xref is written
        // positionally, so a number taken and never written shifts every offset after it.
        const int stateObj = addObject(state.body);
        extGStates.append(QStringLiteral(" /%1 %2 0 R").arg(state.name).arg(stateObj).toUtf8());
    }

    // The Properties dictionary binds each layer NAME — what the content streams address —
    // to its OCG object. It is the same on every page, because the OCG objects are the
    // document's; only the streams differ, and the names are what point into them.
    QByteArray propsDict;
    if (mepOcg) {
        propsDict.append(QStringLiteral("      /%1 %2 0 R\n")
                             .arg(mepLayer->name)
                             .arg(mepOcgObj).toUtf8());
    }
    if (withOcg) {
        for (int i = 0; i < sizeLayers.size(); ++i) {
            propsDict.append(QStringLiteral("      /%1 %2 0 R\n")
                                 .arg(sizeLayers[i]->name)
                                 .arg(ocgObjs[i]).toUtf8());
        }
    }

    // The plan: all the pages together. Derived from the rectangles rather than handed
    // over beside them, because a third figure to keep in agreement with the other two is
    // a third figure that can disagree. It serves one thing only — sizing the /BBox — and
    // takes no part in the page arithmetic.
    // The sheets this file is made of: the cover page's own, when its format gives it one, and
    // then the plan's. It is FIRST — a cover page opens what it covers — and the plan's own figure
    // is taken over the two of them, because the /BBox has to hold whatever any page shows.
    QVector<QRectF> sheets = pages;
    if (!coverSheet.isEmpty())
        sheets.prepend(coverSheet);

    QRectF plan;
    for (const QRectF &page : sheets)
        plan = plan.united(page);

    // Where Qt placed the drawing inside a rendered stream, read off the streams instead of
    // assumed. They are all written the same way; a disagreement means an assumption has
    // moved under us, and the last thing to do is place pages from a value that is only
    // sometimes right.
    Placement placement;
    bool havePlacement = false;
    for (const LayerContent *layer : drawing) {
        if (layer->stream.isEmpty())
            continue;
        Placement found;
        if (!ReadPlacement(layer->stream, found)) {
            qWarning() << "SvgToOcgPdf: no placement cm at the head of a rendered stream";
            break;
        }
        if (!havePlacement) {
            placement = found;
            havePlacement = true;
            continue;
        }
        const auto differs = [](double x, double y) { return std::abs(x - y) > 1e-6; };
        if (differs(found.a, placement.a) || differs(found.d, placement.d)
            || differs(found.f, placement.f)) {
            qWarning() << "SvgToOcgPdf: layers disagree on their placement:"
                       << found.a << found.d << found.f << "against"
                       << placement.a << placement.d << placement.f;
        }
    }

    if (!havePlacement) {
        // What this module used to hard-code. A sub-point offset is a far better outcome
        // than a failed export, so the fallback stays — the warning is what keeps it from
        // being a silent one.
        qWarning() << "SvgToOcgPdf: no placement to read; falling back to" << kPixelToPt
                   << "px→pt and a flip about" << kPixelToPt * plan.bottom();
        placement = Placement{};
        placement.f = kPixelToPt * plan.bottom();
    }

    // The underlay is read on every page through the very translation the plan is read
    // through, so it has to carry the plan's own placement. Read rather than assumed: a
    // frame half a point off its sheets is a frame that does not close, and it would close
    // no better for having been trusted.
    if (!doc.underlay.stream.isEmpty()) {
        Placement found;
        if (!ReadPlacement(doc.underlay.stream, found)) {
            qWarning() << "SvgToOcgPdf: no placement cm at the head of the underlay stream";
        } else {
            const auto differs = [](double x, double y) { return std::abs(x - y) > 1e-6; };
            if (differs(found.a, placement.a) || differs(found.d, placement.d)
                || differs(found.f, placement.f)) {
                qWarning() << "SvgToOcgPdf: the underlay is placed unlike the plan:"
                           << found.a << found.d << found.f << "against"
                           << placement.a << placement.d << placement.f;
            }
        }
    }

    // ── The cover page, and the TWO cms Qt writes ───────────────────────────────────────────
    //
    // A stream Qt renders opens with two `cm`, and they say different things. The first is the
    // engine's page: `0.75 0 0 -0.75 0 <height>`, which is what ReadPlacement names a placement.
    // The second is the painter's mapping of the document's viewBox onto that page:
    // `<k> 0 0 <k> <-k·vbX> <-k·vbY>`.
    //
    // **The two coincide only while the viewBox starts at the origin** — which is the case for a
    // plan, for the underlay, and for nothing else. A cover page is laid out where its format puts
    // it, so its viewBox carries the place, and reading the first cm alone places its drawing one
    // page away from its sheet. Silently: blank paper is what a missing drawing looks like too.
    //
    // Measured, not reasoned — a probe on a document whose viewBox starts at (100.5, 200.25) gives
    // `0.750000000 0 0 -0.750000000 0 734 cm`, then
    // `1.00000032 0 0 0.999997586 -100.500033 -200.249516 cm`. The second is the painter's, and it
    // is composed here from the page the caller declared — the module rebuilds that root itself, so
    // the two agree by construction.
    //
    // What comes out of it is the cm that brings the WHOLE of that mapping into the plan's own
    // frame: the cover's placement undone, the plan's put back. Read rather than assumed for the
    // engine's half, for the reasons ReadPlacement gives; composed rather than guessed for the
    // painter's, from figures the document states about itself.
    Placement coverCorrection;
    bool haveCoverCorrection = false;
    if (!doc.cover.isEmpty()) {
        const QRectF vb = doc.cover.page.viewBox;
        // The painter's scale: the page as the document declares it, over the viewBox it maps.
        const double k = (vb.width() > 0.0)
                             ? doc.cover.page.sizeMm.width() * kPixelsPerMm / vb.width()
                             : 1.0;

        Placement engine;
        if (!ReadPlacement(doc.cover.stream, engine)) {
            // The case the plan's own reading falls back for, answered the same way: the page the
            // document was laid out on says what Qt placed its drawing by.
            qWarning() << "SvgToOcgPdf: no placement cm at the head of the cover page stream";
            engine.a = kPixelToPt;
            engine.d = -kPixelToPt;
            engine.e = 0.0;
            engine.f = kPixelToPt * doc.cover.page.sizeMm.height() * kPixelsPerMm;
        }

        // The painter's mapping first, the engine's after: the whole of where a user point of the
        // cover document lands, in points.
        Placement whole;
        whole.a = engine.a * k;
        whole.d = engine.d * k;
        whole.e = engine.e - engine.a * k * vb.left();
        whole.f = engine.f - engine.d * k * vb.top();

        if (whole.a == 0.0 || whole.d == 0.0) {
            // No scale to undo: no correction would put the drawing anywhere, and a cover page
            // dropped in the wrong place is worse than one that is missing and says so.
            qWarning() << "SvgToOcgPdf: the cover page has no scale to undo; it is not drawn";
        } else {
            coverCorrection.a = placement.a / whole.a;
            coverCorrection.d = placement.d / whole.d;
            coverCorrection.e = placement.e - placement.a * whole.e / whole.a;
            coverCorrection.f = placement.f - placement.d * whole.f / whole.d;
            haveCoverCorrection = true;
        }
    }

    if (placement.b != 0.0 || placement.c != 0.0) {
        // Nothing Qt has produced has a shear here, and the page arithmetic below does not
        // carry one. Say so rather than place pages as if it were zero.
        qWarning() << "SvgToOcgPdf: the placement is sheared (" << placement.b
                   << placement.c << "); pages are placed as if it were not";
    }

    if (placement.a * plan.width() <= 0.0 || -placement.d * plan.height() <= 0.0) {
        qWarning() << "SvgToOcgPdf: the placement gives the plan no surface";
        return false;
    }

    // The form's /BBox clips, so it has to hold every page's /MediaBox mapped into form
    // space — and that is the whole plan. A point of the plan (sx, sy) sits there at
    // (a·sx + e, d·sy + f), and d is negative: that is the Y flip. A point of bleed each
    // way, because a clip is the one thing that cannot recover from being a hair short,
    // and the page's own /MediaBox is what decides what actually shows.
    constexpr double kBleedPt = 1.0;
    const QByteArray bbox =
        QStringLiteral("%1 %2 %3 %4")
            .arg(placement.a * plan.left() + placement.e - kBleedPt, 0, 'f', 6)
            .arg(placement.d * plan.bottom() + placement.f - kBleedPt, 0, 'f', 6)
            .arg(placement.a * plan.right() + placement.e + kBleedPt, 0, 'f', 6)
            .arg(placement.d * plan.top() + placement.f + kBleedPt, 0, 'f', 6)
            .toUtf8();

    // One Form XObject per drawing, written once and invoked by every page: the plan is in
    // the file a single time. It is also RENDERED a single time, and that is the other half
    // of the win — a document read once is M renders, where a page read its own copy was
    // N×M of them.
    const auto makeForm = [&](const QByteArray &content) {
        return addObject(
            QStringLiteral(
                "<< /Type /XObject /Subtype /Form\n"
                "   /BBox [%1]\n"
                "   /Resources <<\n"
                "      /ExtGState << %2 >>\n"
                "      /ColorSpace << /CSp /DeviceRGB >>\n"
                "   >>\n"
                "   /Length %3 >>\n"
                "stream\n"
            ).arg(QString::fromUtf8(bbox)).arg(QString::fromUtf8(extGStates))
             .arg(content.size()).toUtf8()
            + content
            + "\nendstream");
    };

    QVector<int> formObjs;
    formObjs.reserve(drawing.size() + 1);

    for (const LayerContent *layer : drawing) {
        // The layout layer arrives as two streams — the shapes QSvgRenderer drew, and the
        // text this module re-emits as path operators — because they are converted by
        // different hands and must not share a CTM. One form holds both, each in its own
        // q/Q.
        QByteArray content;
        if (layer->isMep) {
            content.append("q\n");
            content.append(layer->stream);
            content.append("\nQ\n");
            if (!layer->textStream.isEmpty()) {
                content.append("q\n");
                content.append(layer->textStream);
                content.append("\nQ\n");
            }
        } else {
            content.append(layer->stream);
        }

        formObjs.append(makeForm(content));
    }

    // The underlay comes last among the forms and first in every page. It is written and
    // rendered exactly once, like the layers; what sets it apart is where the page invokes
    // it — beneath them, and outside every /OC block, so that no layer state can take it
    // off the paper. Its index is therefore one past the last layer, which is the name the
    // page stream below addresses it by.
    int underlayFormIndex = -1;
    if (!doc.underlay.isEmpty()) {
        QByteArray content;
        content.append("q\n");
        content.append(doc.underlay.stream);
        content.append("\nQ\n");
        if (!doc.underlay.textStream.isEmpty()) {
            content.append("q\n");
            content.append(doc.underlay.textStream);
            content.append("\nQ\n");
        }

        underlayFormIndex = formObjs.size();
        formObjs.append(makeForm(content));
    }

    // The cover page's document, written like the underlay's and invoked like it on every page —
    // one form, rendered once. What sets it apart is not where it stands on the page but where its
    // own placement puts it; the page stream corrects that, below.
    int coverFormIndex = -1;
    if (!doc.cover.isEmpty()) {
        QByteArray content;
        content.append("q\n");
        content.append(doc.cover.stream);
        content.append("\nQ\n");
        if (!doc.cover.textStream.isEmpty()) {
            content.append("q\n");
            content.append(doc.cover.textStream);
            content.append("\nQ\n");
        }

        coverFormIndex = formObjs.size();
        formObjs.append(makeForm(content));
    }

    QVector<int> pageObjs;
    pageObjs.reserve(sheets.size());

    // One page per rectangle, in the order they were handed over — the order the sheets are
    // to be printed and taped back together in.
    for (int pageIndex = 0; pageIndex < sheets.size(); ++pageIndex) {
        const QRectF &page = sheets.at(pageIndex);

        // **The grafted sheet carries the cover page and nothing else.** It is not a sheet of the
        // plan, and the plan's own frame runs along its outer edge: a line drawn on that boundary
        // has half its width outside the plan, and a sheet placed against it prints that half.
        // Neither the underlay nor the layers is invoked here — a cover page's sheet holds the
        // cover page, which is the whole of what grafting a page means.
        const bool coverOnly = !coverSheet.isEmpty() && pageIndex == 0;
        const double pageWidthPt  = placement.a * page.width();
        const double pageHeightPt = -placement.d * page.height();

        // A sheet shows one rectangle of the plan, at the plan's own scale. Undoing the
        // placement for that rectangle's corner is the whole of what makes it a sheet: the
        // sheet wants x_page = a·(sx − rx) and y_page = −d·(ry + rh − sy), and the form
        // draws sx at a·sx + e and sy at d·sy + f.
        const double dx = -placement.a * page.x() - placement.e;
        const double dy = -placement.d * (page.y() + page.height()) - placement.f;

        QByteArray streamData;
        streamData.append("q\n");
        streamData.append(QStringLiteral("1 0 0 1 %1 %2 cm\n")
                              .arg(dx, 0, 'f', 6)
                              .arg(dy, 0, 'f', 6)
                              .toUtf8());

        // The underlay first, and under the drawing rather than over it. It carries the decor's
        // marker when the decor is a layer of its own — the frame, the numbering and the marks
        // are drawn on the paper, so they belong to the page's own layer. With no decor layer
        // there is nothing to mark it with and it stands alone, which is the safe way round: a
        // mark that can be switched off is a sheet that can be assembled wrongly.
        //
        // ⚠️ Marked, never MOVED. It stays first in this stream and therefore stays under
        // everything. A PDF layer is a marker around a range rather than a container, so the
        // marker can travel to it here — but putting its `Do` inside the decor's own block
        // would lift the frame and the numbering over the pieces.
        if (underlayFormIndex >= 0 && !coverOnly) {
            if (mepOcg)
                streamData.append(QStringLiteral("/OC /%1 BDC\n").arg(mepLayer->name).toUtf8());
            streamData.append(
                QStringLiteral("q\n/Fm%1 Do\nQ\n").arg(underlayFormIndex).toUtf8());
            if (mepOcg)
                streamData.append("EMC\n");
        }

        // The cover page: over the paper's own marks, under every layer, and — like the underlay —
        // OUTSIDE every /OC block. Nothing in the reader's panel reaches it, whatever the layout
        // layer setting says, and that is the point: a plan delivered without its nomenclature
        // looks perfectly normal, which is why nobody should be able to make one by accident.
        if (haveCoverCorrection) {
            streamData.append(QStringLiteral("q\n%1 0 0 %2 %3 %4 cm\n/Fm%5 Do\nQ\n")
                                  .arg(coverCorrection.a, 0, 'f', 6)
                                  .arg(coverCorrection.d, 0, 'f', 6)
                                  .arg(coverCorrection.e, 0, 'f', 6)
                                  .arg(coverCorrection.f, 0, 'f', 6)
                                  .arg(coverFormIndex)
                                  .toUtf8());
        }

        for (int i = 0; !coverOnly && i < drawing.size(); ++i) {
            const LayerContent *layer = drawing.at(i);
            // The layer markers stay HERE, in the page's own stream, resolving through the
            // /Properties of the page — which is where they have always been. Only the
            // drawing moved into the form, and this is what keeps the layer logic still.
            //
            // A decor that is not a layer is named by nothing: marking it would point at a
            // name the page's /Properties no longer carries, and the reader would take the
            // whole decoration for a layer it can switch off.
            const bool marked = withOcg && (!layer->isMep || mepIsLayer);
            if (marked)
                streamData.append(QStringLiteral("/OC /%1 BDC\n").arg(layer->name).toUtf8());
            streamData.append(QStringLiteral("q\n/Fm%1 Do\nQ\n").arg(i).toUtf8());
            if (marked)
                streamData.append("EMC\n");
        }
        streamData.append("Q\n");

        const int contentObj = addObject(
            QStringLiteral("<< /Length %1 >>\nstream\n").arg(streamData.size()).toUtf8()
            + streamData
            + "\nendstream");

        QByteArray xobjects;
        for (int i = 0; i < formObjs.size(); ++i)
            xobjects.append(QStringLiteral("      /Fm%1 %2 0 R\n")
                                .arg(i)
                                .arg(formObjs.at(i))
                                .toUtf8());

        const int pageObj = addObject(
            QStringLiteral(
                "<< /Type /Page /Parent %1 0 R\n"
                "   /MediaBox [0 0 %2 %3]\n"
                "   /Contents %4 0 R\n"
                "   /Resources <<\n"
                "      /ExtGState << %5 >>\n"
                "      /ColorSpace << /CSp /DeviceRGB >>\n"
                "      /XObject <<\n"
                "%6"
                "      >>\n"
                "      /Properties <<\n"
                "%7"
                "      >>\n"
                "   >>\n"
                "   /Group << /Type /Group /S /Transparency >> >>\n"
            ).arg(pagesObj)
             .arg(pageWidthPt, 0, 'f', 6)
             .arg(pageHeightPt, 0, 'f', 6)
             .arg(contentObj)
             .arg(QString::fromUtf8(extGStates))
             .arg(QString::fromUtf8(xobjects))
             .arg(QString::fromUtf8(propsDict))
             .toUtf8());

        pageObjs.append(pageObj);
    }

    // Pages — every sheet, and as many of them as there are.
    QByteArray kids;
    for (const int obj : pageObjs)
        kids.append(QByteArray::number(obj) + " 0 R ");

    pdf.add(pagesObj,
            QStringLiteral("<< /Type /Pages /Kids [%1] /Count %2 >>\n")
                .arg(QString::fromUtf8(kids).trimmed())
                .arg(pageObjs.size()).toUtf8());

    // Catalog with OCG
    QByteArray ocgRefs;
    QByteArray orderRefs;

    if (mepOcg) {
        const QByteArray ref = QByteArray::number(mepOcgObj) + " 0 R ";
        ocgRefs.append(ref);
        orderRefs.append(ref);
    }
    // A reader lists the layers topmost first, and /Order is that list — so it runs
    // opposite to the painting order, in which the smallest size is drawn last and
    // therefore sits on top. Walked backwards here so the panel reads small to large,
    // right under the decor, which is painted last of all and so comes first.
    for (int i = ocgObjs.size() - 1; i >= 0; --i) {
        const QByteArray ref = QByteArray::number(ocgObjs[i]) + " 0 R ";
        ocgRefs.append(ref);
        orderRefs.append(ref);
    }
    // All layers visible by default
    const QByteArray offRefs("[]");

    if (withOcg) {
        pdf.add(catalogObj,
                QStringLiteral(
                    "<< /Type /Catalog /Pages %1 0 R\n"
                    "   /OCProperties <<\n"
                    "     /OCGs [%2]\n"
                    "     /D <<\n"
                    "       /Order [%3]\n"
                    "       /OFF [%4]\n"
                    "       /AS [<< /Event /View /Category /Print /OCGs [%5] >>]\n"
                    "     >>\n"
                    "   >>\n"
                    ">>\n"
                ).arg(pagesObj)
                 .arg(QString::fromUtf8(ocgRefs).trimmed())
                 .arg(QString::fromUtf8(orderRefs).trimmed())
                 .arg(QString::fromUtf8(offRefs).trimmed())
                 .arg(QString::fromUtf8(ocgRefs).trimmed())
                 .toUtf8());
    } else {
        // A flat PDF says nothing about optional content, because it has none.
        pdf.add(catalogObj,
                QStringLiteral("<< /Type /Catalog /Pages %1 0 R >>\n")
                    .arg(pagesObj).toUtf8());
    }

    // Write file
    int xrefOffset = 0;
    const QByteArray fileContent = pdf.build(xrefOffset);

    QFile file(pdfPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "SvgToOcgPdf: cannot open" << pdfPath;
        return false;
    }
    file.write(fileContent);
    file.close();

    qDebug() << "PDF exported:" << pdfPath;
    qDebug() << "  Pages:" << pageObjs.size() << "of"
             << QStringLiteral("%1 × %2 pt")
                    .arg(placement.a * pages.first().width(), 0, 'f', 2)
                    .arg(-placement.d * pages.first().height(), 0, 'f', 2);
    qDebug() << "  Forms:" << formObjs.size() << "— the plan is written and rendered once";
    if (underlayFormIndex >= 0) {
        qDebug() << "  Underlay: on every page, under every layer,"
                 << (mepOcg ? "marked with the decor layer" : "in no layer group");
    }
    if (withOcg) {
        qDebug() << "  OCG layers:" << sizeLayers.size()
                   << "+ decor:" << (mepOcg ? 1 : 0);
    } else {
        qDebug() << "  Flat document: no optional content";
    }

    return true;
}
