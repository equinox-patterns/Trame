/************************************************************************
 **
 **  @file   SvgReader.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Reads an Ostara-exported graded SVG file and extracts size
 **          layers with piece contours.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include "SvgReader.h"

#include <QDebug>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QString>

#include <cmath>
#include <cstdint>

static constexpr int kCurveSegments = 32;

// ── SVG path data tokenizer ────────────────────────────────────────────────

namespace {

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

// SVG illustrator namespace URI
static const QString kIllustratorNs = QStringLiteral("http://ns.adobe.com/AdobeIllustrator/10.0/");

/// Returns true if the element is an AI/Inkscape layer (<g i:layer="yes">).
bool isAiLayer(const QDomElement &el)
{
    if (el.tagName() != QStringLiteral("g")) return false;
    if (el.attribute(QStringLiteral("i:layer")) == QStringLiteral("yes")) return true;
    if (el.attributeNS(kIllustratorNs, QStringLiteral("layer")) == QStringLiteral("yes")) return true;
    return false;
}

/// Check if a layer is the "Mise en page" layer (layout/labels only, no contours).
bool isMiseEnPage(const QDomElement &layerEl)
{
    const QString id = layerEl.attribute(QStringLiteral("id"));
    return id.contains(QStringLiteral("Mise"), Qt::CaseInsensitive)
           || id.contains(QStringLiteral("en page"), Qt::CaseInsensitive);
}

} // anonymous namespace

// ── Coordinate helpers ─────────────────────────────────────────────────────

Clipper2Lib::Point64 SvgReader::toPoint64(double x, double y) const
{
    return {
        static_cast<int64_t>(std::llround(x * kScale)),
        static_cast<int64_t>(std::llround(y * kScale))
    };
}

void SvgReader::interpolateBezier(Clipper2Lib::Point64 p1, Clipper2Lib::Point64 cp1,
                                  Clipper2Lib::Point64 cp2, Clipper2Lib::Point64 p4,
                                  Clipper2Lib::Path64 &out) const
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

// ── Path data parser ───────────────────────────────────────────────────────

Clipper2Lib::Path64 SvgReader::parsePathData(const QString &d) const
{
    Clipper2Lib::Path64 path;
    SvgTokenizer tok(d);

    Clipper2Lib::Point64 current{0, 0};
    Clipper2Lib::Point64 startPoint{0, 0};

    while (!tok.atEnd()) {
        const SvgCmd cmd = tok.nextCommand();
        switch (cmd) {
            case SvgCmd::MoveTo: {
                const double x = tok.nextNumber(), y = tok.nextNumber();
                current    = toPoint64(x, y);
                startPoint = current;
                path.push_back(current);
                break;
            }
            case SvgCmd::LineTo: {
                const double x = tok.nextNumber(), y = tok.nextNumber();
                current = toPoint64(x, y);
                path.push_back(current);
                break;
            }
            case SvgCmd::CurveTo: {
                const double x1 = tok.nextNumber(), y1 = tok.nextNumber();
                const double x2 = tok.nextNumber(), y2 = tok.nextNumber();
                const double x3 = tok.nextNumber(), y3 = tok.nextNumber();
                const auto cp1 = toPoint64(x1, y1);
                const auto cp2 = toPoint64(x2, y2);
                const auto end = toPoint64(x3, y3);
                interpolateBezier(current, cp1, cp2, end, path);
                current = end;
                break;
            }
            case SvgCmd::ClosePath:
                current = startPoint;
                break;
            case SvgCmd::None:
                break;
        }
    }
    return path;
}

// ── Helper: extract paths from a piece <g> element ─────────────────────────

static void extractPiecePaths(const QDomElement &pieceG,
                              QVector<Clipper2Lib::Path64> &paths)
{
    const QDomNodeList children = pieceG.childNodes();
    for (int i = 0; i < children.size(); ++i) {
        const QDomElement el = children.at(i).toElement();
        if (el.isNull() || el.tagName() != QStringLiteral("path")) continue;

        const QString d = el.attribute(QStringLiteral("d"));
        if (d.isEmpty()) continue;

        // Defer parsing — SvgReader will handle it
        paths.append(Clipper2Lib::Path64{});
        // Store the raw path for now, parse in the reader
        // (We use a workaround: parse inline in the reader)
    }
}

// ── Main reader ────────────────────────────────────────────────────────────
//
// SVG structure (graded export):
//   <svg>
//     <g id="80-84" i:layer="yes">          ← size layer
//       <g id="Front">                       ← piece (standalone)
//         <path d="..."/>                    ← contour
//         <path d="..."/>                    ← internal path
//       </g>
//       <g id="GroupName">                   ← piece group wrapper
//         <g id="Piece1">...</g>
//         <g id="Piece2">...</g>
//       </g>
//     </g>
//     <g id="Mise en page" i:layer="yes">   ← layout layer (skip)
//     </g>
//   </svg>

bool SvgReader::read(const QString &filePath, QVector<SizeLayer> &layers)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "SvgReader: cannot open" << filePath;
        return false;
    }

    QDomDocument doc;
    const auto result = doc.setContent(&file, QDomDocument::ParseOption::Default);
    if (!result) {
        qWarning() << "SvgReader: XML parse error at line" << result.errorLine
                    << ":" << result.errorMessage;
        file.close();
        return false;
    }
    file.close();

    const QDomElement svg = doc.documentElement();
    if (svg.tagName() != QStringLiteral("svg")) {
        qWarning() << "SvgReader: root element is not <svg>";
        return false;
    }

    // Iterate top-level children: find size layers (g with i:layer="yes")
    const QDomNodeList topChildren = svg.childNodes();
    for (int i = 0; i < topChildren.size(); ++i) {
        const QDomElement el = topChildren.at(i).toElement();
        if (el.isNull()) continue;
        if (!isAiLayer(el)) continue;
        if (isMiseEnPage(el)) continue; // skip "Mise en page"

        SizeLayer layer;
        layer.label = el.attribute(QStringLiteral("id"));

        // Try to parse size value from label (e.g. "80-84" → 80)
        // Also check for <title> child
        const QDomElement titleEl = el.firstChildElement(QStringLiteral("title"));
        if (!titleEl.isNull() && !titleEl.text().isEmpty()) {
            layer.label = titleEl.text();
        }

        // Parse the label to extract a numeric size value (e.g. "80-84" → 80)
        {
            int dash = layer.label.indexOf(QLatin1Char('-'));
            const int enDash = layer.label.indexOf(QChar(0x2013));
            if (dash < 0 || (enDash >= 0 && enDash < dash)) dash = enDash;
            const QString firstNum = (dash > 0) ? layer.label.left(dash) : layer.label;
            bool ok = false;
            const double v = firstNum.toDouble(&ok);
            if (ok) layer.sizeValue = v;
        }

        // Iterate direct children: each is either a piece or a group wrapper
        const QDomNodeList layerChildren = el.childNodes();

        // Helper: process a piece <g> element and fill a Piece struct
        auto processPieceElement = [&](const QDomElement &pieceEl, const QString &groupName) {
            Piece piece;
            piece.name      = pieceEl.attribute(QStringLiteral("id"));
            piece.groupName = groupName;

            // Collect <path> children
            struct RawPath { QString d; };
            QVector<RawPath> rawPaths;
            const QDomNodeList pc = pieceEl.childNodes();
            for (int k = 0; k < pc.size(); ++k) {
                const QDomElement pEl = pc.at(k).toElement();
                if (pEl.isNull() || pEl.tagName() != QStringLiteral("path")) continue;
                const QString d = pEl.attribute(QStringLiteral("d"));
                if (!d.isEmpty()) rawPaths.append({d});
            }

            for (int pi = 0; pi < rawPaths.size(); ++pi) {
                Clipper2Lib::Path64 poly = parsePathData(rawPaths[pi].d);
                if (poly.empty()) continue;

                if (pi == 0) {
                    ContourNode cn;
                    cn.type     = QStringLiteral("Contour");
                    cn.polyline = poly;
                    piece.contourNodes.append(cn);
                } else {
                    InternalPath ip;
                    ip.pathId   = static_cast<quint32>(pi);
                    ip.polyline = poly;
                    piece.internalPaths.append(ip);
                }
            }

            if (!piece.contourNodes.isEmpty()) {
                GradedSize sz;
                sz.contour = piece.contour();
                piece.sizes.append(sz);
                layer.pieces.append(piece);
            }
        };

        for (int j = 0; j < layerChildren.size(); ++j) {
            const QDomElement child = layerChildren.at(j).toElement();
            if (child.isNull() || child.tagName() != QStringLiteral("g")) continue;

            // Skip grain group sub-elements
            const QString childId = child.attribute(QStringLiteral("id"));
            if (childId.endsWith(QStringLiteral("_grain"))) continue;

            // Is this a piece group wrapper or a standalone piece?
            // Check if it contains <g> children → group wrapper
            // Check if it contains <path> children → piece
            bool hasChildGroups = false;
            bool hasChildPaths  = false;
            {
                const QDomNodeList gc = child.childNodes();
                for (int k = 0; k < gc.size(); ++k) {
                    const QDomElement gce = gc.at(k).toElement();
                    if (gce.isNull()) continue;
                    if (gce.tagName() == QStringLiteral("g")) hasChildGroups = true;
                    if (gce.tagName() == QStringLiteral("path")) hasChildPaths = true;
                }
            }

            if (hasChildPaths && !hasChildGroups) {
                // Standalone piece
                processPieceElement(child, QString());
            } else if (hasChildGroups) {
                // Group wrapper: iterate inner piece <g> elements
                const QString groupName = childId;
                const QDomNodeList innerGroups = child.childNodes();
                for (int k = 0; k < innerGroups.size(); ++k) {
                    const QDomElement innerG = innerGroups.at(k).toElement();
                    if (innerG.isNull() || innerG.tagName() != QStringLiteral("g")) continue;
                    if (innerG.attribute(QStringLiteral("id")).endsWith(QStringLiteral("_grain"))) continue;
                    processPieceElement(innerG, groupName);
                }
            }
        }

        if (!layer.pieces.isEmpty()) {
            layers.append(layer);
        }
    }

    return true;
}

bool SvgReader::readSingleLayer(const QString &filePath, SizeLayer &layer, int index)
{
    QVector<SizeLayer> all;
    if (!read(filePath, all)) return false;
    if (all.isEmpty()) return false;

    if (index < 0) index = 0;
    if (index >= all.size()) index = all.size() - 1;

    layer = all.at(index);
    return true;
}
