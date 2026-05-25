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

namespace Footprint {

Result compute(const QVector<SizeLayer> &layers)
{
    // Collect all contours for each unique piece across all size layers.
    // Key: groupName + "::" + pieceName (or just pieceName if no group).
    QHash<QString, Clipper2Lib::Paths64> pieceContours;
    QHash<QString, QString> pieceDisplayName;

    for (const auto &layer : layers) {
        for (const auto &piece : layer.pieces) {
            const QString key = piece.groupName.isEmpty()
                ? piece.name
                : piece.groupName;  // group all pieces of same group together

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

    return result;
}

} // namespace Footprint
