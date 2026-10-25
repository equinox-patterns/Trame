/************************************************************************
 **
 **  @file   Footprint.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Multi-size footprint computation via Clipper2 Union.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include "Footprint.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <vector>

namespace Footprint {

Result compute(const QVector<SizeLayer> &layers)
{
    // Collect all contours for each unique piece across all size layers.
    // Key: groupName + "::" + pieceName (or just pieceName if no group).
    QHash<QString, Clipper2Lib::Paths64> pieceContours;
    QHash<QString, QString> pieceDisplayName;

    for (const auto &layer : layers) {
        for (const auto &piece : layer.pieces) {
            // Group on the group's *identity*, never on its name: nothing enforces
            // unique group names, and two groups called the same used to be unioned
            // into one footprint. groupKey is what a caller that has a stable id
            // passes; the name stays the display name.
            // ⚠️ **La clé de la pièce d'abord, son nom seulement à défaut.** `groupKey` est ce que
            // l'appelant qui a une identité stable passe — et une pièce hors groupe en a une aussi
            // (`p<id>` pour un projecteur). L'ancienne lecture ne la regardait que pour un membre de
            // groupe : la clé devenait alors le nom, et un lecteur qui appariait sur la clé de la
            // pièce ne retrouvait plus rien.
            const QString key = !piece.groupKey.isEmpty()
                ? piece.groupKey
                : (piece.groupName.isEmpty() ? piece.name : piece.groupName);

            Clipper2Lib::Path64 c = piece.contour();
            if (c.empty())
                continue;

            // Ensure path is closed for Union
            if (c.front() != c.back())
                c.push_back(c.front());

            auto it = pieceContours.find(key);
            if (it == pieceContours.end()) {
                pieceContours.insert(key, Clipper2Lib::Paths64{c});
                const QString displayName = piece.groupName.isEmpty()
                    ? piece.name
                    : piece.groupName;  // display group name only
                pieceDisplayName.insert(key, displayName);
            } else {
                it->push_back(std::move(c));
            }
        }
    }

    // Compute union for each piece
    Result result;
    result.polygons.reserve(pieceContours.size());
    result.names.reserve(pieceContours.size());
    result.keys.reserve(pieceContours.size());
    result.sizeCounts.reserve(pieceContours.size());

    for (auto it = pieceContours.begin(); it != pieceContours.end(); ++it) {
        const Clipper2Lib::Paths64 &contours = it.value();
        const int count = static_cast<int>(contours.size());

        if (count == 1) {
            // Single size — no union needed
            result.polygons.push_back(contours[0]);
        } else {
            Clipper2Lib::Paths64 unified;
            Clipper2Lib::Clipper64 clipper;
            clipper.AddSubject(contours);
            clipper.Execute(Clipper2Lib::ClipType::Union,
                           Clipper2Lib::FillRule::NonZero, unified);

            if (unified.empty()) {
                // Should not happen with valid piece contours, but guard anyway
                result.polygons.push_back(contours[0]);
            } else {
                // Union should produce exactly one polygon for concentric contours
                result.polygons.push_back(unified[0]);
            }
        }

        result.names.push_back(pieceDisplayName.value(it.key()));
        result.keys.push_back(it.key());
        result.sizeCounts.push_back(count);
    }

    // Deterministic, process-independent ordering.
    // The loop above iterates a QHash, whose iteration order is arbitrary and
    // differs per process — without this sort the engine would receive the same
    // pieces in a different order on every run (equal-area pieces shuffle), so
    // nests are not reproducible. Order: area descending (matches the engine's
    // initial area heuristic), then by piece key so identical-area pieces get a
    // stable, content-based order.
    {
        std::vector<int> idx(result.polygons.size());
        for (size_t i = 0; i < idx.size(); ++i)
            idx[i] = static_cast<int>(i);

        std::sort(idx.begin(), idx.end(), [&result](int a, int b) {
            const double aa = std::fabs(Clipper2Lib::Area(result.polygons[a]));
            const double ab = std::fabs(Clipper2Lib::Area(result.polygons[b]));
            if (aa != ab)
                return aa > ab;   // area descending
            return result.keys[a] < result.keys[b];   // stable tie-break
        });

        Result sorted;
        sorted.polygons.reserve(result.polygons.size());
        sorted.names.reserve(result.names.size());
        sorted.keys.reserve(result.keys.size());
        sorted.sizeCounts.reserve(result.sizeCounts.size());
        for (int i : idx) {
            sorted.polygons.push_back(std::move(result.polygons[i]));
            sorted.names.push_back(std::move(result.names[i]));
            sorted.keys.push_back(std::move(result.keys[i]));
            sorted.sizeCounts.push_back(result.sizeCounts[i]);
        }
        result = std::move(sorted);
    }

    return result;
}

} // namespace Footprint
