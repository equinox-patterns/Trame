/************************************************************************
 **
 **  @file   Placement.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Placement result produced by the nesting engine.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef PLACEMENT_H
#define PLACEMENT_H

/// Result of placing one piece in the bin.
///
/// ⚠️ x and y are NOT in the unit of the polygons handed to Engine::place().
/// Those travel in the engine's internal integer frame — the input coordinates
/// times Engine::kScale — while the placement is divided back by kScale before
/// being returned (`pl.x = px / kScale`, Engine.cpp). x and y are therefore in
/// the CALLER's coordinate unit: SVG user units, i.e. pixels at 96 dpi for
/// every caller in this repository. Adding a placement straight to a polygon
/// vertex throws the piece away by a factor kScale.
///
/// The reference point is the min-bbox corner of the piece BEFORE rotation, so
/// its absolute contour is
///     (x, y) + R(rotation) · (p - minBBox(p))
/// — what NestedSvgWriter emits, and what tests/gapbench.cpp rebuilds.
struct Placement
{
    int    pieceIndex = 0;  ///< Index in the input pieces array
    double x          = 0;  ///< Reference point X, in the caller's coordinate unit (already un-scaled)
    double y          = 0;  ///< Reference point Y, in the caller's coordinate unit (already un-scaled)
    double rotation   = 0;  ///< Rotation applied, in degrees, about the reference point
};

#endif // PLACEMENT_H
