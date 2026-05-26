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
#include <QPageSize>
#include <QPainter>
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

bool SvgToOcgPdf::convert(const QString &svgPath, const QString &pdfPath)
{
    QFile svgFile(svgPath);
    if (!svgFile.open(QIODevice::ReadOnly)) {
        qWarning() << "SvgToOcgPdf: cannot open" << svgPath;
        return false;
    }

    QDomDocument doc;
    QString errorMsg;
    int errorLine, errorCol;
    if (!doc.setContent(&svgFile, &errorMsg, &errorLine, &errorCol)) {
        qWarning() << "SvgToOcgPdf: XML parse error at line" << errorLine
                   << ":" << errorMsg;
        svgFile.close();
        return false;
    }
    svgFile.close();

    const PageInfo page = getPageInfo(doc);
    const QVector<LayerEntry> layers = findLayers(doc);

    if (layers.isEmpty()) {
        qWarning() << "SvgToOcgPdf: no layers found";
        return false;
    }

    qWarning() << "Found" << layers.size() << "layers, page"
               << page.sizeMm.width() << "x" << page.sizeMm.height() << "mm";

    QVector<LayerContent> contents;
    contents.reserve(layers.size());

    for (const auto &layer : layers) {
        qWarning() << "  Rendering layer:" << layer.name;

        const QByteArray layerSvg = buildLayerSvg(doc, layer.element, page);
        const QByteArray pdfBuf = renderToPdfBuffer(layerSvg, page);
        if (pdfBuf.isEmpty()) {
            qWarning() << "  Failed to render layer:" << layer.name;
            continue;
        }

        QByteArray stream = extractContentStream(pdfBuf);
        if (stream.isEmpty()) {
            qWarning() << "  Failed to extract stream for layer:" << layer.name;
            continue;
        }

        stream = decompressFlate(stream);

        QByteArray textStream;
        // For MEP layer, generate text as PDF path operators
        // (kept separate — must not be affected by QSvgRenderer's CTM)
        if (layer.isMep) {
            const double pageHeightPt = page.sizeMm.height() * kPointsPerMm;
            textStream = emitMepTextAsPdfOps(layer.element, pageHeightPt);
        }

        contents.append({layer.name, layer.isMep, stream, textStream});
    }

    if (contents.isEmpty()) {
        qWarning() << "SvgToOcgPdf: no layer content generated";
        return false;
    }

    return buildOcgPdf(pdfPath, page, contents);
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

        const bool isMep = id.contains(QStringLiteral("Mise"), Qt::CaseInsensitive)
                        || id.contains(QStringLiteral("en_page"), Qt::CaseInsensitive);

        result.append({id, isMep, el});
        qWarning() << "  Layer:" << id << (isMep ? "(MEP)" : "");
    }

    return result;
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

QByteArray SvgToOcgPdf::extractContentStream(const QByteArray &pdfBuf)
{
    // Find the content stream object: search for "/Contents N 0 R"
    // Then find "N 0 obj" and extract the stream data

    // Step 1: find the content object number from the page object
    const QByteArray marker = "/Contents ";
    int idx = pdfBuf.indexOf(marker);
    if (idx < 0) return {};

    idx += marker.size();
    // Read the object number
    int contentObjNum = 0;
    while (idx < pdfBuf.size() && pdfBuf[idx] >= '0' && pdfBuf[idx] <= '9') {
        contentObjNum = contentObjNum * 10 + (pdfBuf[idx] - '0');
        idx++;
    }
    if (contentObjNum <= 0) return {};

    // Step 2: find "N 0 obj" for the content object
    const QByteArray objMarker = QByteArray::number(contentObjNum) + " 0 obj";
    idx = pdfBuf.indexOf(objMarker);
    if (idx < 0) return {};

    // Step 3: find "stream" keyword after the object header
    idx = pdfBuf.indexOf("stream", idx);
    if (idx < 0) return {};

    // Skip "stream" and the following newline(s)
    idx += 6;
    if (idx < pdfBuf.size() && pdfBuf[idx] == '\r') idx++;
    if (idx < pdfBuf.size() && pdfBuf[idx] == '\n') idx++;

    const int streamStart = idx;

    // Step 4: find "endstream"
    // Search backwards from end to find the matching endstream
    int endIdx = pdfBuf.indexOf("endstream", streamStart);
    if (endIdx < 0) return {};

    // Trim trailing whitespace before endstream
    int streamEnd = endIdx;
    while (streamEnd > streamStart && (pdfBuf[streamEnd - 1] == '\n'
           || pdfBuf[streamEnd - 1] == '\r' || pdfBuf[streamEnd - 1] == ' '))
        streamEnd--;

    return pdfBuf.mid(streamStart, streamEnd - streamStart);
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
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            qWarning() << "decompressFlate: inflate failed with" << ret;
            inflateEnd(&strm);
            return {};
        }
        result.append(chunk.data(), kChunkSize - strm.avail_out);
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

