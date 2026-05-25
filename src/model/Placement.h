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
struct Placement
{
    int    pieceIndex = 0;  ///< Index in the input pieces array
    double x          = 0;  ///< Reference point X in millimetres
    double y          = 0;  ///< Reference point Y in millimetres
    double rotation   = 0;  ///< Rotation applied, in degrees
};

#endif // PLACEMENT_H
