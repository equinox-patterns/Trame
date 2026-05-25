/************************************************************************
 **
 **  @file   Piece.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Data model for a complete pattern piece with all its elements.
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

#include "GradedSize.h"

// ── Path visual style ───────────────────────────────────────────────────────

struct PathStyle
{
    QString stroke = QStringLiteral("#000000");
    double  strokeWidth = 1.0;
    QString strokeDasharray;   // SVG dasharray syntax, empty = solid
    QString fill = QStringLiteral("none");
};

// ── Mise-en-page path (grainline) ────────────────────────────────────────────

struct MepPath
{
    Clipper2Lib::Path64 polyline;
    PathStyle           style;
};

// ── Label type ───────────────────────────────────────────────────────────────

enum class LabelType { Text, Logo };

// ── Mise-en-page label (deprecated — for SVG import only) ────────────────────

struct MepLabel
{
    enum Type { Text, RasterBlock, SvgFragment };
    Type    type = Text;
    QString pieceName;
    QString text;
    QString fontFamily;
    double  fontSize = 12;
    QString fontWeight = QStringLiteral("normal");
    QString fill = QStringLiteral("#000000");
    QString align  = QStringLiteral("left");
    double  x = 0, y = 0;
    double  rotation = 0;
    double  scaleX = 1, scaleY = 1;
    QString transform;  // raw SVG transform attribute
    QString svgContent; // serialised SVG fragment (for RasterBlock logos)
    int     blockId = 0;
    double  origX = 0, origY = 0;  // original position before nesting transform
    QString elementId;              // SVG element ID for QSvgRenderer lookup
};

// ── Flat label (native .rabnest label) ───────────────────────────────────────

struct FlatLabel
{
    LabelType type = LabelType::Text;
    double x = 0, y = 0;        // position relative to piece origin (pixels 96 DPI)
    double rotation = 0;         // degrees
    int    blockId = 0;          // label block identifier
    double pieceMx = 0, pieceMy = 0; // piece origin in layout coords (for nesting transform)

    // Text
    QString text;
    QString fontFamily;
    double  fontSize = 12;
    QString fontWeight;          // "normal" | "bold"
    QString fill;                // hex color
    QString align;               // "left" | "center" | "right"

    // Logo (paths pre-rendered by Valentina)
    struct LogoPath {
        QString d;
        QString fill;
        QString stroke;
        double  strokeWidth = 0;
        QString strokeDasharray;
        QString fillRule;        // "nonzero" | "evenodd"
    };
    QVector<LogoPath> logoPaths;
    double logoScale = 1.0;     // scale from native SVG coords to label pixels
};

// ── Piece label data ──────────────────────────────────────────────────────

struct PieceLabelData
{
    bool    visible    = true;
    QString letter;
    QString annotation;
    QString orientation;
    QString rotationWay;
    QString tilt;
    QString foldPosition;
    bool    onFold     = false;
    quint32 fontSize   = 0;
    double  mx         = 0;
    double  my         = 0;
    QString width;
    QString height;
    QString rotation;
    quint32 centerPin     = 0;
    quint32 topLeftPin    = 0;
    quint32 bottomRightPin = 0;
    quint32 quantity      = 1;
    quint32 noBufferMaterial   = 0;
    quint32 withBufferMaterial = 0;
    quint32 bufferMaterial     = 0;
};

// ── Pattern info block ─────────────────────────────────────────────────────

struct PatternInfoData
{
    bool    visible  = false;
    quint32 fontSize = 0;
    double  mx       = 0;
    double  my       = 0;
    QString width;
    QString height;
    QString rotation;
    quint32 centerPin       = 0;
    quint32 topLeftPin      = 0;
    quint32 bottomRightPin  = 0;
};

// ── Grainline ──────────────────────────────────────────────────────────────

struct GrainlineData
{
    bool    enabled = false;
    bool    visible = true;
    double  mx      = 0;
    double  my      = 0;
    QString length;
    QString rotation;
    quint32 arrows   = 0;
    quint32 centerPin = 0;
    quint32 topPin    = 0;
    quint32 bottomPin = 0;
};

// ── Passmark on a contour node ─────────────────────────────────────────────

struct PassmarkData
{
    bool    passmark           = false;
    QString passmarkLine;       // "One" | "Two" | "Three"
    QString passmarkAngle;      // "Straightforward" | "Bisector" | "UserDefined"
    bool    showSecondPassmark  = false;
    bool    clockwiseOpening    = false;
    bool    manualLength        = false;
    QString passmarkLength;
    bool    manualWidth         = false;
    QString passmarkWidth;
    bool    manualAngle         = false;
    QString passmarkAngleFormula;
    bool    notMirrored         = false;
    QString visibilityFormula;
};

// ── Custom seam allowance record ───────────────────────────────────────────

struct CSARecord
{
    quint32 start     = 0;
    quint32 path      = 0;
    quint32 end       = 0;
    bool    reverse   = false;
    quint32 includeAs = 1; // 1 = main path, 2 = not included
};

// ── Mirror / fold line ─────────────────────────────────────────────────────

struct MirrorLineData
{
    quint32 p1 = 0;
    quint32 p2 = 0;
    QString height;
    QString width;
    QString center;
    bool    manualHeight  = false;
    bool    manualWidth   = false;
    bool    manualCenter  = false;
    QString type;           // foldLineType
    quint32 fontSize  = 0;
    bool    bold      = false;
    bool    italic    = false;
    QString label;
    quint32 alignment = 0;
    bool    visible   = true;
};

// ── SA margin group ────────────────────────────────────────────────────────

struct SAMarginGroup
{
    QVector<quint32> nodeIds;
    QString name;
    int     colorIndex = 0;
    bool    show       = true;
};

// ── Contour node (resolved geometry) ───────────────────────────────────────

struct ContourNode
{
    QString type;       // "NodePoint", "NodeSpline", "NodeArc", "NodeSplinePath"
    quint32 idObject = 0;
    bool    reverse  = false;
    bool    excluded = false;
    PassmarkData passmark;
    // Resolved geometry (filled by PieceBuilder)
    Clipper2Lib::Path64 polyline; // interpolated segment (1+ points)
};

// ── Internal path (resolved) ───────────────────────────────────────────────

struct InternalPath
{
    quint32 pathId = 0;
    Clipper2Lib::Path64 polyline;
};

// ── Pin point (resolved) ───────────────────────────────────────────────────

struct PinPoint
{
    quint32 pointId = 0;
    Clipper2Lib::Point64 pos;
};

// ── Piece (complete) ───────────────────────────────────────────────────────

struct Piece
{
    QString name;
    quint32 id = 0;

    PieceLabelData    labelData;
    PatternInfoData   patternInfo;
    GrainlineData     grainline;
    MirrorLineData    mirrorLine;

    QVector<ContourNode>  contourNodes;    // ordered contour (closed polygon)
    QVector<InternalPath> internalPaths;   // paths drawn inside the piece
    QVector<PinPoint>     pins;            // pin/passmark points
    QVector<PinPoint>     placeLabels;     // label placement points
    QVector<CSARecord>    csaRecords;      // custom SA records
    QVector<SAMarginGroup> saMarginGroups;

    QVector<GradedSize> sizes;             // graded contour per size

    // Visual style extracted from SVG / .rabnest
    PathStyle style;

    // For SVG import: which group this piece belongs to (empty = standalone)
    QString groupName;

    // Convenience: full contour as a single Path64
    Clipper2Lib::Path64 contour() const;
};

// ── Size layer (from SVG graded export) ────────────────────────────────────

struct SizeLayer
{
    QString label;     // e.g. "80-84" display label
    double  sizeValue = 0;  // e.g. 80.0 (nominal size for styling)
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
