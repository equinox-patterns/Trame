/************************************************************************
 **
 **  @file   RabnestReader.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   24/05/2026
 **
 **  @brief  Reader for the .rabnest XML exchange format.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include "RabnestReader.h"

#include <QFile>
#include <QXmlStreamReader>

using namespace Clipper2Lib;

namespace {

static constexpr int kCurveSegments = 32;

enum class SvgCmd { MoveTo, LineTo, CurveTo, ClosePath, None };

struct SvgTokenizer
{
    const QChar *pos = nullptr;
    const QChar *end = nullptr;

    explicit SvgTokenizer(const QString &d)
        : pos(d.constData()), end(pos + d.size()) {}

    bool atEnd()
    {
        while (pos < end && pos->isSpace()) ++pos;
        return pos >= end;
    }

    double nextNumber()
    {
        while (pos < end && pos->isSpace()) ++pos;
        if (pos < end && *pos == QLatin1Char(',')) {
            ++pos;
            while (pos < end && pos->isSpace()) ++pos;
        }
        const QChar *start = pos;
        if (pos < end && (*pos == QLatin1Char('-') || *pos == QLatin1Char('+')))
            ++pos;
        while (pos < end && pos->isDigit()) ++pos;
        if (pos < end && *pos == QLatin1Char('.')) {
            ++pos;
            while (pos < end && pos->isDigit()) ++pos;
        }
        if (pos == start) return 0.0;
        return QStringView(start, pos - start).toDouble();
    }

    SvgCmd nextCommand()
    {
        while (pos < end && pos->isSpace()) ++pos;
        if (pos >= end) return SvgCmd::None;
        const QChar c = *pos;
        ++pos;
        switch (c.toLatin1()) {
            case 'M': return SvgCmd::MoveTo;
            case 'L': return SvgCmd::LineTo;
            case 'C': return SvgCmd::CurveTo;
            case 'Z': case 'z': return SvgCmd::ClosePath;
            default:  return SvgCmd::None;
        }
    }
};

/// Build a lookup key for MEP data from piece name and optional group.
QString mepKey(const QString &pieceName, const QString &groupName)
{
    return groupName.isEmpty()
        ? pieceName
        : groupName + QStringLiteral("::") + pieceName;
}

/// Parse a hex color string (e.g. "#ff0000" or "#f00") into R,G,B strings.
/// Returns "0 0 0" (black) for "none" or invalid inputs.
QString parseHexColor(const QString &s)
{
    if (s.isEmpty() || s == QStringLiteral("none"))
        return QStringLiteral("0 0 0");

    QString hex = s;
    if (hex.startsWith(QLatin1Char('#')))
        hex = hex.mid(1);

    if (hex.size() == 3) {
        hex = QStringLiteral("%1%1%2%2%3%3")
                  .arg(hex[0]).arg(hex[1]).arg(hex[2]);
    }

    if (hex.size() != 6)
        return QStringLiteral("0 0 0");

    bool ok;
    const int r = hex.mid(0, 2).toInt(&ok, 16);
    const int g = hex.mid(2, 2).toInt(&ok, 16);
    const int b = hex.mid(4, 2).toInt(&ok, 16);
    return QStringLiteral("%1 %2 %3").arg(r).arg(g).arg(b);
}

} // anonymous namespace

// ── Coordinate helpers ────────────────────────────────────────────────────

Point64 RabnestReader::toPoint64(double x, double y) const
{
    return {
        static_cast<int64_t>(std::llround(x * kScale)),
        static_cast<int64_t>(std::llround(y * kScale))
    };
}

void RabnestReader::interpolateBezier(Point64 p1, Point64 cp1,
                                       Point64 cp2, Point64 p4,
                                       Path64 &out) const
{
    for (int i = 1; i <= kCurveSegments; ++i) {
        const double t = static_cast<double>(i) / kCurveSegments;
        const double t2 = t * t;
        const double t3 = t2 * t;
        const double u  = 1.0 - t;
        const double u2 = u * u;
        const double u3 = u2 * u;

        out.push_back({
            static_cast<int64_t>(std::llround(
                u3 * p1.x + 3.0 * u2 * t * cp1.x + 3.0 * u * t2 * cp2.x + t3 * p4.x)),
            static_cast<int64_t>(std::llround(
                u3 * p1.y + 3.0 * u2 * t * cp1.y + 3.0 * u * t2 * cp2.y + t3 * p4.y))
        });
    }
}

Path64 RabnestReader::parsePathData(const QString &d) const
{
    Path64 out;
    if (d.isEmpty()) return out;

    SvgTokenizer tok(d);
    Point64 current{0, 0};
    Point64 subpathStart{0, 0};

    while (!tok.atEnd()) {
        const SvgCmd cmd = tok.nextCommand();
        switch (cmd) {
            case SvgCmd::MoveTo: {
                const double x = tok.nextNumber();
                const double y = tok.nextNumber();
                current = toPoint64(x, y);
                subpathStart = current;
                out.push_back(current);
                break;
            }
            case SvgCmd::LineTo: {
                const double x = tok.nextNumber();
                const double y = tok.nextNumber();
                current = toPoint64(x, y);
                out.push_back(current);
                break;
            }
            case SvgCmd::CurveTo: {
                const double cx1 = tok.nextNumber();
                const double cy1 = tok.nextNumber();
                const double cx2 = tok.nextNumber();
                const double cy2 = tok.nextNumber();
                const double x   = tok.nextNumber();
                const double y   = tok.nextNumber();
                const Point64 cp1 = toPoint64(cx1, cy1);
                const Point64 cp2 = toPoint64(cx2, cy2);
                const Point64 ep  = toPoint64(x, y);
                interpolateBezier(current, cp1, cp2, ep, out);
                current = ep;
                break;
            }
            case SvgCmd::ClosePath: {
                out.push_back(subpathStart);
                break;
            }
            case SvgCmd::None:
                return out;
        }
    }
    return out;
}

// ── Range listing ──────────────────────────────────────────────────────────

QVector<RangeInfo> RabnestReader::listRanges(const QString &filePath)
{
    QVector<RangeInfo> ranges;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("RabnestReader: cannot open file: %s", qUtf8Printable(filePath));
        return ranges;
    }

    QXmlStreamReader xml(&file);

    // Navigate to <rabnest>
    if (!xml.readNextStartElement() || xml.name() != QStringLiteral("rabnest"))
        return ranges;

    // Navigate to <sizes>
    while (xml.readNextStartElement()) {
        if (xml.name() == QStringLiteral("sizes"))
            break;
        xml.skipCurrentElement();
    }
    if (xml.name() != QStringLiteral("sizes"))
        return ranges;

    int idx = 0;
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("range")) {
            xml.skipCurrentElement();
            continue;
        }

        RangeInfo ri;
        ri.index = idx;
        ri.minSize = xml.attributes().value(QStringLiteral("minSize")).toDouble();
        ri.maxSize = xml.attributes().value(QStringLiteral("maxSize")).toDouble();
        ri.stature = xml.attributes().value(QStringLiteral("stature")).toDouble();
        ranges.append(ri);

        xml.skipCurrentElement();
        ++idx;
    }

    return ranges;
}

// ── Main reading functions ────────────────────────────────────────────────

static PathStyle parsePathAttributes(const QXmlStreamReader &xml)
{
    PathStyle s;
    if (xml.attributes().hasAttribute(QStringLiteral("fill")))
        s.fill = xml.attributes().value(QStringLiteral("fill")).toString();
    if (xml.attributes().hasAttribute(QStringLiteral("stroke")))
        s.stroke = xml.attributes().value(QStringLiteral("stroke")).toString();
    if (xml.attributes().hasAttribute(QStringLiteral("strokeWidth")))
        s.strokeWidth = xml.attributes().value(QStringLiteral("strokeWidth")).toDouble();
    if (xml.attributes().hasAttribute(QStringLiteral("strokeDasharray")))
        s.strokeDasharray = xml.attributes().value(QStringLiteral("strokeDasharray")).toString();
    return s;
}

bool RabnestReader::read(const QString &filePath, QVector<SizeLayer> &layers)
{
    return readRange(filePath, 0, layers);
}

bool RabnestReader::readRange(const QString &filePath, int rangeIndex,
                               QVector<SizeLayer> &layers)
{
    layers.clear();
    mepPaths.clear();
    flatLabels.clear();
    m_rangeCount = 0;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning("RabnestReader: cannot open file: %s", qUtf8Printable(filePath));
        return false;
    }

    QXmlStreamReader xml(&file);

    // Navigate to <rabnest>
    if (!xml.readNextStartElement() || xml.name() != QStringLiteral("rabnest")) {
        qWarning("RabnestReader: missing <rabnest> root element");
        return false;
    }

    // Navigate to <sizes> (skip <info>)
    while (xml.readNextStartElement()) {
        if (xml.name() == QStringLiteral("sizes"))
            break;
        xml.skipCurrentElement();
    }
    if (xml.name() != QStringLiteral("sizes")) {
        qWarning("RabnestReader: missing <sizes> element");
        return false;
    }

    // Find the requested range
    int currentRange = 0;
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("range")) {
            xml.skipCurrentElement();
            continue;
        }

        ++m_rangeCount;

        if (currentRange != rangeIndex) {
            xml.skipCurrentElement();
            ++currentRange;
            continue;
        }

        // Parse this range
        const double rangeStature =
            xml.attributes().value(QStringLiteral("stature")).toDouble();

        while (xml.readNextStartElement()) {
            if (xml.name() == QStringLiteral("mep")) {
                // ── Parse MEP ──────────────────────────────────────────
                while (xml.readNextStartElement()) {
                    if (xml.name() != QStringLiteral("piece")) {
                        xml.skipCurrentElement();
                        continue;
                    }

                    const QString pName = xml.attributes().value(QStringLiteral("name")).toString();
                    const QString gName = xml.attributes().value(QStringLiteral("group")).toString();
                    const QString key = mepKey(pName, gName);
                    const double pieceMx = xml.attributes().value(QStringLiteral("mx")).toDouble();
                    const double pieceMy = xml.attributes().value(QStringLiteral("my")).toDouble();

                    while (xml.readNextStartElement()) {
                        if (xml.name() == QStringLiteral("grainlines")) {
                            while (xml.readNextStartElement()) {
                                if (xml.name() == QStringLiteral("path")) {
                                    PathStyle style = parsePathAttributes(xml);
                                    const QString d = xml.attributes().value(QStringLiteral("d")).toString();
                                    MepPath mp;
                                    mp.polyline = parsePathData(d);
                                    mp.style = style;
                                    mepPaths[key].append(std::move(mp));
                                }
                                xml.skipCurrentElement();
                            }
                        }
                        else if (xml.name() == QStringLiteral("labels")) {
                            while (xml.readNextStartElement()) {
                                if (xml.name() == QStringLiteral("label")) {
                                    FlatLabel lbl;
                                    const QString typeStr = xml.attributes()
                                        .value(QStringLiteral("type")).toString();
                                    lbl.type = (typeStr == QStringLiteral("logo"))
                                        ? LabelType::Logo : LabelType::Text;

                                    lbl.blockId = xml.attributes()
                                        .value(QStringLiteral("blockId")).toInt();
                                    lbl.x = xml.attributes()
                                        .value(QStringLiteral("x")).toDouble();
                                    lbl.y = xml.attributes()
                                        .value(QStringLiteral("y")).toDouble();
                                    lbl.rotation = xml.attributes()
                                        .value(QStringLiteral("rotation")).toDouble();
                                    lbl.pieceMx = pieceMx;
                                    lbl.pieceMy = pieceMy;
                                    if (xml.attributes().hasAttribute(QStringLiteral("scale")))
                                        lbl.logoScale = xml.attributes()
                                            .value(QStringLiteral("scale")).toDouble();

                                    if (lbl.type == LabelType::Text) {
                                        lbl.fontFamily = xml.attributes()
                                            .value(QStringLiteral("fontFamily")).toString();
                                        lbl.fontSize = xml.attributes()
                                            .value(QStringLiteral("fontSize")).toDouble();
                                        lbl.fontWeight = xml.attributes()
                                            .value(QStringLiteral("fontWeight")).toString();
                                        lbl.fill = xml.attributes()
                                            .value(QStringLiteral("fill")).toString();
                                        lbl.align = xml.attributes()
                                            .value(QStringLiteral("align")).toString();
                                        // Read text content
                                        lbl.text = xml.readElementText().trimmed();
                                        flatLabels[key].append(lbl);
                                        continue; // already consumed the element
                                    }
                                    else {
                                        // Logo: read child <path> elements
                                        while (xml.readNextStartElement()) {
                                            if (xml.name() == QStringLiteral("path")) {
                                                FlatLabel::LogoPath lp;
                                                lp.d = xml.attributes()
                                                    .value(QStringLiteral("d")).toString();
                                                lp.fill = xml.attributes()
                                                    .value(QStringLiteral("fill")).toString();
                                                lp.stroke = xml.attributes()
                                                    .value(QStringLiteral("stroke")).toString();
                                                lp.strokeWidth = xml.attributes()
                                                    .value(QStringLiteral("strokeWidth")).toDouble();
                                                lp.strokeDasharray = xml.attributes()
                                                    .value(QStringLiteral("strokeDasharray")).toString();
                                                lp.fillRule = xml.attributes()
                                                    .value(QStringLiteral("fillRule")).toString();
                                                lbl.logoPaths.append(lp);
                                            }
                                            xml.skipCurrentElement();
                                        }
                                        flatLabels[key].append(lbl);
                                    }
                                }
                                else {
                                    xml.skipCurrentElement();
                                }
                            }
                        }
                        else {
                            xml.skipCurrentElement();
                        }
                    }
                }
            }
            else if (xml.name() == QStringLiteral("size")) {
                // ── Parse size ─────────────────────────────────────────
                SizeLayer layer;
                layer.label = xml.attributes().value(QStringLiteral("label")).toString();
                layer.sizeValue = xml.attributes().value(QStringLiteral("value")).toDouble();

                while (xml.readNextStartElement()) {
                    if (xml.name() != QStringLiteral("piece")) {
                        xml.skipCurrentElement();
                        continue;
                    }

                    Piece piece;
                    piece.name = xml.attributes().value(QStringLiteral("name")).toString();
                    piece.groupName = xml.attributes().value(QStringLiteral("group")).toString();

                    while (xml.readNextStartElement()) {
                        if (xml.name() == QStringLiteral("contour")) {
                            while (xml.readNextStartElement()) {
                                if (xml.name() == QStringLiteral("path")) {
                                    piece.style = parsePathAttributes(xml);
                                    const QString d = xml.attributes()
                                        .value(QStringLiteral("d")).toString();
                                    Path64 pts = parsePathData(d);
                                    if (!pts.empty()) {
                                        ContourNode node;
                                        node.polyline = pts;
                                        piece.contourNodes.append(node);
                                    }
                                }
                                xml.skipCurrentElement();
                            }
                        }
                        else if (xml.name() == QStringLiteral("internal")) {
                            while (xml.readNextStartElement()) {
                                if (xml.name() == QStringLiteral("path")) {
                                    const QString d = xml.attributes()
                                        .value(QStringLiteral("d")).toString();
                                    Path64 pts = parsePathData(d);
                                    if (!pts.empty()) {
                                        InternalPath ip;
                                        ip.polyline = pts;
                                        piece.internalPaths.append(ip);
                                    }
                                }
                                xml.skipCurrentElement();
                            }
                        }
                        else {
                            xml.skipCurrentElement();
                        }
                    }

                    // Populate GradedSize from contourNodes for SizeRestorer
                    if (!piece.contourNodes.isEmpty()) {
                        GradedSize sz;
                        sz.size = layer.sizeValue;
                        sz.stature = rangeStature;
                        sz.contour = piece.contour(); // uses the inline method
                        piece.sizes.append(sz);
                    }

                    layer.pieces.append(std::move(piece));
                }

                layers.append(std::move(layer));
            }
            else {
                xml.skipCurrentElement();
            }
        }

        // Found and processed the requested range
        return true;
    }

    if (xml.hasError()) {
        qWarning("RabnestReader: XML error: %s", qUtf8Printable(xml.errorString()));
        return false;
    }

    qWarning("RabnestReader: range index %d out of bounds (%d ranges)",
             rangeIndex, m_rangeCount);
    return false;
}
