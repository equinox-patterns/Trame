/************************************************************************
 **
 **  @file   Bin.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Strip bin implementation.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include "Bin.h"

#include <cassert>

Bin::Bin(double widthMm, double marginMm,
         double obstacleWidthMm, double obstacleHeightMm) noexcept
    : m_widthMm(widthMm)
    , m_marginMm(marginMm)
    , m_obstacleWidthMm(obstacleWidthMm)
    , m_obstacleHeightMm(obstacleHeightMm)
{
    assert(widthMm > 0.0 && "Bin width must be positive");
    assert(marginMm >= 0.0 && "Bin margin must be non-negative");
    assert(obstacleWidthMm >= 0.0 && "Obstacle width must be non-negative");
    assert(obstacleHeightMm >= 0.0 && "Obstacle height must be non-negative");
}