bool SvgToOcgPdf::buildOcgPdf(const QString                &pdfPath,
                               const PageInfo               &page,
                               const QVector<LayerContent>  &layers)
{
    PdfBuilder pdf;
    pdf.header = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";

    const int catalogObj = pdf.alloc();
    const int pagesObj   = pdf.alloc();

    // Separate size layers from MEP
    QVector<const LayerContent *> sizeLayers;
    const LayerContent *mepLayer = nullptr;
    for (const auto &lc : layers) {
        if (lc.isMep)
            mepLayer = &lc;
        else
            sizeLayers.append(&lc);
    }

    // OCG objects — one per size
    QVector<int> ocgObjs;
    ocgObjs.reserve(sizeLayers.size());
    for (const auto *sz : sizeLayers) {
        QByteArray body;
        QTextStream ts(&body);
        QString displayName = sz->name;
        displayName.replace(QLatin1Char('_'), QLatin1Char(' '));
        ts << "<< /Type /OCG /Name (" << displayName << ") >>\n";
        ts.flush();
        const int objNum = pdf.alloc();
        ocgObjs.append(objNum);
        pdf.add(objNum, body);
    }

    // MEP OCG (always ON)
    int mepOcgObj = 0;
    if (mepLayer) {
        mepOcgObj = pdf.alloc();
        pdf.add(mepOcgObj,
                QStringLiteral("<< /Type /OCG /Name (Mise en page) >>\n").toUtf8());
    }

    // ExtGState object referenced by QPdfWriter content streams as /GSa gs
    const int gsaObj = pdf.alloc();
    pdf.add(gsaObj,
            QStringLiteral("<< /Type /ExtGState /SA true /SM 0.02 /ca 1.0 /CA 1.0"
                           " /AIS false /SMask /None >>\n").toUtf8());

    const int pageObj    = pdf.alloc();
    const int contentObj = pdf.alloc();

    // Build content stream with BDC/EMC for each layer
    const double pageHeightPt = page.sizeMm.height() * kPointsPerMm;
    const double pageWidthPt  = page.sizeMm.width() * kPointsPerMm;

    QByteArray streamData;

    // Size layers first (bottom)
    // Each layer is wrapped in q...Q to isolate its graphics state (CTM, colors, etc.)
    // since QPdfWriter's per-layer streams start with a cm that concatenates.
    for (int i = 0; i < sizeLayers.size(); ++i) {
        const QString name = sizeLayers[i]->name;

        streamData.append(
            QStringLiteral("/OC /%1 BDC\nq\n").arg(name).toUtf8());

        streamData.append(sizeLayers[i]->stream);
        streamData.append("\nQ\n");

        streamData.append("EMC\n");
    }

    // Mise-en-page layer on top
    // QSvgRenderer content and text are in separate q/Q scopes so that
    // the QSvgRenderer CTM (0.75 0 0 -0.75 0 pageH cm) does not affect
    // the text operators, which have their own SVG→PDF conversion.
    if (mepLayer) {
        streamData.append("/OC /Mise_en_page BDC\n");
        // QSvgRenderer content (paths, logos, grainlines)
        streamData.append("q\n");
        streamData.append(mepLayer->stream);
        streamData.append("\nQ\n");
        // Text operators (independent CTM scope)
        if (!mepLayer->textStream.isEmpty()) {
            streamData.append("q\n");
            streamData.append(mepLayer->textStream);
            streamData.append("\nQ\n");
        }
        streamData.append("EMC\n");
    }

    pdf.add(contentObj,
            QStringLiteral("<< /Length %1 >>\nstream\n").arg(streamData.size()).toUtf8()
            + streamData
            + "\nendstream");

    // Build Properties dict for OCG
    QByteArray propsDict;
    if (mepLayer) {
        propsDict.append(QStringLiteral("      /Mise_en_page %1 0 R\n")
                             .arg(mepOcgObj).toUtf8());
    }
    for (int i = 0; i < sizeLayers.size(); ++i) {
        propsDict.append(QStringLiteral("      /%1 %2 0 R\n")
                             .arg(sizeLayers[i]->name)
                             .arg(ocgObjs[i]).toUtf8());
    }

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

    // Pages
    pdf.add(pagesObj,
            QStringLiteral("<< /Type /Pages /Kids [%1 0 R] /Count 1 >>\n")
                .arg(pageObj).toUtf8());

    // Catalog with OCG
    QByteArray ocgRefs;
    QByteArray orderRefs;

    if (mepLayer) {
        const QByteArray ref = QByteArray::number(mepOcgObj) + " 0 R ";
        ocgRefs.append(ref);
        orderRefs.append(ref);
    }
    for (int i = 0; i < ocgObjs.size(); ++i) {
        const QByteArray ref = QByteArray::number(ocgObjs[i]) + " 0 R ";
        ocgRefs.append(ref);
        orderRefs.append(ref);
    }
    // All layers visible by default
    const QByteArray offRefs("[]");

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

    qWarning() << "OCG PDF exported:" << pdfPath;
    qWarning() << "  Page size:" << pageWidthPt << "×" << pageHeightPt << "pt"
               << "(" << page.sizeMm.width() << "×" << page.sizeMm.height() << "mm)";
    qWarning() << "  OCG layers:" << sizeLayers.size()
               << "+ MEP:" << (mepLayer ? 1 : 0);

    return true;
}
