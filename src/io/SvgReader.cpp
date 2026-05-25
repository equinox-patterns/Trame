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
//
// Splits the SVG path data at each M command. The first sub-path is usually
// the main contour; additional M-sub-paths are passmarks grafted onto it.

QVector<Clipper2Lib::Path64> SvgReader::parsePathData(const QString &d) const
{
    QVector<Clipper2Lib::Path64> subPaths;
    SvgTokenizer tok(d);

    Clipper2Lib::Point64 current{0, 0};
    Clipper2Lib::Point64 startPoint{0, 0};

    while (!tok.atEnd()) {
        const SvgCmd cmd = tok.nextCommand();
        switch (cmd) {
            case SvgCmd::MoveTo: {
                // Start a new sub-path
                const double x = tok.nextNumber(), y = tok.nextNumber();
                current    = toPoint64(x, y);
                startPoint = current;
                subPaths.append(Clipper2Lib::Path64{current});
                break;
            }
            case SvgCmd::LineTo: {
                const double x = tok.nextNumber(), y = tok.nextNumber();
                current = toPoint64(x, y);
                if (!subPaths.isEmpty()) subPaths.last().push_back(current);
                break;
            }
            case SvgCmd::CurveTo: {
                const double x1 = tok.nextNumber(), y1 = tok.nextNumber();
                const double x2 = tok.nextNumber(), y2 = tok.nextNumber();
                const double x3 = tok.nextNumber(), y3 = tok.nextNumber();
                const auto cp1 = toPoint64(x1, y1);
                const auto cp2 = toPoint64(x2, y2);
                const auto end = toPoint64(x3, y3);
                if (!subPaths.isEmpty())
                    interpolateBezier(current, cp1, cp2, end, subPaths.last());
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
    return subPaths;
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

    // Helper: extract style from a <path> element (shared by size layers and MEP)
    auto extractPathStyle = [](const QDomElement &pathEl) -> PathStyle {
        PathStyle s;

        auto readAttr = [&](const QString &name) -> QString {
            QString v = pathEl.attribute(name);
            if (!v.isEmpty()) return v;

            const QString css = pathEl.attribute(QStringLiteral("style"));
            if (!css.isEmpty()) {
                const QStringList parts = css.split(QLatin1Char(';'));
                for (const auto &part : parts) {
                    const int colon = part.indexOf(QLatin1Char(':'));
                    if (colon < 0) continue;
                    const QString key = part.left(colon).trimmed();
                    if (key == name) {
                        return part.mid(colon + 1).trimmed();
                    }
                }
            }
            return {};
        };

        const QString f = readAttr(QStringLiteral("fill"));
        if (!f.isEmpty())
            s.fill = f;

        const QString st = readAttr(QStringLiteral("stroke"));
        if (!st.isEmpty())
            s.stroke = st;

        const QString sw = readAttr(QStringLiteral("stroke-width"));
        if (!sw.isEmpty())
            s.strokeWidth = sw.toDouble();

        const QString sda = readAttr(QStringLiteral("stroke-dasharray"));
        if (!sda.isEmpty())
            s.strokeDasharray = sda;

        return s;
    };

    // Iterate top-level children: find size layers (g with i:layer="yes")
    const QDomNodeList topChildren = svg.childNodes();
    QDomElement mepLayerElement; // preserve MEP layer for later processing
    for (int i = 0; i < topChildren.size(); ++i) {
        const QDomElement el = topChildren.at(i).toElement();
        if (el.isNull()) continue;
        if (!isAiLayer(el)) continue;
        if (isMiseEnPage(el)) {
            mepLayerElement = el; // save for later, don't skip
            continue;
        }

        SizeLayer layer;
        layer.label = el.attribute(QStringLiteral("id"));

        // Try to parse size value from label (e.g. "80-84" → 80)
        // Also check for <title> child
        const QDomElement titleEl = el.firstChildElement(QStringLiteral("title"));
        if (!titleEl.isNull() && !titleEl.text().isEmpty()) {
            layer.label = titleEl.text();
        }

        // Extract a numeric size value from the label.
        // Handles: "80-84", "Size 46", "Taille 38", "38", "46 – 50"
        {
            double v = 0;
            bool ok = false;

            // First try: label contains a dash/en-dash range (e.g. "80-84")
            int dash = layer.label.indexOf(QLatin1Char('-'));
            const int enDash = layer.label.indexOf(QChar(0x2013));
            if (dash < 0 || (enDash >= 0 && enDash < dash)) dash = enDash;
            if (dash > 0) {
                v = layer.label.left(dash).trimmed().toDouble(&ok);
            }

            // Second try: find the first number in the string
            if (!ok) {
                int numStart = -1;
                int numEnd   = -1;
                for (int i = 0; i < layer.label.size(); ++i) {
                    if (layer.label[i].isDigit() || layer.label[i] == QLatin1Char('.')) {
                        if (numStart < 0) numStart = i;
                        numEnd = i + 1;
                    } else if (numStart >= 0) {
                        break;
                    }
                }
                if (numStart >= 0) {
                    v = QStringView(layer.label.constData() + numStart, numEnd - numStart).toDouble(&ok);
                }
            }

            if (ok) layer.sizeValue = v;
        }

        // Iterate direct children: each is either a piece or a group wrapper
        const QDomNodeList layerChildren = el.childNodes();

        // Helper: process a piece <g> element and fill a Piece struct
        auto processPieceElement = [&](const QDomElement &pieceEl, const QString &groupName) {
            Piece piece;
            piece.name      = pieceEl.attribute(QStringLiteral("id"));
            piece.groupName = groupName;

            // Collect all sub-paths from all <path> children.
            // Passmarks are grafted onto the contour as separate M-sub-paths,
            // so we split at each M and then classify by area.
            struct SubPath { Clipper2Lib::Path64 poly; };
            QVector<SubPath> allSubPaths;
            bool styleExtracted = false;
            const QDomNodeList pc = pieceEl.childNodes();
            for (int k = 0; k < pc.size(); ++k) {
                const QDomElement pEl = pc.at(k).toElement();
                if (pEl.isNull() || pEl.tagName() != QStringLiteral("path")) continue;
                const QString d = pEl.attribute(QStringLiteral("d"));
                if (d.isEmpty()) continue;

                if (!styleExtracted) {
                    piece.style = extractPathStyle(pEl);
                    styleExtracted = true;
                }

                const auto subPaths = parsePathData(d);
                for (const auto &sp : subPaths) {
                    if (!sp.empty()) allSubPaths.append({sp});
                }
            }

            if (allSubPaths.isEmpty()) return;

            // Find the largest sub-path by Area → main contour
            // Passmarks are small lines (Area ≈ 0), contour is a large polygon.
            int largestIdx = 0;
            double largestArea = -1;
            for (int i = 0; i < allSubPaths.size(); ++i) {
                const double area = std::abs(Clipper2Lib::Area(allSubPaths[i].poly));
                if (area > largestArea) {
                    largestArea = area;
                    largestIdx = i;
                }
            }

            // Main contour
            {
                ContourNode cn;
                cn.type     = QStringLiteral("Contour");
                cn.polyline = allSubPaths[largestIdx].poly;
                piece.contourNodes.append(cn);
            }

            // All other sub-paths → internal paths (passmarks, grainlines, etc.)
            for (int i = 0; i < allSubPaths.size(); ++i) {
                if (i == largestIdx) continue;
                InternalPath ip;
                ip.pathId   = static_cast<quint32>(i);
                ip.polyline = allSubPaths[i].poly;
                piece.internalPaths.append(ip);
            }

            GradedSize sz;
            sz.contour = piece.contour();
            piece.sizes.append(sz);
            layer.pieces.append(piece);
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

    // Extract <style> elements from the original SVG for inclusion in label wrappers
    QString globalStyles;
    {
        const QDomNodeList styleNodes = svg.elementsByTagName(QStringLiteral("style"));
        for (int i = 0; i < styleNodes.size(); ++i) {
            const QDomElement styleEl = styleNodes.at(i).toElement();
            if (!styleEl.isNull()) {
                QString s;
                QTextStream ts(&s);
                styleEl.save(ts, 0);
                globalStyles += s + QStringLiteral("\n");
            }
        }
    }

    // Process Mise-en-page layer: extract grainline paths and label text/logos per piece
    if (!mepLayerElement.isNull()) {
        // Helper: recursively collect all <path> descendants from a DOM element
        struct PathCollect {
            QVector<Clipper2Lib::Path64> polys;
            PathStyle style;
        };
        auto collectPaths = [&](auto &self, const QDomElement &parent,
                                 QVector<PathCollect> &out) -> void {
            const QDomNodeList children = parent.childNodes();
            for (int i = 0; i < children.size(); ++i) {
                const QDomElement child = children.at(i).toElement();
                if (child.isNull()) continue;
                if (child.tagName() == QStringLiteral("path")) {
                    const QString d = child.attribute(QStringLiteral("d"));
                    if (d.isEmpty()) continue;
                    PathCollect pc;
                    pc.style = extractPathStyle(child);
                    const auto subPaths = parsePathData(d);
                    for (const auto &sp : subPaths) {
                        if (!sp.empty())
                            pc.polys.append(sp);
                    }
                    if (!pc.polys.isEmpty())
                        out.append(std::move(pc));
                } else if (child.tagName() == QStringLiteral("g")) {
                    self(self, child, out);
                }
            }
        };

        // Helper: serialize a DOM element to an SVG string
        auto serializeElement = [](const QDomElement &el) -> QString {
            QString s;
            QTextStream ts(&s);
            el.save(ts, 0);
            return s;
        };

        // Helper: extract labels (text + SVG fragments) from a *_label_* group
        auto extractLabels = [&](auto &self, const QDomElement &parent,
                                  QVector<MepLabel> &out) -> void {
            const QDomNodeList children = parent.childNodes();
            for (int i = 0; i < children.size(); ++i) {
                const QDomElement child = children.at(i).toElement();
                if (child.isNull()) continue;
                if (child.tagName() == QStringLiteral("text")) {
                    MepLabel lbl;
                    lbl.type = MepLabel::Text;
                    lbl.text = child.text();
                    lbl.fontFamily = child.attribute(QStringLiteral("font-family"));
                    lbl.fontSize   = child.attribute(QStringLiteral("font-size"))
                                        .replace(QStringLiteral("pt"), QString()).toDouble();
                    lbl.fontWeight = child.attribute(QStringLiteral("font-weight"));
                    const QString fillAttr = child.attribute(QStringLiteral("fill"));
                    if (!fillAttr.isEmpty())
                        lbl.fill = fillAttr;
                    lbl.x = child.attribute(QStringLiteral("x")).toDouble();
                    lbl.y = child.attribute(QStringLiteral("y")).toDouble();
                    // Handle transform on the text element itself
                    const QString tx = child.attribute(QStringLiteral("transform"));
                    if (!tx.isEmpty())
                        lbl.transform = tx;
                    if (!lbl.text.isEmpty())
                        out.append(std::move(lbl));
                } else if (child.tagName() == QStringLiteral("g")) {
                    // Check if this is a logo group (has defs/style/path children)
                    const QString cid = child.attribute(QStringLiteral("id"));
                    const bool isLabelNested = cid.contains(QStringLiteral("_label_"));
                    // Recurse into non-label groups (logos) or handle directly
                    const QDomNodeList gc = child.childNodes();
                    bool hasDefs = false;
                    for (int j = 0; j < gc.size(); ++j) {
                        const QDomElement gce = gc.at(j).toElement();
                        if (!gce.isNull() && gce.tagName() == QStringLiteral("defs"))
                            hasDefs = true;
                    }
                    if (hasDefs || isLabelNested) {
                        // Serialize as SVG fragment
                        MepLabel lbl;
                        lbl.type = MepLabel::SvgFragment;
                        lbl.svgContent = serializeElement(child);
                        const QString tx = child.attribute(QStringLiteral("transform"));
                        if (!tx.isEmpty())
                            lbl.transform = tx;
                        out.append(std::move(lbl));
                    } else {
                        self(self, child, out);
                    }
                }
            }
        };

        const QDomNodeList mepChildren = mepLayerElement.childNodes();
        // Helper: extract a reference position from a label group.
        // Uses the first <text> element if present; otherwise parses
        // translate(x,y) from the first child <g> transform attribute.
        auto labelRefPos = [](const QDomElement &labelGroup) -> QPair<double, double> {
            const QDomNodeList texts = labelGroup.elementsByTagName(QStringLiteral("text"));
            if (!texts.isEmpty()) {
                const QDomElement firstText = texts.at(0).toElement();
                return {firstText.attribute(QStringLiteral("x")).toDouble(),
                        firstText.attribute(QStringLiteral("y")).toDouble()};
            }
            // No text — try the first child <g> with a translate() transform
            const QDomNodeList children = labelGroup.childNodes();
            for (int i = 0; i < children.size(); ++i) {
                const QDomElement child = children.at(i).toElement();
                if (child.isNull() || child.tagName() != QStringLiteral("g")) continue;
                const QString tx = child.attribute(QStringLiteral("transform"));
                const int tp = tx.indexOf(QStringLiteral("translate("));
                if (tp < 0) continue;
                const int tEnd = tx.indexOf(QLatin1Char(')'), tp + 10);
                if (tEnd < 0) continue;
                const QStringList parts = tx.mid(tp + 10, tEnd - tp - 10).split(QLatin1Char(','));
                if (parts.size() >= 2)
                    return {parts[0].trimmed().toDouble(), parts[1].trimmed().toDouble()};
                break;
            }
            return {0.0, 0.0};
        };

        // First pass: derive pieceKey for each direct child (flat structure)
        // and handle _grain/_label groups that are direct children of Mise_en_page.
        QHash<QString, int> pieceBlockCounters; // per-pieceKey block id counter
        for (int i = 0; i < mepChildren.size(); ++i) {
            const QDomElement pieceG = mepChildren.at(i).toElement();
            if (pieceG.isNull() || pieceG.tagName() != QStringLiteral("g")) continue;

            const QString pieceId = pieceG.attribute(QStringLiteral("id"));
            // Remove suffix ("_grain", "_label_1", etc.) to get the canonical piece key
            QString pieceKey = pieceId;
            const int usIdx = pieceKey.lastIndexOf(QLatin1Char('_'));
            if (usIdx >= 0) {
                const QString suffix = pieceKey.mid(usIdx);
                if (suffix == QStringLiteral("_grain") || suffix.startsWith(QStringLiteral("_label_")))
                    pieceKey = pieceKey.left(usIdx);
            }

            // Handle flat structure: pieceG IS a _grain or _label group
            if (pieceId.endsWith(QStringLiteral("_grain"))) {
                QVector<PathCollect> collected;
                collectPaths(collectPaths, pieceG, collected);
                auto &vec = mepPaths[pieceKey];
                for (auto &pc : collected) {
                    for (auto &poly : pc.polys) {
                        MepPath mp;
                        mp.polyline = std::move(poly);
                        mp.style = pc.style;
                        vec.append(std::move(mp));
                    }
                }
            } else if (pieceId.contains(QStringLiteral("_label"))) {
                int &blockCounter = pieceBlockCounters[pieceKey];
                const int currentBlock = blockCounter++;

                const auto [refX, refY] = labelRefPos(pieceG);

                const QString rawXml = serializeElement(pieceG);
                const QString wrapped = QStringLiteral(
                    "<svg xmlns=\"http://www.w3.org/2000/svg\">\n%1%2\n</svg>").arg(globalStyles, rawXml);

                MepLabel block;
                block.type = MepLabel::RasterBlock;
                block.svgContent = wrapped;
                block.x = refX;
                block.y = refY;
                block.origX = refX;
                block.origY = refY;
                block.blockId = currentBlock;
                block.elementId = pieceId;
                mepLabels[pieceKey].append(block);
            }

            // Also process children for wrapped structure
            // (pieceG may contain nested _grain / _label child groups)
            const QDomNodeList pcChildren = pieceG.childNodes();
            for (int j = 0; j < pcChildren.size(); ++j) {
                const QDomElement childG = pcChildren.at(j).toElement();
                if (childG.isNull() || childG.tagName() != QStringLiteral("g")) continue;
                const QString cid = childG.attribute(QStringLiteral("id"));

                if (cid.endsWith(QStringLiteral("_grain"))) {
                    QVector<PathCollect> collected;
                    collectPaths(collectPaths, childG, collected);
                    auto &vec = mepPaths[pieceKey];
                    for (auto &pc : collected) {
                        for (auto &poly : pc.polys) {
                            MepPath mp;
                            mp.polyline = std::move(poly);
                            mp.style = pc.style;
                            vec.append(std::move(mp));
                        }
                    }
                } else if (cid.contains(QStringLiteral("_label"))) {
                    int &blockCounter = pieceBlockCounters[pieceKey];
                    const int currentBlock = blockCounter++;

                const auto [refX, refY] = labelRefPos(childG);

                    const QString rawXml = serializeElement(childG);
                    const QString wrapped = QStringLiteral(
                        "<svg xmlns=\"http://www.w3.org/2000/svg\">\n%1%2\n</svg>").arg(globalStyles, rawXml);

                    MepLabel block;
                    block.type = MepLabel::RasterBlock;
                    block.svgContent = wrapped;
                    block.x = refX;
                    block.y = refY;
                    block.origX = refX;
                    block.origY = refY;
                    block.blockId = currentBlock;
                    mepLabels[pieceKey].append(block);
                }
            }
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
