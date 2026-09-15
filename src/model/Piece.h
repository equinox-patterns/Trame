/************************************************************************
 **
 **  @file   Piece.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Data model for pattern pieces read from a graded SVG.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef PIECE_H
#define PIECE_H

#include <QString>
#include <QVector>

#include <clipper2/clipper.h>

// ── Path visual style ───────────────────────────────────────────────────────

struct PathStyle
{
    QString stroke = QStringLiteral("#000000");
    double  strokeWidth = 1.0;
    QString strokeDasharray;   // SVG dasharray syntax, empty = solid
    QString fill = QStringLiteral("none");
};

// ── Contour node (resolved geometry) ───────────────────────────────────────

struct ContourNode
{
    QString type;       // "Contour", "NodePoint", ...
    Clipper2Lib::Path64 polyline; // interpolated segment (1+ points)
};

// ── Piece (geometry read from a graded SVG) ─────────────────────────────────

struct Piece
{
    QString name;

    QVector<ContourNode> contourNodes;    // ordered contour (closed polygon)

    // Visual style extracted from the SVG (fill/stroke of the first path)
    PathStyle style;

    // Which group this piece belongs to (empty = standalone)
    QString groupName;

    // Identity of the group, when the caller has one. Group NAMES are not unique —
    // nothing in the application enforces it — so grouping on the name alone merges
    // two different groups into a single footprint. Callers that know a stable id
    // set it here; the name is then only what gets displayed.
    // Empty falls back to groupName, which is what an SVG reader can offer.
    QString groupKey;

    // Convenience: full contour as a single Path64
    Clipper2Lib::Path64 contour() const;
};

// ── Size layer (from SVG graded export) ────────────────────────────────────

struct SizeLayer
{
    QString label;     // e.g. "Taille 46" display label
    QVector<Piece> pieces;
};

// ── Inline impl ────────────────────────────────────────────────────────────

inline Clipper2Lib::Path64 Piece::contour() const
{
    Clipper2Lib::Path64 p;
    for (const auto &node : contourNodes) {
        for (const auto &pt : node.polyline) {
            p.push_back(pt);
        }
    }
    return p;
}

#endif // PIECE_H
