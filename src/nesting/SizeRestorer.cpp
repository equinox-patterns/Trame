/************************************************************************
 **
 **  @file   SizeRestorer.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Restores individual size contours from placed footprints.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include "SizeRestorer.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace {

constexpr double kScale = 1e6;
constexpr double kPi    = 3.14159265358979323846;

struct BBox
{
    int64_t xMin, yMin, xMax, yMax;
};

BBox bboxFromPath(const Clipper2Lib::Path64 &path)
{
    BBox b{std::numeric_limits<int64_t>::max(),
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

} // anonymous namespace

namespace SizeRestorer {

std::vector<TransformedPiece> restore(
    const std::vector<Placement>             &placements,
    const Footprint::Result                  &fp,
    const QVector<SizeLayer>                 &layers,
    const QHash<QString, QVector<MepPath>>   &mepPaths,
    const QHash<QString, QVector<MepLabel>>  &mepLabels,
    const QHash<QString, QVector<FlatLabel>> &flatLabels)
{
    std::vector<TransformedPiece> result;

    for (size_t pi = 0; pi < placements.size(); ++pi) {
        const Placement &pl = placements[pi];
        if (pi >= fp.keys.size())
            continue;

        const QString &key = fp.keys[pi];

        // Parse key: "groupName::pieceName" (single piece) or "groupName" (whole group)
        QString groupName;
        QString pieceName;
        const int sep = key.indexOf(QStringLiteral("::"));
        if (sep >= 0) {
            groupName = key.left(sep);
            pieceName = key.mid(sep + 2);
        } else {
            // No separator: key is either a standalone piece name or a group name.
            // Check if any piece has this as a groupName
            bool isGroup = false;
            for (const auto &layer : layers) {
                for (const auto &piece : layer.pieces) {
                    if (piece.groupName == key) {
                        isGroup = true;
                        break;
                    }
                }
                if (isGroup) break;
            }
            if (isGroup) {
                groupName = key;
                // pieceName stays empty → match all pieces in group
            } else {
                pieceName = key;
            }
        }

        // Compute footprint bbox for normalisation (same as Engine does)
        const BBox fpBbox = bboxFromPath(fp.polygons[pi]);

        const double rotRad = pl.rotation * kPi / 180.0;
        const double c = std::cos(rotRad);
        const double s = std::sin(rotRad);

        const int64_t px = static_cast<int64_t>(std::round(pl.x * kScale));
        const int64_t py = static_cast<int64_t>(std::round(pl.y * kScale));

        // Helper: apply transform to a path
        auto transformPath = [&](const Clipper2Lib::Path64 &src) -> Clipper2Lib::Path64 {
            if (src.empty()) return {};
            Clipper2Lib::Path64 n;
            n.reserve(src.size());
            for (const auto &pt : src)
                n.push_back({pt.x - fpBbox.xMin, pt.y - fpBbox.yMin});
            Clipper2Lib::Path64 r;
            r.reserve(n.size());
            for (const auto &pt : n)
                r.push_back({static_cast<int64_t>(std::round(pt.x * c - pt.y * s)),
                             static_cast<int64_t>(std::round(pt.x * s + pt.y * c))});
            Clipper2Lib::Path64 t;
            t.reserve(r.size());
            for (const auto &pt : r)
                t.push_back({pt.x + px, pt.y + py});
            return t;
        };

        // Build the list of (groupName, pieceName) pairs to match
        struct PieceMatch { QString gName; QString pName; };
        QVector<PieceMatch> matches;
        if (pieceName.isEmpty()) {
            // Group key: match all pieces with this groupName
            for (const auto &layer : layers) {
                for (const auto &piece : layer.pieces) {
                    if (piece.groupName == groupName) {
                        // Add unique entries
                        PieceMatch pm{groupName, piece.name};
                        bool found2 = false;
                        for (const auto &m : matches) {
                            if (m.pName == pm.pName) { found2 = true; break; }
                        }
                        if (!found2) matches.append(pm);
                    }
                }
            }
        } else {
            matches.append({groupName, pieceName});
        }

        for (const auto &match : matches) {
            TransformedPiece tp;
            tp.pieceName = match.pName;
            tp.groupName = match.gName;

            // Find and transform contours per size layer
            for (const auto &layer : layers) {
                const Piece *found = nullptr;
                for (const auto &piece : layer.pieces) {
                    if (piece.groupName == match.gName && piece.name == match.pName) {
                        found = &piece;
                        break;
                    }
                }
                if (!found || found->sizes.isEmpty())
                    continue;

                const GradedSize &sz = found->sizes.first();
                if (sz.contour.size() < 3)
                    continue;

                TransformedSize ts;
                ts.size    = layer.sizeValue;
                ts.stature = sz.stature;
                ts.style   = found->style;
                ts.contour = transformPath(sz.contour);

                for (const auto &ip : found->internalPaths) {
                    auto tp_ip = transformPath(ip.polyline);
                    if (!tp_ip.empty())
                        ts.internalPaths.append(std::move(tp_ip));
                }

                tp.sizes.append(std::move(ts));
            }

            // MEP paths: lookup by piece name or group::piece
            const QString mepLookupKey = match.gName.isEmpty()
                ? match.pName
                : match.gName + QStringLiteral("::") + match.pName;
            auto itMep = mepPaths.find(mepLookupKey);
            if (itMep == mepPaths.end())
                itMep = mepPaths.find(match.pName);
            if (itMep != mepPaths.end()) {
                for (const auto &mp : itMep.value()) {
                    MepPath transformed;
                    transformed.style = mp.style;
                    transformed.polyline = transformPath(mp.polyline);
                    if (!transformed.polyline.empty())
                        tp.mepPaths.append(std::move(transformed));
                }
            }

            // MEP labels: try group::piece, then piece, then group alone
            auto itMepLabel = mepLabels.find(mepLookupKey);
            if (itMepLabel == mepLabels.end())
                itMepLabel = mepLabels.find(match.pName);
            if (itMepLabel == mepLabels.end())
                itMepLabel = mepLabels.find(match.gName);
            if (itMepLabel != mepLabels.end()) {
                for (const auto &lbl : itMepLabel.value()) {
                    MepLabel transformed = lbl;
                    if (lbl.type == MepLabel::Text || lbl.type == MepLabel::RasterBlock) {
                        const int64_t lx = static_cast<int64_t>(std::round(lbl.x * kScale));
                        const int64_t ly = static_cast<int64_t>(std::round(lbl.y * kScale));
                        const int64_t nx = lx - fpBbox.xMin;
                        const int64_t ny = ly - fpBbox.yMin;
                        const int64_t rx = static_cast<int64_t>(std::round(nx * c - ny * s));
                        const int64_t ry = static_cast<int64_t>(std::round(nx * s + ny * c));
                        transformed.x = (rx + px) / kScale;
                        transformed.y = (ry + py) / kScale;
                        transformed.rotation = pl.rotation;
                    } else if (lbl.type == MepLabel::SvgFragment) {
                    // Compute absolute position from the logo's <g> transform
                    // lbl.transform is the original SVG attribute: "translate(lx,ly) scale(sx,sy)"
                    double logoX = 0, logoY = 0, logoSx = 1, logoSy = 1;
                    if (!lbl.transform.isEmpty()) {
                        // Simple parser for "translate(x,y) scale(sx,sy)"
                        const QString tx = lbl.transform;
                        const int tp = tx.indexOf(QStringLiteral("translate("));
                        const int sp = tx.indexOf(QStringLiteral("scale("));
                        if (tp >= 0) {
                            const int tEnd = tx.indexOf(QLatin1Char(')'), tp + 10);
                            if (tEnd > tp) {
                                const QString args = tx.mid(tp + 10, tEnd - tp - 10);
                                const auto parts = args.split(QLatin1Char(','));
                                if (parts.size() >= 2) {
                                    logoX = parts[0].trimmed().toDouble();
                                    logoY = parts[1].trimmed().toDouble();
                                }
                            }
                        }
                        if (sp >= 0) {
                            const int sEnd = tx.indexOf(QLatin1Char(')'), sp + 6);
                            if (sEnd > sp) {
                                const QString args = tx.mid(sp + 6, sEnd - sp - 6);
                                const auto parts = args.split(QLatin1Char(','));
                                logoSx = parts[0].trimmed().toDouble();
                                logoSy = (parts.size() >= 2) ? parts[1].trimmed().toDouble() : logoSx;
                            }
                        }
                    }

                    // Compute absolute position: normalise → rotate → translate
                    const int64_t lx = static_cast<int64_t>(std::round(logoX * kScale));
                    const int64_t ly = static_cast<int64_t>(std::round(logoY * kScale));
                    const int64_t nx = lx - fpBbox.xMin;
                    const int64_t ny = ly - fpBbox.yMin;
                    const int64_t rx = static_cast<int64_t>(std::round(nx * c - ny * s));
                    const int64_t ry = static_cast<int64_t>(std::round(nx * s + ny * c));
                    transformed.x = (rx + px) / kScale;
                    transformed.y = (ry + py) / kScale;
                    transformed.rotation = pl.rotation;
                    transformed.scaleX = logoSx;
                    transformed.scaleY = logoSy;

                    // Also store combined transform for SVG debug output
                    const double txPx = static_cast<double>(px) / kScale;
                    const double tyPx = static_cast<double>(py) / kScale;
                    const double bMinX = static_cast<double>(fpBbox.xMin) / kScale;
                    const double bMinY = static_cast<double>(fpBbox.yMin) / kScale;
                    const QString placementTx = QStringLiteral("translate(%1 %2) rotate(%3) translate(%4 %5)")
                        .arg(txPx, 0, 'f', 3)
                        .arg(tyPx, 0, 'f', 3)
                        .arg(pl.rotation, 0, 'f', 3)
                        .arg(-bMinX, 0, 'f', 3)
                        .arg(-bMinY, 0, 'f', 3);
                    if (lbl.transform.isEmpty())
                        transformed.transform = placementTx;
                    else
                        transformed.transform = placementTx + QStringLiteral(" ") + lbl.transform;
                }
                tp.mepLabels.append(std::move(transformed));
            }
        }

        // Transform flat labels from .rabnest for this piece
        const QString flatLookupKey = match.gName.isEmpty()
            ? match.pName
            : match.gName + QStringLiteral("::") + match.pName;
        auto itFlat = flatLabels.find(flatLookupKey);
        if (itFlat == flatLabels.end())
            itFlat = flatLabels.find(match.pName);
        if (itFlat == flatLabels.end())
            itFlat = flatLabels.find(match.gName);
        if (itFlat != flatLabels.end()) {
            for (const auto &lbl : itFlat.value()) {
                FlatLabel transformed = lbl;
                // lbl.x/y are already absolute positions (scenePos from ExportToRabnest).
                // Contours are also at absolute positions (not shifted by fpBbox).
                // Apply nesting placement: rotation around footprint center + translation.
                const double cx = (fpBbox.xMin + fpBbox.xMax) / (2.0 * kScale);
                const double cy = (fpBbox.yMin + fpBbox.yMax) / (2.0 * kScale);
                const double dx = lbl.x - cx;
                const double dy = lbl.y - cy;
                transformed.x = cx + dx * c - dy * s + px / kScale;
                transformed.y = cy + dx * s + dy * c + py / kScale;
                transformed.rotation += pl.rotation; // add placement rotation
                tp.flatLabels.append(std::move(transformed));
            }
        }

            if (!tp.sizes.isEmpty())
                result.push_back(std::move(tp));
        } // for (match : matches)
    } // for (pi : placements)

    return result;
}

} // namespace SizeRestorer
