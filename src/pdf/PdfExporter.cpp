/************************************************************************
 **
 **  @file   PdfExporter.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  PDF exporter implementation — writes PDF-1.7 with OCG from scratch.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include "PdfExporter.h"
#include "PdfBuilder.h"

#include <QBuffer>
#include <QDebug>
#include <QDomDocument>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QMap>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QSet>
#include <QSvgRenderer>
#include <QTextStream>

#define NANOSVG_IMPLEMENTATION
#include "io/nanosvg.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// ── Constants ─────────────────────────────────────────────────────────────

static constexpr double kScale        = 1e6;
static constexpr double kPointsPerMm  = 72.0 / 25.4;          // 1 mm = 2.8346 pt
static constexpr double kPixelsPerMm  = 96.0 / 25.4;          // 1 mm = 3.7795 px @ 96 DPI
static constexpr double kCoordToPt    = 72.0 / (1e6 * 96.0);  // Clipper2 → PDF points
static constexpr double kPixelToPt    = 72.0 / 96.0;            // SVG pixel → PDF points

// ── 2D affine transform (row-vector convention: [x' y' 1] = [x y 1] × M) ────

struct Affine2D
{
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

    Affine2D() = default;
    Affine2D(double a_, double b_, double c_, double d_, double e_, double f_)
        : a(a_), b(b_), c(c_), d(d_), e(e_), f(f_) {}

    /// Pre-multiply: returns m × this (m applied first, then this).
    Affine2D preMul(const Affine2D &m) const
    {
        return {
            m.a * a + m.c * b,
            m.b * a + m.d * b,
            m.a * c + m.c * d,
            m.b * c + m.d * d,
            m.a * e + m.c * f + m.e,
            m.b * e + m.d * f + m.f
        };
    }

    /// Post-multiply: returns this × o (this applied first, then o).
    Affine2D postMul(const Affine2D &o) const
    {
        return {
            o.a * a + o.c * b,
            o.b * a + o.d * b,
            o.a * c + o.c * d,
            o.b * c + o.d * d,
            o.a * e + o.c * f + o.e,
            o.b * e + o.d * f + o.f
        };
    }

    /// Apply transform to a point (modifies x, y in place).
    void apply(double &x, double &y) const
    {
        const double nx = a * x + c * y + e;
        y = b * x + d * y + f;
        x = nx;
    }
};

/// Parse a single SVG transform function. Returns true on success.
static bool parseOneTransform(const QString &fn, Affine2D &m)
{
    const int paren = fn.indexOf(QLatin1Char('('));
    if (paren < 0) return false;
    const QString name = fn.left(paren).trimmed().toLower();
    const int rparen = fn.lastIndexOf(QLatin1Char(')'));
    if (rparen < 0) return false;

    const QStringView argsView = QStringView(fn).mid(paren + 1, rparen - paren - 1);
    // Split by comma or whitespace
    QString argStr = argsView.toString();
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
            // rotate(deg cx cy) = translate(cx,cy) rotate(deg) translate(-cx,-cy)
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

/// Parse a full SVG transform attribute (space-separated transform functions).
/// SVG applies transforms right-to-left; this returns the composed matrix.
static Affine2D parseSvgTransform(const QString &tx)
{
    Affine2D result;
    if (tx.isEmpty()) return result;

    // Split by "function(…)" pattern
    QStringList funcs;
    int depth = 0, fnStart = 0;
    for (int i = 0; i < tx.size(); ++i) {
        const QChar ch = tx[i];
        if (ch == QLatin1Char('(')) {
            if (depth == 0) {
                // Walk back to find start of function name
                fnStart = i - 1;
                while (fnStart >= 0 && tx[fnStart] != QLatin1Char(' '))
                    fnStart--;
                fnStart++; // first char of function name
            }
            depth++;
        } else if (ch == QLatin1Char(')')) {
            depth--;
            if (depth == 0)
                funcs.append(tx.mid(fnStart, i - fnStart + 1));
        }
    }

    if (funcs.isEmpty())
        return result;

    // Compose: SVG applies left-to-right list as right-to-left transforms.
    // For row vectors: p' = p × M_rightmost × … × M_leftmost.
    // So we pre-multiply: result = M_current × result.
    for (const auto &fn : funcs) {
        Affine2D m;
        if (parseOneTransform(fn, m))
            result = result.preMul(m);
    }

    return result;
}

// ── Color parsing ─────────────────────────────────────────────────────────

namespace {

/// Convert a single hex character to its integer value (0–15).
static inline int hexDigitValue(QChar c)
{
    const ushort u = c.unicode();
    if (u >= '0' && u <= '9') return u - '0';
    if (u >= 'a' && u <= 'f') return u - 'a' + 10;
    if (u >= 'A' && u <= 'F') return u - 'A' + 10;
    return 0;
}

/// Parse an SVG colour string to PDF RGB components (0..1 range).
/// Handles: "none", "#RGB", "#RRGGBB", "rgb(r,g,b)".
/// Returns true if the colour is valid (not "none").
bool parseColor(const QString &s, double &r, double &g, double &b)
{
    if (s.isEmpty() || s == QStringLiteral("none"))
        return false;

    // #RGB or #RRGGBB
    if (s.startsWith(QLatin1Char('#'))) {
        const QStringView hex = QStringView(s).mid(1);
        if (hex.size() == 3) {
            r = hexDigitValue(hex[0]) / 15.0;
            g = hexDigitValue(hex[1]) / 15.0;
            b = hexDigitValue(hex[2]) / 15.0;
            return true;
        }
        if (hex.size() == 6) {
            r = (hexDigitValue(hex[0]) * 16 + hexDigitValue(hex[1])) / 255.0;
            g = (hexDigitValue(hex[2]) * 16 + hexDigitValue(hex[3])) / 255.0;
            b = (hexDigitValue(hex[4]) * 16 + hexDigitValue(hex[5])) / 255.0;
            return true;
        }
        return false;
    }

    // rgb(r, g, b)
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

    // SVG/CSS named colours (common subset)
    static const QHash<QString, quint32> namedColors = {
        {QStringLiteral("black"),   0x000000},
        {QStringLiteral("white"),   0xFFFFFF},
        {QStringLiteral("red"),     0xFF0000},
        {QStringLiteral("green"),   0x008000},
        {QStringLiteral("blue"),    0x0000FF},
        {QStringLiteral("yellow"),  0xFFFF00},
        {QStringLiteral("orange"),  0xFFA500},
        {QStringLiteral("purple"),  0x800080},
        {QStringLiteral("gray"),    0x808080},
        {QStringLiteral("grey"),    0x808080},
        {QStringLiteral("silver"),  0xC0C0C0},
        {QStringLiteral("maroon"),  0x800000},
        {QStringLiteral("navy"),    0x000080},
        {QStringLiteral("teal"),    0x008080},
        {QStringLiteral("lime"),    0x00FF00},
        {QStringLiteral("fuchsia"), 0xFF00FF},
        {QStringLiteral("aqua"),    0x00FFFF},
        {QStringLiteral("olive"),   0x808000},
    };
    const auto it = namedColors.find(s.toLower());
    if (it != namedColors.end()) {
        const quint32 c = it.value();
        r = ((c >> 16) & 0xFF) / 255.0;
        g = ((c >> 8) & 0xFF) / 255.0;
        b = (c & 0xFF) / 255.0;
        return true;
    }

    return false;
}

/// Format a Clipper2 point to a PDF coordinate string: "x y"
inline QString fmtPt(int64_t x, int64_t y, double pageHeightPt)
{
    const double xPt = x * kCoordToPt;
    const double yPt = pageHeightPt - y * kCoordToPt; // flip Y: SVG top-down → PDF bottom-up
    return QStringLiteral("%1 %2")
        .arg(xPt, 0, 'f', 6)
        .arg(yPt, 0, 'f', 6);
}

/// Emit style and path geometry for one polygon.
void emitPath(QByteArray &buf, const Clipper2Lib::Path64 &contour,
              const PathStyle &style, double pageHeightPt)
{
    if (contour.size() < 2)
        return;

    const bool isClosed = contour.size() >= 3 && contour.front() == contour.back();

    double sr, sg, sb;
    const bool hasStroke = parseColor(style.stroke, sr, sg, sb);

    double fr, fg, fb;
    const bool hasFill = parseColor(style.fill, fr, fg, fb);

    // Fill only makes sense for closed paths
    const bool hasEffectiveFill = hasFill && isClosed;

    const double lwPt = style.strokeWidth * (72.0 / 96.0);

    if (!style.strokeDasharray.isEmpty() && hasStroke) {
        const QStringList parts = style.strokeDasharray.split(QLatin1Char(' '));
        QStringList pdfParts;
        for (const auto &p : parts)
            pdfParts.append(QString::number(p.toDouble() * (72.0 / 96.0), 'f', 4));
        buf.append(QStringLiteral("[%1] 0 d\n").arg(pdfParts.join(QLatin1Char(' '))).toUtf8());
    }

    buf.append(QStringLiteral("%1 w\n").arg(lwPt, 0, 'f', 4).toUtf8());

    if (hasStroke)
        buf.append(QStringLiteral("%1 %2 %3 RG\n").arg(sr, 0, 'f', 4).arg(sg, 0, 'f', 4).arg(sb, 0, 'f', 4).toUtf8());
    if (hasEffectiveFill)
        buf.append(QStringLiteral("%1 %2 %3 rg\n").arg(fr, 0, 'f', 4).arg(fg, 0, 'f', 4).arg(fb, 0, 'f', 4).toUtf8());

    buf.append(fmtPt(contour[0].x, contour[0].y, pageHeightPt).toUtf8());
    buf.append(" m\n");

    for (size_t i = 1; i < contour.size(); ++i) {
        buf.append(fmtPt(contour[i].x, contour[i].y, pageHeightPt).toUtf8());
        buf.append(" l\n");
    }

    if (isClosed)
        buf.append("h\n");

    if (hasEffectiveFill && hasStroke)
        buf.append("B\n");
    else if (hasEffectiveFill)
        buf.append("f\n");
    else if (hasStroke)
        buf.append("S\n");

    if (!style.strokeDasharray.isEmpty() && hasStroke)
        buf.append("[] 0 d\n");
}

/// Render a text label as vector outlines in PDF content stream.
/// Uses QPainterPath::addText() to convert text to Bezier paths,
/// producing font-perfect rendering independent of PDF font availability.
static void emitTextLabel(QByteArray &buf, const MepLabel &lbl, double pageHeightPt)
{
    // Apply text element's own transform (if any) to the position
    double px = lbl.x;
    double py = lbl.y;
    double extraRot = 0.0;
    if (!lbl.transform.isEmpty()) {
        const Affine2D tx = parseSvgTransform(lbl.transform);
        tx.apply(px, py);
        extraRot = std::atan2(tx.b, tx.a) * 180.0 / M_PI;
    }

    // Build QFont matching the SVG font
    QFont font(lbl.fontFamily);
    // Font size in SVG is in points; convert to pixels for QPainterPath (96 DPI)
    font.setPixelSize(static_cast<int>(std::round(lbl.fontSize * 72.0 / 96.0)));

    // Parse font weight: "bold" keyword or numeric 100–900
    const QString fw = lbl.fontWeight.toLower();
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
        const int num = lbl.fontWeight.toInt(&ok);
        if (ok) font.setWeight(static_cast<QFont::Weight>(num));
    }
    font.setStyleName(QString()); // ensure normal style for addText()

    // Generate outline path
    QPainterPath textPath;
    textPath.addText(0.0, 0.0, font, lbl.text);

    if (textPath.isEmpty())
        return;

    // Fill color (default to black)
    double fr = 0, fg = 0, fb = 0;
    parseColor(lbl.fill, fr, fg, fb);

    // Build PDF transformation matrix from text-local coords to PDF points.
    // Text path is in Qt pixel coords (96 DPI, Y-down).
    // Transformation: scale(72/96, 72/96) × rotate(netRot) × translate(xPt, yPt)
    // where yPt already includes the Y-flip for PDF.
    const double scl = kPixelToPt; // 72/96
    const double netRot = -(lbl.rotation + extraRot); // SVG CW → PDF CCW
    const double rad = netRot * M_PI / 180.0;
    const double c = std::cos(rad), sn = std::sin(rad);

    // cm matrix: [a b c d e f] = [scl*c  scl*sn  scl*sn  -scl*c  xPt  yPt]
    const double a = scl * c;
    const double b_coord = scl * sn;
    const double cc = scl * sn;
    const double d = -scl * c;

    const double xPt = px * scl;
    const double yPt = pageHeightPt - py * scl;

    buf.append("q\n");
    buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 cm\n")
               .arg(a, 0, 'f', 6).arg(b_coord, 0, 'f', 6)
               .arg(cc, 0, 'f', 6).arg(d, 0, 'f', 6)
               .arg(xPt, 0, 'f', 6).arg(yPt, 0, 'f', 6).toUtf8());
    buf.append(QStringLiteral("%1 %2 %3 rg\n").arg(fr, 0, 'f', 4).arg(fg, 0, 'f', 4).arg(fb, 0, 'f', 4).toUtf8());

    // Emit path elements
    for (int i = 0; i < textPath.elementCount(); ++i) {
        const QPainterPath::Element el = textPath.elementAt(i);
        switch (el.type) {
        case QPainterPath::MoveToElement:
            buf.append(QStringLiteral("%1 %2 m\n").arg(el.x, 0, 'f', 3).arg(el.y, 0, 'f', 3).toUtf8());
            break;
        case QPainterPath::LineToElement:
            buf.append(QStringLiteral("%1 %2 l\n").arg(el.x, 0, 'f', 3).arg(el.y, 0, 'f', 3).toUtf8());
            break;
        case QPainterPath::CurveToElement: {
            const QPainterPath::Element cp2 = textPath.elementAt(i + 1);
            const QPainterPath::Element endPt = textPath.elementAt(i + 2);
            buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 c\n")
                       .arg(el.x, 0, 'f', 3).arg(el.y, 0, 'f', 3)
                       .arg(cp2.x, 0, 'f', 3).arg(cp2.y, 0, 'f', 3)
                       .arg(endPt.x, 0, 'f', 3).arg(endPt.y, 0, 'f', 3).toUtf8());
            i += 2;
            break;
        }
        default:
            break;
        }
    }
    // Non-zero winding fill (correct for glyph paths with holes like 'O', 'D', etc.)
    buf.append("f\n");
    buf.append("Q\n");
}

/// Convert nanosvg's packed color (0x00BBGGRR) to PDF RGB components.
static void nsvgColorToPdf(unsigned int color, double &r, double &g, double &b)
{
    r = (color & 0xFF) / 255.0;
    g = ((color >> 8) & 0xFF) / 255.0;
    b = ((color >> 16) & 0xFF) / 255.0;
}

/// Render an SVG fragment (logo) using nanosvg as PDF vector paths.
/// nanosvg handles all CSS, transforms, and path syntax natively.
static void emitSvgFragment(QByteArray &buf, const MepLabel &lbl, double pageHeightPt)
{
    if (lbl.svgContent.isEmpty())
        return;

    // Wrap fragment in a full SVG document for nanosvg
    const QString wrapped = QStringLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\">%1</svg>").arg(lbl.svgContent);

    QByteArray utf8 = wrapped.toUtf8();
    // nanosvg modifies the input buffer; work on a copy
    std::vector<char> copy(utf8.size() + 1);
    memcpy(copy.data(), utf8.constData(), utf8.size() + 1);

    NSVGimage *img = nsvgParse(copy.data(), "px", 96.0f);
    if (!img)
        return;

    // Position using absolute coords computed by SizeRestorer (same as text labels)
    const double xPt = lbl.x * kPixelToPt;
    const double yPt = pageHeightPt - lbl.y * kPixelToPt;
    // net rotation (SVG CW → PDF CCW)
    const double netRot = -lbl.rotation;
    const double rad = netRot * M_PI / 180.0;
    const double c = std::cos(rad), s = std::sin(rad);
    const double kPt = kPixelToPt;
    const double m_a  = kPt * c;
    const double m_b  = kPt * s;
    const double m_c  = kPt * s;
    const double m_d  = -kPt * c;
    const double m_e  = xPt;
    const double m_f  = yPt;

    buf.append("q\n");
    buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 cm\n")
               .arg(m_a, 0, 'f', 6).arg(m_b, 0, 'f', 6)
               .arg(m_c, 0, 'f', 6).arg(m_d, 0, 'f', 6)
               .arg(m_e, 0, 'f', 6).arg(m_f, 0, 'f', 6).toUtf8());

    // Render shapes parsed by nanosvg
    for (NSVGshape *shape = img->shapes; shape; shape = shape->next) {
        if (!shape->paths)
            continue;

        const bool hasFill   = shape->fill.type   == NSVG_PAINT_COLOR;
        const bool hasStroke = shape->stroke.type == NSVG_PAINT_COLOR;

        if (hasFill) {
            double fr, fg, fb;
            nsvgColorToPdf(shape->fill.color, fr, fg, fb);
            buf.append(QStringLiteral("%1 %2 %3 rg\n").arg(fr, 0, 'f', 4).arg(fg, 0, 'f', 4).arg(fb, 0, 'f', 4).toUtf8());
        }
        if (hasStroke) {
            double sr, sg, sb;
            nsvgColorToPdf(shape->stroke.color, sr, sg, sb);
            buf.append(QStringLiteral("%1 %2 %3 RG\n").arg(sr, 0, 'f', 4).arg(sg, 0, 'f', 4).arg(sb, 0, 'f', 4).toUtf8());
            buf.append(QStringLiteral("%1 w\n").arg(shape->strokeWidth * kPixelToPt, 0, 'f', 4).toUtf8());
        }

        if (!hasFill && !hasStroke)
            continue;

        for (NSVGpath *path = shape->paths; path; path = path->next) {
            if (path->npts < 2) continue;

            buf.append(QStringLiteral("%1 %2 m\n").arg(path->pts[0], 0, 'f', 3).arg(path->pts[1], 0, 'f', 3).toUtf8());

            for (int i = 0; i < path->npts - 1; i += 3) {
                const float *p = &path->pts[i * 2];
                buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 c\n")
                           .arg(p[2], 0, 'f', 3).arg(p[3], 0, 'f', 3)
                           .arg(p[4], 0, 'f', 3).arg(p[5], 0, 'f', 3)
                           .arg(p[6], 0, 'f', 3).arg(p[7], 0, 'f', 3).toUtf8());
            }

            if (path->closed)
                buf.append("h\n");
        }

        if (hasFill && hasStroke)      buf.append("B\n");
        else if (hasFill)              buf.append("f\n");
        else if (hasStroke)            buf.append("S\n");
    }

    nsvgDelete(img);
    buf.append("Q\n");
}

// ── FlatLabel rendering (from .rabnest) ─────────────────────────────────

/// Render a FlatLabel::Text using QPainterPath::addText (same as emitTextLabel).
static void emitFlatTextLabel(QByteArray &buf, const FlatLabel &lbl, double pageHeightPt)
{
    QFont font(lbl.fontFamily);
    font.setPixelSize(static_cast<int>(std::round(lbl.fontSize * 72.0 / 96.0)));

    const QString fw = lbl.fontWeight.toLower();
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
        const int num = lbl.fontWeight.toInt(&ok);
        if (ok) font.setWeight(static_cast<QFont::Weight>(num));
    }
    font.setStyleName(QString());

    // Compute text width for alignment
    QFontMetrics fm(font);
    const double textWidth = static_cast<double>(fm.horizontalAdvance(lbl.text));

    double offsetX = 0.0;
    if (lbl.align == QStringLiteral("center"))
        offsetX = -textWidth / 2.0;
    else if (lbl.align == QStringLiteral("right"))
        offsetX = -textWidth;

    QPainterPath textPath;
    textPath.addText(offsetX, 0.0, font, lbl.text);
    if (textPath.isEmpty()) return;

    double fr = 0, fg = 0, fb = 0;
    parseColor(lbl.fill, fr, fg, fb);

    const double scl = kPixelToPt; // 72/96
    const double netRot = -lbl.rotation; // SVG CW → PDF CCW
    const double rad = netRot * M_PI / 180.0;
    const double c = std::cos(rad), sn = std::sin(rad);

    const double a = scl * c;
    const double b_coord = scl * sn;
    const double cc = scl * sn;
    const double d = -scl * c;
    const double xPt = lbl.x * kPixelToPt;
    const double yPt = pageHeightPt - lbl.y * kPixelToPt;

    buf.append("q\n");
    buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 cm\n")
                   .arg(a, 0, 'f', 6).arg(b_coord, 0, 'f', 6)
                   .arg(cc, 0, 'f', 6).arg(d, 0, 'f', 6)
                   .arg(xPt, 0, 'f', 6).arg(yPt, 0, 'f', 6)
                   .toUtf8());
    buf.append(QStringLiteral("%1 %2 %3 RG %1 %2 %3 rg\n")
                   .arg(fr, 0, 'f', 6).arg(fg, 0, 'f', 6).arg(fb, 0, 'f', 6)
                   .toUtf8());

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
                               .arg(endPt.x, 0, 'f', 3).arg(endPt.y, 0, 'f', 3).toUtf8());
                i += 2;
                break;
            }
            default: break;
        }
    }

    buf.append("f\n");
    buf.append("Q\n");
}

// ── SVG path data parser ─────────────────────────────────────────────

/// Parse an SVG path data string to a QPainterPath, handling relative/absolute commands.
static QPainterPath parseSvgPathData(const QString &d)
{
    QPainterPath pp;
    if (d.isEmpty()) return pp;

    double curX = 0, curY = 0;
    double startX = 0, startY = 0;
    double lastCx2 = 0, lastCy2 = 0; // for smooth cubic reflection
    QChar lastCmd = QChar::Null;

    const QChar *pos = d.constData();
    const QChar *end = pos + d.size();

    auto readNum = [&]() -> double {
        while (pos < end && (*pos == QLatin1Char(' ') || *pos == QLatin1Char(',')
               || *pos == QLatin1Char('\t') || *pos == QLatin1Char('\n')
               || *pos == QLatin1Char('\r'))) ++pos;
        if (pos >= end) return 0.0;
        const QChar *nStart = pos;
        if (*pos == QLatin1Char('-') || *pos == QLatin1Char('+')) ++pos;
        while (pos < end && pos->isDigit()) ++pos;
        if (pos < end && *pos == QLatin1Char('.')) { ++pos; while (pos < end && pos->isDigit()) ++pos; }
        if (pos == nStart) return 0.0;
        return QStringView(nStart, pos - nStart).toDouble();
    };

    while (pos < end) {
        while (pos < end && (*pos == QLatin1Char(' ') || *pos == QLatin1Char(',')
               || *pos == QLatin1Char('\t') || *pos == QLatin1Char('\n')
               || *pos == QLatin1Char('\r'))) ++pos;
        if (pos >= end) break;

        QChar cmd;
        if (pos->isLetter()) {
            cmd = *pos;
            ++pos;
        } else {
            if (lastCmd == QLatin1Char('M')) cmd = QLatin1Char('L');
            else if (lastCmd == QLatin1Char('m')) cmd = QLatin1Char('l');
            else cmd = lastCmd;
        }

        switch (cmd.toLatin1()) {
        case 'M': {
            double x = readNum(), y = readNum();
            pp.moveTo(x, y);
            curX = x; curY = y;
            startX = x; startY = y;
            lastCx2 = x; lastCy2 = y;
            break;
        }
        case 'm': {
            double dx = readNum(), dy = readNum();
            curX += dx; curY += dy;
            pp.moveTo(curX, curY);
            startX = curX; startY = curY;
            lastCx2 = curX; lastCy2 = curY;
            break;
        }
        case 'L': {
            double x = readNum(), y = readNum();
            pp.lineTo(x, y);
            curX = x; curY = y;
            lastCx2 = x; lastCy2 = y;
            break;
        }
        case 'l': {
            double dx = readNum(), dy = readNum();
            curX += dx; curY += dy;
            pp.lineTo(curX, curY);
            lastCx2 = curX; lastCy2 = curY;
            break;
        }
        case 'H': {
            double x = readNum();
            pp.lineTo(x, curY);
            curX = x;
            lastCx2 = x; lastCy2 = curY;
            break;
        }
        case 'h': {
            double dx = readNum();
            curX += dx;
            pp.lineTo(curX, curY);
            lastCx2 = curX; lastCy2 = curY;
            break;
        }
        case 'V': {
            double y = readNum();
            pp.lineTo(curX, y);
            curY = y;
            lastCx2 = curX; lastCy2 = y;
            break;
        }
        case 'v': {
            double dy = readNum();
            curY += dy;
            pp.lineTo(curX, curY);
            lastCx2 = curX; lastCy2 = curY;
            break;
        }
        case 'C': {
            double cx1 = readNum(), cy1 = readNum();
            double cx2 = readNum(), cy2 = readNum();
            double x = readNum(), y = readNum();
            pp.cubicTo(cx1, cy1, cx2, cy2, x, y);
            curX = x; curY = y;
            lastCx2 = cx2; lastCy2 = cy2;
            break;
        }
        case 'c': {
            double dx1 = readNum(), dy1 = readNum();
            double dx2 = readNum(), dy2 = readNum();
            double dx = readNum(), dy = readNum();
            double cx1 = curX + dx1, cy1 = curY + dy1;
            double cx2 = curX + dx2, cy2 = curY + dy2;
            double x = curX + dx, y = curY + dy;
            pp.cubicTo(cx1, cy1, cx2, cy2, x, y);
            curX = x; curY = y;
            lastCx2 = cx2; lastCy2 = cy2;
            break;
        }
        case 'S': {
            double refCx = 2 * curX - lastCx2;
            double refCy = 2 * curY - lastCy2;
            double cx2 = readNum(), cy2 = readNum();
            double x = readNum(), y = readNum();
            pp.cubicTo(refCx, refCy, cx2, cy2, x, y);
            curX = x; curY = y;
            lastCx2 = cx2; lastCy2 = cy2;
            break;
        }
        case 's': {
            double refCx = 2 * curX - lastCx2;
            double refCy = 2 * curY - lastCy2;
            double dx2 = readNum(), dy2 = readNum();
            double dx = readNum(), dy = readNum();
            double cx2 = curX + dx2, cy2 = curY + dy2;
            double x = curX + dx, y = curY + dy;
            pp.cubicTo(refCx, refCy, cx2, cy2, x, y);
            curX = x; curY = y;
            lastCx2 = cx2; lastCy2 = cy2;
            break;
        }
        case 'Z': case 'z':
            pp.closeSubpath();
            curX = startX; curY = startY;
            lastCx2 = curX; lastCy2 = curY;
            break;
        default:
            break;
        }
        lastCmd = cmd;
    }

    return pp;
}

/// Emit a QPainterPath as PDF path operators (m, l, c, h).
static void emitPainterPath(QByteArray &buf, const QPainterPath &pp)
{
    for (int i = 0; i < pp.elementCount(); ++i) {
        const QPainterPath::Element e = pp.elementAt(i);
        switch (e.type) {
        case QPainterPath::MoveToElement:
            buf.append(QStringLiteral("%1 %2 m\n")
                           .arg(e.x, 0, 'f', 6).arg(e.y, 0, 'f', 6).toUtf8());
            break;
        case QPainterPath::LineToElement:
            buf.append(QStringLiteral("%1 %2 l\n")
                           .arg(e.x, 0, 'f', 6).arg(e.y, 0, 'f', 6).toUtf8());
            break;
        case QPainterPath::CurveToElement: {
            const QPainterPath::Element cp2 = pp.elementAt(i + 1);
            const QPainterPath::Element endPt = pp.elementAt(i + 2);
            buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 c\n")
                           .arg(e.x, 0, 'f', 6).arg(e.y, 0, 'f', 6)
                           .arg(cp2.x, 0, 'f', 6).arg(cp2.y, 0, 'f', 6)
                           .arg(endPt.x, 0, 'f', 6).arg(endPt.y, 0, 'f', 6).toUtf8());
            i += 2;
            break;
        }
        default: break;
        }
    }
}

/// Render a FlatLabel::Logo — parse SVG paths and emit as PDF operators.
static void emitLogoLabel(QByteArray &buf, const FlatLabel &lbl, double pageHeightPt)
{
    if (lbl.logoPaths.isEmpty()) return;

    const double logoScale = (lbl.logoScale > 0.0) ? lbl.logoScale : 1.0;
    const double scl = kPixelToPt * logoScale; // 72/96 with logo scale
    const double rad = -lbl.rotation * M_PI / 180.0;
    const double c = std::cos(rad), sn = std::sin(rad);

    const double a = scl * c;
    const double b_coord = scl * sn;
    const double cc = scl * sn;
    const double d = -scl * c;
    const double tx = lbl.x * kPixelToPt;
    const double ty = pageHeightPt - lbl.y * kPixelToPt;

    buf.append("q\n");
    buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 cm\n")
                   .arg(a, 0, 'f', 6).arg(b_coord, 0, 'f', 6)
                   .arg(cc, 0, 'f', 6).arg(d, 0, 'f', 6)
                   .arg(tx, 0, 'f', 6).arg(ty, 0, 'f', 6)
                   .toUtf8());

    for (const auto &lp : lbl.logoPaths) {
        if (lp.d.isEmpty()) continue;

        QPainterPath pp = parseSvgPathData(lp.d);
        if (pp.isEmpty()) continue;

        double fr = 0, fg = 0, fb = 0;
        double sr = 0, sg = 0, sb = 0;
        parseColor(lp.fill, fr, fg, fb);
        parseColor(lp.stroke, sr, sg, sb);

        bool hasFill = (lp.fill != QStringLiteral("none") && !lp.fill.isEmpty());
        bool hasStroke = (lp.stroke != QStringLiteral("none") && !lp.stroke.isEmpty()
                          && lp.strokeWidth > 0);

        emitPainterPath(buf, pp);

        if (hasFill && hasStroke)
            buf.append(QStringLiteral("%1 %2 %3 RG %4 %5 %6 rg B\n")
                           .arg(sr, 0, 'f', 6).arg(sg, 0, 'f', 6).arg(sb, 0, 'f', 6)
                           .arg(fr, 0, 'f', 6).arg(fg, 0, 'f', 6).arg(fb, 0, 'f', 6).toUtf8());
        else if (hasFill)
            buf.append(QStringLiteral("%1 %2 %3 rg f\n")
                           .arg(fr, 0, 'f', 6).arg(fg, 0, 'f', 6).arg(fb, 0, 'f', 6).toUtf8());
        else if (hasStroke)
            buf.append(QStringLiteral("%1 %2 %3 RG S\n")
                           .arg(sr, 0, 'f', 6).arg(sg, 0, 'f', 6).arg(sb, 0, 'f', 6).toUtf8());
    }

    buf.append("Q\n");
}

/// Estimate the bounding box of an SVG fragment by parsing text and transform attributes.
/// Returns a QRect in SVG coordinate space, or an empty rect if nothing found.
// ── Label rendering via original SVG + QSvgRenderer ──────────────────

/// Render a label element from the original SVG file.
/// Uses QSvgRenderer on the FULL original SVG (correct viewBox, all styles)
/// and renders only the specified element by its ID.
/// Returns JPEG bytes, or empty on failure. Outputs image dimensions
/// and exact content bounds from the original SVG coordinate space.
static QByteArray renderLabelFromOriginalSvg(const QString &svgFilePath,
                                              const QString &elementId,
                                              int &imgW, int &imgH,
                                              QRectF &contentBounds)
{
    QSvgRenderer renderer(svgFilePath);
    if (!renderer.isValid()) return {};

    // Get exact bounds of the element in SVG coordinates
    contentBounds = renderer.boundsOnElement(elementId);
    if (contentBounds.isEmpty()) return {};

    // Render at 300 DPI
    constexpr double kDpiScale = 300.0 / 96.0;
    imgW = qMax(1, static_cast<int>(std::ceil(contentBounds.width() * kDpiScale)));
    imgH = qMax(1, static_cast<int>(std::ceil(contentBounds.height() * kDpiScale)));

    QImage image(imgW, imgH, QImage::Format_RGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        renderer.render(&painter, elementId, QRectF(0, 0, imgW, imgH));
    }

    QByteArray jpeg;
    QBuffer jpgBuf(&jpeg);
    jpgBuf.open(QIODevice::WriteOnly);
    if (!image.save(&jpgBuf, "JPEG", 85)) return {};
    jpgBuf.close();

    return jpeg;
}

/// Emit an Image XObject reference (Do) for a RasterBlock label.
static void emitLabelImage(QByteArray &buf, int imgObjNum, int imgW, int imgH,
                           const MepLabel &lbl, QRectF bounds, double pageHeightPt)
{
    const double scl = kPixelToPt; // 72/96

    // Offset between original SVG position and SizeRestorer-transformed position
    const double tx = (lbl.x - lbl.origX) * scl;
    const double ty = pageHeightPt + (lbl.origY - lbl.y) * scl;

    // Bottom-left corner of the image bounds in PDF coordinates
    const double blX = bounds.x() * scl + tx;
    const double blY = -(bounds.y() + bounds.height()) * scl + ty;

    // Image scale: 1 image pixel = 72/300 PDF points (rendered at 300 DPI)
    constexpr double kPdfPerPx = 72.0 / 300.0;
    const double imgWPt = imgW * kPdfPerPx;
    const double imgHPt = imgH * kPdfPerPx;

    const double cx = lbl.x * scl;
    const double cy = pageHeightPt - lbl.y * scl;

    buf.append("q\n");

    if (std::abs(lbl.rotation) > 0.001) {
        const double rad = -lbl.rotation * M_PI / 180.0;
        const double c = std::cos(rad), s = std::sin(rad);
        // Rotate around (cx, cy) in PDF space
        buf.append(QStringLiteral("%1 %2 %3 %4 %5 %6 cm\n")
                   .arg(c, 0, 'f', 6).arg(s, 0, 'f', 6)
                   .arg(-s, 0, 'f', 6).arg(c, 0, 'f', 6)
                   .arg(cx - c*cx + s*cy, 0, 'f', 6)
                   .arg(cy - s*cx - c*cy, 0, 'f', 6).toUtf8());
    }

    buf.append(QStringLiteral("%1 0 0 %2 %3 %4 cm\n")
               .arg(imgWPt, 0, 'f', 6).arg(imgHPt, 0, 'f', 6)
               .arg(blX, 0, 'f', 6).arg(blY, 0, 'f', 6).toUtf8());
    buf.append(QStringLiteral("/Im%1 Do\n").arg(imgObjNum).toUtf8());
    buf.append("Q\n");
}

/// Build a PDF content stream for one OCG layer (one size).
QByteArray buildSizeLayerContent(const std::vector<std::pair<const TransformedPiece *, const TransformedSize *>> &entries,
                                  double pageHeightPt)
{
    QByteArray buf;

    for (const auto &[piece, sz] : entries) {
        const PathStyle &style = sz->style;

        // Main contour
        emitPath(buf, sz->contour, style, pageHeightPt);

        // Internal paths (passmarks, grainlines)
        for (const auto &ip : sz->internalPaths)
            emitPath(buf, ip, style, pageHeightPt);
    }

    return buf;
}

} // anonymous namespace

// ── Public API ────────────────────────────────────────────────────────────

bool PdfExporter::write(const QString                       &path,
                        const std::vector<TransformedPiece> &pieces,
                        const Config                        &cfg)
{
    if (pieces.empty()) {
        qWarning() << "PdfExporter: no pieces to export";
        return false;
    }

    // ── Collect unique sizes ────────────────────────────────────────────

    // Group (piece, size) entries by size value for OCG layers
    QMap<double, std::vector<std::pair<const TransformedPiece *, const TransformedSize *>>> sizeGroups;

    for (const auto &piece : pieces) {
        for (const auto &sz : piece.sizes) {
            if (sz.contour.size() >= 3)
                sizeGroups[sz.size].emplace_back(&piece, &sz);
        }
    }

    if (sizeGroups.isEmpty()) {
        qWarning() << "PdfExporter: no valid contours";
        return false;
    }

    const QList<double> sortedSizes = sizeGroups.keys();

    // ── Compute page dimensions (including labels) ──────────────────────

    int64_t contentMinY = std::numeric_limits<int64_t>::max();
    int64_t contentMaxY = 0;
    for (const auto &piece : pieces) {
        for (const auto &sz : piece.sizes) {
            for (const auto &pt : sz.contour) {
                if (pt.y < contentMinY) contentMinY = pt.y;
                if (pt.y > contentMaxY) contentMaxY = pt.y;
            }
        }
        // Labels (96 DPI pixels → Clipper2 coords) — ensure they fit on page
        for (const auto &lbl : piece.flatLabels) {
            const int64_t ly = static_cast<int64_t>(std::round(lbl.y * kScale));
            if (ly < contentMinY) contentMinY = ly;
            if (ly > contentMaxY) contentMaxY = ly;
        }
    }
    if (contentMinY == std::numeric_limits<int64_t>::max())
        contentMinY = 0;

    const double pageWidthPt  = cfg.binWidthMm * kPointsPerMm;
    // Page height covers from minY to maxY; negative minY adds headroom
    const double pageHeightPt = ((contentMaxY - contentMinY) / kScale) * (72.0 / 96.0);
    const double yOffsetPx    = (contentMinY < 0) ? (-contentMinY / kScale) : 0.0;

    // ── Build PDF ──────────────────────────────────────────────────────

    PdfBuilder pdf;
    pdf.header = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n"; // binary comment for broad compliance

    const int catalogObj  = pdf.alloc();
    const int pagesObj    = pdf.alloc();

    // ── Pre-process RasterBlock labels → Image XObjects ─────────────

    struct LabelImgInfo {
        int objNum, imgW, imgH;
        QRectF bounds;
    };
    QHash<QPair<QString, int>, LabelImgInfo> labelImgMap; // (pieceKey, blockId) → info

    if (!cfg.svgFilePath.isEmpty()) {
        for (const auto &piece : pieces) {
            for (const auto &lbl : piece.mepLabels) {
                if (lbl.type != MepLabel::RasterBlock || lbl.elementId.isEmpty())
                    continue;

                int imgW = 0, imgH = 0;
                QRectF bounds;
                const QByteArray jpeg = renderLabelFromOriginalSvg(
                    cfg.svgFilePath, lbl.elementId, imgW, imgH, bounds);
                if (jpeg.isEmpty()) continue;

                // Create Image XObject
                const int imgObjNum = pdf.alloc();
                QByteArray imgDict;
                imgDict.append("<< /Type /XObject /Subtype /Image\n");
                imgDict.append("   /Width " + QByteArray::number(imgW) + "\n");
                imgDict.append("   /Height " + QByteArray::number(imgH) + "\n");
                imgDict.append("   /ColorSpace /DeviceRGB\n");
                imgDict.append("   /BitsPerComponent 8\n");
                imgDict.append("   /Filter /DCTDecode\n");
                imgDict.append("   /Length " + QByteArray::number(jpeg.size()) + "\n>>\n");
                imgDict.append("stream\n");
                imgDict.append(jpeg);
                imgDict.append("\nendstream");
                pdf.add(imgObjNum, imgDict);

                QString pieceKey = piece.groupName.isEmpty()
                    ? piece.pieceName
                    : piece.groupName + QStringLiteral("::") + piece.pieceName;
                labelImgMap.insert({pieceKey, lbl.blockId},
                                   {imgObjNum, imgW, imgH, bounds});
            }
        }
    }

    // Check for Mise-en-page paths and labels
    bool hasMep = false;
    for (const auto &piece : pieces) {
        if (!piece.mepPaths.isEmpty() || !piece.mepLabels.isEmpty()
            || !piece.flatLabels.isEmpty()) { hasMep = true; break; }
    }

    // MEP OCG (optional, comes first = bottom layer)
    int mepOcgObj = 0;
    if (hasMep) {
        mepOcgObj = pdf.alloc();
        pdf.add(mepOcgObj,
                QStringLiteral("<< /Type /OCG /Name (Mise_en_page) >>\n").toUtf8());
    }

    // OCG objects — one per size
    QVector<int> ocgObjs;
    ocgObjs.reserve(sortedSizes.size());
    for (int i = 0; i < sortedSizes.size(); ++i) {
        const int objNum = pdf.alloc();
        ocgObjs.append(objNum);
        const QString name = QStringLiteral("Size_%1").arg(sortedSizes[i], 0, 'f', 0);
        pdf.add(objNum,
                QStringLiteral("<< /Type /OCG /Name (%1) >>\n").arg(name).toUtf8());
    }

    const int pageObj    = pdf.alloc();
    const int contentObj = pdf.alloc();

    // ── Content stream ─────────────────────────────────────────────────

    QByteArray streamData;

    // Size layers rendered first (bottom) so contours don't cover labels
    for (int si = 0; si < sortedSizes.size(); ++si) {
        const double sizeVal = sortedSizes[si];
        const int    ocgNum  = ocgObjs[si];

        const auto &entries = sizeGroups[sizeVal];

        streamData.append(
            QStringLiteral("/OC /Size_%1 BDC\n")
                .arg(sizeVal, 0, 'f', 0).toUtf8());

        streamData.append(buildSizeLayerContent(entries, pageHeightPt));

        streamData.append("EMC\n");
    }

    // Mise-en-page layer rendered last (on top) so labels are always visible
    if (hasMep) {
        streamData.append("/OC /Mise_en_page BDC\n");
        QSet<QString> renderedGroups; // deduplicate labels for group pieces sharing a footprint
        for (const auto &piece : pieces) {
            for (const auto &mp : piece.mepPaths)
                emitPath(streamData, mp.polyline, mp.style, pageHeightPt);

            // Skip labels if this group was already rendered (avoids overlapping
            // labels from variant pieces that share the same footprint).
            if (!piece.groupName.isEmpty() && renderedGroups.contains(piece.groupName))
                continue;
            if (!piece.groupName.isEmpty())
                renderedGroups.insert(piece.groupName);

            for (const auto &lbl : piece.mepLabels) {
                if (lbl.type == MepLabel::RasterBlock) {
                    QString pieceKey = piece.groupName.isEmpty()
                        ? piece.pieceName
                        : piece.groupName + QStringLiteral("::") + piece.pieceName;
                    auto it = labelImgMap.constFind({pieceKey, lbl.blockId});
                    if (it != labelImgMap.constEnd())
                        emitLabelImage(streamData, it->objNum, it->imgW, it->imgH,
                                       lbl, it->bounds, pageHeightPt);
                } else if (lbl.type == MepLabel::Text)
                    emitTextLabel(streamData, lbl, pageHeightPt);
                else if (lbl.type == MepLabel::SvgFragment)
                    emitSvgFragment(streamData, lbl, pageHeightPt);
            }
            for (const auto &lbl : piece.flatLabels) {
                if (lbl.type == LabelType::Text)
                    emitFlatTextLabel(streamData, lbl, pageHeightPt);
                else if (lbl.type == LabelType::Logo)
                    emitLogoLabel(streamData, lbl, pageHeightPt);
            }
        }
        streamData.append("EMC\n");
    }

    // If content extends above y=0, shift everything down by adding a global
    // translation so no label is clipped by the page media box.
    const double yOffsetPt = yOffsetPx * kPixelToPt;
    if (yOffsetPt > 0.0) {
        const QByteArray shift = QStringLiteral("q 1 0 0 1 0 %1 cm\n")
                                     .arg(-yOffsetPt, 0, 'f', 6).toUtf8();
        streamData.prepend(shift);
        streamData.append("Q\n");
    }

    pdf.add(contentObj,
            QStringLiteral("<< /Length %1 >>\nstream\n").arg(streamData.size()).toUtf8()
            + streamData
            + "\nendstream");

    // ── Build XObject dict for Image XObject references ──────────

    QByteArray xobjDict;
    if (!labelImgMap.isEmpty()) {
        xobjDict.append("      /XObject <<\n");
        for (auto it = labelImgMap.constBegin(); it != labelImgMap.constEnd(); ++it) {
            xobjDict.append(QStringLiteral("        /Im%1 %2 0 R\n")
                           .arg(it->objNum).arg(it->objNum).toUtf8());
        }
        xobjDict.append("      >>\n");
    }

    // ── Build Properties dict for OCG references in BDC ──────────────

    QByteArray propsDict;
    if (hasMep) {
        propsDict.append(QStringLiteral("      /Mise_en_page %1 0 R\n")
                         .arg(mepOcgObj).toUtf8());
    }
    for (int i = 0; i < sortedSizes.size(); ++i) {
        const QString name = QStringLiteral("Size_%1").arg(sortedSizes[i], 0, 'f', 0);
        propsDict.append(QStringLiteral("      /%1 %2 0 R\n")
                         .arg(name).arg(ocgObjs[i]).toUtf8());
    }

    // ── Page ───────────────────────────────────────────────────────────

    pdf.add(pageObj,
            QStringLiteral(
                "<< /Type /Page /Parent %1 0 R\n"
                "   /MediaBox [0 0 %2 %3]\n"
                "   /Contents %4 0 R\n"
                "   /Resources <<\n"
                "%5"
                "      /Properties <<\n"
                "%6"
                "      >>\n"
                "   >>\n"
                "   /Group << /Type /Group /S /Transparency >> >>\n"
            ).arg(pagesObj)
             .arg(pageWidthPt, 0, 'f', 6)
             .arg(pageHeightPt, 0, 'f', 6)
             .arg(contentObj)
             .arg(QString::fromUtf8(xobjDict))
             .arg(QString::fromUtf8(propsDict))
             .toUtf8());

    // ── Pages ──────────────────────────────────────────────────────────

    pdf.add(pagesObj,
            QStringLiteral(
                "<< /Type /Pages /Kids [%1 0 R] /Count 1 >>\n"
            ).arg(pageObj).toUtf8());

    // ── Catalog with OCG ───────────────────────────────────────────────

    QByteArray ocgRefs;
    QByteArray orderRefs;
    QByteArray offRefs;
    // MEP is always ON, never in OFF list
    if (hasMep) {
        const QString mepRef = QStringLiteral("%1 0 R ").arg(mepOcgObj);
        ocgRefs.append(mepRef.toUtf8());
        orderRefs.append(mepRef.toUtf8());
    }
    for (int i = 0; i < ocgObjs.size(); ++i) {
        const QString ref = QStringLiteral("%1 0 R ").arg(ocgObjs[i]);
        ocgRefs.append(ref.toUtf8());
        orderRefs.append(ref.toUtf8());
        // First size ON by default, others OFF
        if (i > 0)
            offRefs.append(ref.toUtf8());
    }

    if (offRefs.isEmpty())
        offRefs = QByteArrayLiteral("[]");

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

    // ── Write file ─────────────────────────────────────────────────────

    int xrefOffset = 0;
    const QByteArray fileContent = pdf.build(xrefOffset);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "PdfExporter: cannot open" << path;
        return false;
    }

    file.write(fileContent);
    file.close();

    qWarning() << "PDF exported:" << path;
    qWarning() << "  Page size:" << pageWidthPt << "×" << pageHeightPt << "pt"
               << "(" << cfg.binWidthMm << "×" << (pageHeightPt / kPointsPerMm) << "mm)";
    qWarning() << "  OCG layers:" << sortedSizes.size();

    return true;
}

// ── Simple PDF via QPdfWriter + QPainter (no OCG) ────────────────────────────

static constexpr double kInvScale = 1.0 / 1e6;   // Clipper2 → 96 DPI pixels
static constexpr double kPtToPx  = 96.0 / 72.0;   // points → pixels at 96 DPI

/// Convert a Clipper2 coordinate to a QPointF in 96 DPI pixels.
static inline QPointF toPx(int64_t x, int64_t y) {
    return {static_cast<double>(x) * kInvScale, static_cast<double>(y) * kInvScale};
}

/// Convert a Clipper2 contour to a closed QPainterPath.
static QPainterPath toPath(const Clipper2Lib::Path64 &pts)
{
    QPainterPath p;
    if (pts.size() < 2) return p;
    p.moveTo(toPx(pts[0].x, pts[0].y));
    for (size_t i = 1; i < pts.size(); ++i)
        p.lineTo(toPx(pts[i].x, pts[i].y));
    if (pts.size() >= 3 && pts.front() == pts.back())
        p.closeSubpath();
    return p;
}

bool PdfExporter::writeSimple(const QString                       &path,
                               const std::vector<TransformedPiece> &pieces,
                               double binWidthMm,
                               double usedHeightMm)
{
    if (pieces.empty()) {
        qWarning() << "PdfExporter::writeSimple: no pieces";
        return false;
    }

    // Collect unique sizes
    QMap<double, QVector<QPair<const TransformedPiece *, const TransformedSize *>>> sizesMap;
    for (const auto &p : pieces)
        for (const auto &sz : p.sizes)
            if (sz.contour.size() >= 3)
                sizesMap[sz.size].append({&p, &sz});

    if (sizesMap.isEmpty()) {
        qWarning() << "PdfExporter::writeSimple: no valid contours";
        return false;
    }

    const QList<double> sortedSizes = sizesMap.keys();

    // Check for MEP content
    bool hasMep = false;
    for (const auto &p : pieces) {
        if (!p.mepPaths.isEmpty() || !p.flatLabels.isEmpty()) {
            hasMep = true;
            break;
        }
    }
    qWarning() << "MEP content:" << hasMep;

    // Page dimensions
    constexpr double kMmToPx = 96.0 / 25.4;
    const double pageWPx = binWidthMm * kMmToPx;
    const double pageHPx = usedHeightMm * kMmToPx;

    QPdfWriter writer(path);
    writer.setResolution(96);
    writer.setPdfVersion(QPagedPaintDevice::PdfVersion_1_4);
    writer.setPageSize(QPageSize(QSizeF(binWidthMm, usedHeightMm), QPageSize::Millimeter));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));

    QPainter painter(&writer);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Paint in 96-DPI pixel coordinates with Y-up (PDF convention).
    // Clipper2 paths are converted to pixels via toPx() / toPath().
    // Text is rendered in pixel coordinates directly.
    painter.translate(0.0, pageHPx);
    painter.scale(1.0, -1.0);

    auto setPen = [&](const QString &stroke, double strokeWidth, const QString &dashArray) {
        QPen pen;
        QColor sc(stroke);
        if (sc.isValid() && stroke != QStringLiteral("none")) {
            pen.setColor(sc);
            pen.setStyle(Qt::SolidLine);
        } else {
            pen.setStyle(Qt::NoPen);
        }
        pen.setWidthF(strokeWidth * (72.0 / 96.0)); // px → pt
        if (!dashArray.isEmpty())
            pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
    };

    auto setBrush = [&](const QString &fill) {
        QColor fc(fill);
        if (fc.isValid() && fill != QStringLiteral("none"))
            painter.setBrush(QBrush(fc));
        else
            painter.setBrush(Qt::NoBrush);
    };

    // ── MEP layer (rendered first = bottom) ──
    if (hasMep) {
        for (const auto &p : pieces) {
            // Grainlines
            for (const auto &mp : p.mepPaths) {
                if (mp.polyline.size() < 2) continue;
                setPen(mp.style.stroke, mp.style.strokeWidth, mp.style.strokeDasharray);
                painter.setBrush(Qt::NoBrush);
                QPainterPath path;
                path.moveTo(toPx(mp.polyline[0].x, mp.polyline[0].y));
                for (size_t i = 1; i < mp.polyline.size(); ++i)
                    path.lineTo(toPx(mp.polyline[i].x, mp.polyline[i].y));
                painter.drawPath(path);
            }

            // FlatLabels
            for (const auto &fl : p.flatLabels) {
                if (fl.type == LabelType::Text) {
                    QFont font(fl.fontFamily);
                    font.setPixelSize(static_cast<int>(fl.fontSize * kPtToPx));
                    if (fl.fontWeight == QStringLiteral("bold"))
                        font.setBold(true);

                    painter.save();
                    painter.translate(fl.x, fl.y);
                    painter.rotate(-fl.rotation);

                    painter.setFont(font);
                    QPen textPen(QColor(fl.fill));
                    painter.setPen(textPen);
                    painter.setBrush(Qt::NoBrush);

                    // Align text
                    int flags = Qt::AlignLeft | Qt::AlignTop;
                    if (fl.align == QStringLiteral("center"))
                        flags = Qt::AlignHCenter | Qt::AlignTop;
                    else if (fl.align == QStringLiteral("right"))
                        flags = Qt::AlignRight | Qt::AlignTop;

                    painter.drawText(QRectF(0, 0, 10000, 10000), flags, fl.text);
                    painter.restore();
                }
                else if (fl.type == LabelType::Logo) {
                    painter.save();
                    painter.translate(fl.x, fl.y);
                    painter.rotate(-fl.rotation);

                    for (const auto &lp : fl.logoPaths) {
                        setPen(lp.stroke, lp.strokeWidth, lp.strokeDasharray);
                        setBrush(lp.fill);

                        QPainterPath qpath;
                        const QString &d = lp.d;
                        const QChar *pos = d.constData();
                        const QChar *end = pos + d.size();
                        while (pos < end) {
                            while (pos < end && pos->isSpace()) ++pos;
                            if (pos >= end) break;
                            QChar cmd = *pos; ++pos;
                            auto readNum = [&]() -> double {
                                while (pos < end && (pos->isSpace() || *pos == QLatin1Char(','))) ++pos;
                                const QChar *start = pos;
                                if (pos < end && (*pos == QLatin1Char('-') || *pos == QLatin1Char('+'))) ++pos;
                                while (pos < end && pos->isDigit()) ++pos;
                                if (pos < end && *pos == QLatin1Char('.')) { ++pos; while (pos < end && pos->isDigit()) ++pos; }
                                if (pos == start) return 0.0;
                                return QStringView(start, pos - start).toDouble();
                            };
                            switch (cmd.toLatin1()) {
                                case 'M': case 'm': { qpath.moveTo(readNum(), readNum()); break; }
                                case 'L': case 'l': { qpath.lineTo(readNum(), readNum()); break; }
                                case 'C': case 'c': { double cx1 = readNum(), cy1 = readNum(), cx2 = readNum(), cy2 = readNum(), x = readNum(), y = readNum(); qpath.cubicTo(cx1, cy1, cx2, cy2, x, y); break; }
                                case 'Z': case 'z': qpath.closeSubpath(); break;
                                default: break;
                            }
                        }
                        painter.drawPath(qpath);
                    }
                    painter.restore();
                }
            }
        }
    }

    // ── Size layers ──
    for (double sizeVal : sortedSizes) {
        const auto &entries = sizesMap[sizeVal];
        for (const auto &[piece, sz] : entries) {
            const PathStyle &style = sz->style;
            setPen(style.stroke, style.strokeWidth, style.strokeDasharray);
            setBrush(style.fill);

            // Contour
            painter.drawPath(toPath(sz->contour));

            // Internal paths
            for (const auto &ip : sz->internalPaths) {
                if (ip.size() >= 2) {
                    QPainterPath p;
                    p.moveTo(toPx(ip[0].x, ip[0].y));
                    for (size_t i = 1; i < ip.size(); ++i)
                        p.lineTo(toPx(ip[i].x, ip[i].y));
                    painter.drawPath(p);
                }
            }
        }
    }

    painter.end();

    qWarning() << "PDF (simple) exported:" << path
               << "page:" << binWidthMm << "×" << usedHeightMm << "mm";

    return true;
}
