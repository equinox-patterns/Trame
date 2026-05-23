/************************************************************************
 **
 **  @file   GradedSize.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Represents one graded size of a pattern piece.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef GRADEDSIZE_H
#define GRADEDSIZE_H

#include <clipper2/clipper.h>

struct GradedSize
{
    double size    = 0;        // e.g. 80, 84, 88 (cm)
    double stature = 0;        // e.g. 160, 168, 176
    Clipper2Lib::Path64 contour; // resolved polygon points
};

#endif // GRADEDSIZE_H
