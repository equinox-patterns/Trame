/************************************************************************
 **
 **  @file   NestedSvgWriter.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   26/05/2026
 **
 **  @brief  Applies nesting placements as SVG transforms on a graded SVG DOM.
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

#include "NestedSvgWriter.h"
#include "GradedSvgContract.h"

#include <QDebug>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNodeList>
#include <QFile>
#include <QTextStream>

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

constexpr double kScale  = 1e6;
constexpr double kMmToPx = 96.0 / 25.4;

/// Compute bounding box of a Clipper2 path.
struct IBox { int64_t xMin, yMin, xMax, yMax; };

IBox pathBbox(const Clipper2Lib::Path64 &path)
{
    IBox b{std::numeric_limits<int64_t>::max(),
           std::numeric_limits<int64_t>::max(),
           std::numeric_limits<int64_t>::min(),
           std::numeric_limits<int64_t>::min()};
    for (const auto &p : path) {
        if (p.x < b.xMin) b.xMin = p.x;
        if (p.y < b.yMin) b.yMin = p.y;
        if (p.x > b.xMax) b.xMax = p.x;
        if (p.y > b.yMax) b.yMax = p.y;
    }
    return b;
}

/// Build SVG transform string: translate(px,py) rotate(deg) translate(-ox,-oy)
QString makeTransform(double px, double py, double deg, double ox, double oy)
{
    QString s;
    QTextStream ts(&s);
    ts << "translate(" << px << ',' << py << ") rotate(" << deg
       << ") translate(" << -ox << ',' << -oy << ')';
    return s;
}

/// Check whether a top-level <g> is the decor layer of the graded SVG —
/// identified by the ostara:layout backend marker (contract with the Ostara writer).
bool isLayoutLayer(const QDomElement &layerEl)
{
    return layerEl.attribute(QLatin1String(kGradedLayoutLayerAttr))
               == QLatin1String(kGradedLayoutLayerMarker);
}

/// Remove a trailing "_<digits>" dedup suffix added by the Ostara writer's
/// uniqueId() (ogradedsvgwriter.cpp) when an id repeats inside a layer.
/// Returns the base id, or an empty string when there is no such suffix.
QString stripDedupSuffix(const QString &id)
{
    const int us = id.lastIndexOf(QLatin1Char('_'));
    if (us <= 0) return {};
    for (int c = us + 1; c < id.size(); ++c) {
        if (!id.at(c).isDigit()) return {};
    }
    return id.left(us);
}

bool isAiLayer(const QDomElement &el)
{
    if (el.tagName() != QStringLiteral("g")) return false;
    if (el.attribute(QStringLiteral("i:layer")) == QStringLiteral("yes")) return true;
    if (el.attributeNS(QStringLiteral("http://ns.adobe.com/AdobeIllustrator/10.0/"),
                       QStringLiteral("layer")) == QStringLiteral("yes")) return true;
    return false;
}

} // anonymous namespace

bool NestedSvgWriter::write(const QString              &inputSvg,
                             const QString              &outputSvg,
                             const std::vector<Placement> &placements,
                             const Footprint::Result    &fp,
                             double                     binWidthMm,
                             double                     usedHeightMm,
                             double                     obstacleWidthMm,
                             double                     obstacleHeightMm)
{
    // ── 1. Parse input SVG ──────────────────────────────────────────────────
    QFile inFile(inputSvg);
    if (!inFile.open(QIODevice::ReadOnly)) {
        qWarning() << "NestedSvgWriter: cannot open" << inputSvg;
        return false;
    }

    QDomDocument doc;
    const auto parseResult = doc.setContent(&inFile, QDomDocument::ParseOption::Default);
    inFile.close();
    if (!parseResult) {
        qWarning() << "NestedSvgWriter: XML parse error at line"
                    << parseResult.errorLine << ":" << parseResult.errorMessage;
        return false;
    }

    QDomElement svgRoot = doc.documentElement();
    if (svgRoot.tagName() != QStringLiteral("svg")) {
        qWarning() << "NestedSvgWriter: root is not <svg>";
        return false;
    }

    // ── 2. Build placement map: key → PieceTransform ────────────────────────
    QHash<QString, PieceTransform> xforms;
    for (size_t i = 0; i < placements.size() && i < fp.keys.size(); ++i) {
        const Placement &pl = placements[i];
        const IBox bb = pathBbox(fp.polygons[i]);

        PieceTransform pt;
        pt.px       = pl.x;
        pt.py       = pl.y;
        pt.rotation = pl.rotation;
        pt.origX    = bb.xMin / kScale;
        pt.origY    = bb.yMin / kScale;
        xforms.insert(fp.keys[i], pt);
    }

    // Also insert by piece name alone (for MEP element lookup)
    for (const auto &key : fp.keys) {
        const int sep = key.indexOf(QStringLiteral("::"));
        if (sep >= 0) {
            const QString pieceOnly = key.mid(sep + 2);
            if (!xforms.contains(pieceOnly))
                xforms.insert(pieceOnly, xforms.value(key));
        }
    }

    // ── 3. Helper: apply transform to an element ────────────────────────────
    auto applyTransform = [](QDomElement &el, const PieceTransform &pt) {
        const QString tx = makeTransform(pt.px, pt.py, pt.rotation, pt.origX, pt.origY);
        el.setAttribute(QStringLiteral("transform"), tx);
    };

    // ── 4. Walk size layers and apply transforms to piece groups ────────────
    //
    // Also build a piece-to-group mapping for the MEP layer (step 5).
    // Grouped pieces have their MEP wrappers keyed by individual piece name,
    // but xforms uses the group name. This mapping bridges the two.
    QHash<QString, QString> pieceToGroup;

    const QDomNodeList topChildren = svgRoot.childNodes();
    for (int i = 0; i < topChildren.size(); ++i) {
        QDomElement layerEl = topChildren.at(i).toElement();
        if (layerEl.isNull() || !isAiLayer(layerEl)) continue;
        if (isLayoutLayer(layerEl)) continue;

        // Walk children: direct pieces or group wrappers
        const QDomNodeList layerChildren = layerEl.childNodes();
        for (int j = 0; j < layerChildren.size(); ++j) {
            QDomElement child = layerChildren.at(j).toElement();
            if (child.isNull() || child.tagName() != QStringLiteral("g")) continue;

            // Is this a group wrapper or a standalone piece?
            bool hasChildGroups = false;
            {
                const QDomNodeList gc = child.childNodes();
                for (int k = 0; k < gc.size(); ++k) {
                    const QDomElement gce = gc.at(k).toElement();
                    if (!gce.isNull() && gce.tagName() == QStringLiteral("g")) {
                        hasChildGroups = true;
                        break;
                    }
                }
            }

            if (hasChildGroups) {
                // Footprint keys groups by groupName alone — look it up once
                const QString groupName = child.attribute(QStringLiteral("id"));
                const auto itGroup = xforms.find(groupName);
                const QDomNodeList inner = child.childNodes();
                for (int k = 0; k < inner.size(); ++k) {
                    QDomElement pieceEl = inner.at(k).toElement();
                    if (pieceEl.isNull() || pieceEl.tagName() != QStringLiteral("g")) continue;

                    // Save mapping for MEP layer lookup
                    pieceToGroup.insert(pieceEl.attribute(QStringLiteral("id")), groupName);

                    if (itGroup != xforms.end()) {
                        applyTransform(pieceEl, itGroup.value());
                    }
                }
            } else {
                // Standalone piece
                const QString key = child.attribute(QStringLiteral("id"));
                auto it = xforms.find(key);
                if (it == xforms.end()) {
                    // Try with empty group name prefix
                    for (const auto &k : fp.keys) {
                        if (k == key || k.endsWith(QStringLiteral("::") + key)) {
                            it = xforms.find(k);
                            break;
                        }
                    }
                }
                if (it != xforms.end()) {
                    applyTransform(child, it.value());
                }
            }
        }
    }

    // ── 5. Walk the decor layer and apply transforms ──────────────
    // It contains one wrapper per piece/group (<g id="Dos" i:layer="yes">)
    // holding the decor (grainline/fold, info blocks, annotations). Apply the
    // transform ONLY to the wrappers — children inherit it, avoiding double
    // transformation and keeping each block's own rotation intact.
    for (int i = 0; i < topChildren.size(); ++i) {
        QDomElement layerEl = topChildren.at(i).toElement();
        if (layerEl.isNull() || !isAiLayer(layerEl)) continue;
        if (!isLayoutLayer(layerEl)) continue;

        const QDomNodeList mepChildren = layerEl.childNodes();
        for (int j = 0; j < mepChildren.size(); ++j) {
            QDomElement el = mepChildren.at(j).toElement();
            if (el.isNull() || el.tagName() != QStringLiteral("g")) continue;
            if (!isAiLayer(el)) continue; // only transform i:layer wrappers

            const QString pieceName = el.attribute(QStringLiteral("id"));
            auto it = xforms.find(pieceName);
            if (it == xforms.end()) {
                // Try piece→group mapping (for grouped pieces whose decor
                // wrapper is keyed by individual piece name, not group name)
                const QString group = pieceToGroup.value(pieceName);
                if (!group.isEmpty())
                    it = xforms.find(group);
            }
            if (it == xforms.end()) {
                for (const auto &k : fp.keys) {
                    if (k.endsWith(QStringLiteral("::") + pieceName)) {
                        it = xforms.find(k);
                        break;
                    }
                }
            }
            if (it == xforms.end()) {
                // Ostara's writer dedups repeated ids with a numeric suffix
                // ("_1", "_2", …) via its uniqueId() — retry without it.
                const QString stripped = stripDedupSuffix(pieceName);
                if (!stripped.isEmpty()) {
                    it = xforms.find(stripped);
                    if (it == xforms.end()) {
                        const QString group = pieceToGroup.value(stripped);
                        if (!group.isEmpty())
                            it = xforms.find(group);
                    }
                }
            }
            if (it != xforms.end()) {
                applyTransform(el, it.value());
            }
        }
        break; // only one MEP layer
    }

    // ── 6. Update page dimensions ────────────────────────────────────────────
    const double binWidthPx  = binWidthMm * kMmToPx;
    const double binHeightPx = usedHeightMm * kMmToPx;

    svgRoot.setAttribute(QStringLiteral("width"),
                         QStringLiteral("%1mm").arg(binWidthMm, 0, 'f', 4));
    svgRoot.setAttribute(QStringLiteral("height"),
                         QStringLiteral("%1mm").arg(usedHeightMm, 0, 'f', 4));
    svgRoot.setAttribute(QStringLiteral("viewBox"),
        QStringLiteral("0 0 %1 %2").arg(binWidthPx, 0, 'f', 1).arg(binHeightPx, 0, 'f', 1));

    // ── 7. Serialize ─────────────────────────────────────────────────────────
    QFile outFile(outputSvg);
    if (!outFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "NestedSvgWriter: cannot write" << outputSvg;
        return false;
    }

    QTextStream ts(&outFile);
    doc.save(ts, 0);
    outFile.close();

    return true;
}
