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

bool SvgToOcgPdf::convert(const QString &svgPath, const QString &pdfPath, const DocumentInfo &info)
{
    return convert(QStringList{svgPath}, pdfPath, info);
}

bool SvgToOcgPdf::convert(const QStringList &svgPaths, const QString &pdfPath,
                          const DocumentInfo &info)
{
    if (svgPaths.isEmpty()) {
        qWarning() << "SvgToOcgPdf: no input document";
        return false;
    }

    QVector<SvgContent> pages;
    pages.reserve(svgPaths.size());

    for (const QString &svgPath : svgPaths) {
        SvgContent sheet = readSvg(svgPath);
        if (!sheet.ok) {
            // One sheet short and the whole delivery is wrong: every page after it would
            // be a sheet earlier than it should be, and the reader would tape the plan up
            // in the wrong order. Handing over nothing beats handing over a plan that
            // cannot be reassembled.
            return false;
        }
        pages.append(sheet);
    }

    return buildOcgPdf(pdfPath, pages, info);
}

SvgToOcgPdf::SvgContent SvgToOcgPdf::readSvg(const QString &svgPath)
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

        QByteArray textStream;
        // For MEP layer, generate text as PDF path operators
        // (kept separate — must not be affected by QSvgRenderer's CTM)
        if (layer.isMep) {
            const double pageHeightPt = result.page.sizeMm.height() * kPointsPerMm;
            textStream = emitMepTextAsPdfOps(layer.element, pageHeightPt);
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

QByteArray SvgToOcgPdf::buildLayerSvg(const QDomDocument &origDoc,
                                       const QDomElement   &layerEl,
                                       const PageInfo      &page) const
{
    QDomDocument layerDoc;

    // Processing instruction
    QDomNode pi = layerDoc.createProcessingInstruction(
        QStringLiteral("xml"), QStringLiteral("version=\"1.0\" encoding=\"UTF-8\""));
    layerDoc.appendChild(pi);

    QDomElement svgRoot = layerDoc.createElement(QStringLiteral("svg"));
    svgRoot.setAttribute(QStringLiteral("xmlns"), QStringLiteral("http://www.w3.org/2000/svg"));
    svgRoot.setAttribute(QStringLiteral("viewBox"),
                         QStringLiteral("%1 %2 %3 %4")
                             .arg(page.viewBox.x(), 0, 'f', 2)
                             .arg(page.viewBox.y(), 0, 'f', 2)
                             .arg(page.viewBox.width(), 0, 'f', 2)
                             .arg(page.viewBox.height(), 0, 'f', 2));
    svgRoot.setAttribute(QStringLiteral("width"),
                         QStringLiteral("%1mm").arg(page.sizeMm.width(), 0, 'f', 4));
    svgRoot.setAttribute(QStringLiteral("height"),
                         QStringLiteral("%1mm").arg(page.sizeMm.height(), 0, 'f', 4));

    // Deep-clone the layer element, stripping <text> elements if MEP
    QDomNode cloned = layerDoc.importNode(layerEl, true);

    // Remove <text> elements from the clone (they'll be rendered as paths)
    auto removeTextElements = [](QDomElement root) {
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
    };
    removeTextElements(cloned.toElement());

    svgRoot.appendChild(cloned);
    layerDoc.appendChild(svgRoot);

    return layerDoc.toByteArray(-1); // -1 = no indent, compact XML
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

QByteArray SvgToOcgPdf::emitMepTextAsPdfOps(const QDomElement &root,
                                               double           pageHeightPt) const
{
    QByteArray buf;

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
            font.setStyleName(QString());

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
            const double xPt = px * scl;
            const double yPt = pageHeightPt - py * scl;

            buf.append("q\n");
            buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 cm\n")
                           .arg(a, 0, 'f', 6).arg(b_coord, 0, 'f', 6)
                           .arg(cc, 0, 'f', 6).arg(d, 0, 'f', 6)
                           .arg(xPt, 0, 'f', 6).arg(yPt, 0, 'f', 6).toUtf8());
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
}  // namespace

bool SvgToOcgPdf::buildOcgPdf(const QString             &pdfPath,
                               const QVector<SvgContent> &pages,
                               const DocumentInfo        &info)
{
    if (pages.isEmpty()) {
        qWarning() << "SvgToOcgPdf: nothing to write";
        return false;
    }

    PdfBuilder pdf;
    pdf.header = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";

    const int catalogObj = pdf.alloc();
    const int pagesObj   = pdf.alloc();

    // The information dictionary, which the trailer points at. Without it a reader has
    // nothing to show — no title, no author, no producer.
    const int infoObj = pdf.alloc();
    pdf.infoObj = infoObj;
    pdf.add(infoObj, BuildInfoObject(info));

    // The layer set belongs to the DOCUMENT, not to the sheet. Every sheet of a tiled plan
    // is written from one and the same content, so they all carry the same layer ids; one
    // OCG object per layer, referenced by every page that draws it, is what keeps the
    // reader's panel to one entry per layer rather than one per layer per sheet. Read off
    // the first page, which is the one whose content defined them.
    const SvgContent &first = pages.first();
    const bool plain = first.plain;
    const bool withOcg = !plain;

    // Separate the size layers from the decor, and tell a layered document from a plain
    // one: optional content only means something when there is something to switch.
    QVector<const LayerContent *> sizeLayers;
    const LayerContent *mepLayer = nullptr;
    for (const auto &lc : first.layers) {
        if (lc.plain)
            continue;
        if (lc.isMep)
            mepLayer = &lc;
        else
            sizeLayers.append(&lc);
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

    // Decor OCG (always ON)
    int mepOcgObj = 0;
    if (withOcg && mepLayer != nullptr) {
        mepOcgObj = pdf.alloc();
        pdf.add(mepOcgObj,
                "<< /Type /OCG /Name " + pdfTextString(mepLayer->displayName) + " >>\n");
    }

    // ExtGState object referenced by QPdfWriter content streams as /GSa gs
    const int gsaObj = pdf.alloc();
    pdf.add(gsaObj,
            QStringLiteral("<< /Type /ExtGState /SA true /SM 0.02 /ca 1.0 /CA 1.0"
                           " /AIS false /SMask /None >>\n").toUtf8());

    // The Properties dictionary binds each layer NAME — what the content streams address —
    // to its OCG object. It is the same on every page, because the OCG objects are the
    // document's; only the streams differ, and the names are what point into them.
    QByteArray propsDict;
    if (withOcg && mepLayer != nullptr) {
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

    QVector<int> pageObjs;
    pageObjs.reserve(pages.size());

    // One page per sheet, in the order the sheets were handed over — the order they are to
    // be printed and taped back together in.
    for (const SvgContent &sheet : pages) {
        const int pageObj    = pdf.alloc();
        const int contentObj = pdf.alloc();

        const double pageHeightPt = sheet.page.sizeMm.height() * kPointsPerMm;
        const double pageWidthPt  = sheet.page.sizeMm.width() * kPointsPerMm;

        // This sheet's streams, by layer name. Looking them up rather than indexing by
        // position keeps a sheet that carried fewer layers from shifting the others'.
        QHash<QString, const LayerContent *> byName;
        for (const LayerContent &lc : sheet.layers)
            byName.insert(lc.name, &lc);

        QByteArray streamData;

        if (plain) {
            // Nothing to mark: the whole document is the page.
            if (!sheet.layers.isEmpty())
                streamData.append(sheet.layers.first().stream);
        }

        // Size layers first (bottom)
        // Each layer is wrapped in q...Q to isolate its graphics state (CTM, colors, etc.)
        // since QPdfWriter's per-layer streams start with a cm that concatenates.
        for (const auto *sz : sizeLayers) {
            const auto it = byName.constFind(sz->name);
            if (it == byName.constEnd())
                continue;

            streamData.append(QStringLiteral("/OC /%1 BDC\nq\n").arg(sz->name).toUtf8());
            streamData.append(it.value()->stream);
            streamData.append("\nQ\n");
            streamData.append("EMC\n");
        }

        // Decor layer on top
        // QSvgRenderer content and text are in separate q/Q scopes so that
        // the QSvgRenderer CTM (0.75 0 0 -0.75 0 pageH cm) does not affect
        // the text operators, which have their own SVG→PDF conversion.
        if (withOcg && mepLayer != nullptr) {
            const auto it = byName.constFind(mepLayer->name);
            if (it != byName.constEnd()) {
                const LayerContent *mep = it.value();
                streamData.append(QStringLiteral("/OC /%1 BDC\n").arg(mep->name).toUtf8());
                // QSvgRenderer content (paths, logos, grainlines)
                streamData.append("q\n");
                streamData.append(mep->stream);
                streamData.append("\nQ\n");
                // Text operators (independent CTM scope)
                if (!mep->textStream.isEmpty()) {
                    streamData.append("q\n");
                    streamData.append(mep->textStream);
                    streamData.append("\nQ\n");
                }
                streamData.append("EMC\n");
            }
        }

        pdf.add(contentObj,
                QStringLiteral("<< /Length %1 >>\nstream\n").arg(streamData.size()).toUtf8()
                + streamData
                + "\nendstream");

        // Page
        pdf.add(pageObj,
                QStringLiteral(
                    "<< /Type /Page /Parent %1 0 R\n"
                    "   /MediaBox [0 0 %2 %3]\n"
                    "   /Contents %4 0 R\n"
                    "   /Resources <<\n"
                    "      /ExtGState << /GSa %5 0 R >>\n"
                    "      /ColorSpace << /CSp /DeviceRGB >>\n"
                    "      /Properties <<\n"
                    "%6"
                    "      >>\n"
                    "   >>\n"
                    "   /Group << /Type /Group /S /Transparency >> >>\n"
                ).arg(pagesObj)
                 .arg(pageWidthPt, 0, 'f', 6)
                 .arg(pageHeightPt, 0, 'f', 6)
                 .arg(contentObj)
                 .arg(gsaObj)
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

    if (mepLayer) {
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
    qDebug() << "  Pages:" << pageObjs.size();
    for (const SvgContent &sheet : pages) {
        qDebug() << "    " << sheet.page.sizeMm.width() << "×" << sheet.page.sizeMm.height()
                   << "mm";
    }
    if (withOcg) {
        qDebug() << "  OCG layers:" << sizeLayers.size()
                   << "+ decor:" << (mepLayer != nullptr ? 1 : 0);
    } else {
        qDebug() << "  Flat document: no optional content";
    }

    return true;
}
