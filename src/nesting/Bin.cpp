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

#include "Bin.h"

#include <cassert>
#include <utility>

Bin::Bin(double widthMm, double marginMm,
         double obstacleWidthMm, double obstacleHeightMm)
    : m_widthMm(widthMm)
    , m_margins(Margins::Uniform(marginMm))
    , m_obstacleWidthMm(obstacleWidthMm)
    , m_obstacleHeightMm(obstacleHeightMm)
{
    assert(widthMm > 0.0 && "Bin width must be positive");
    assert(marginMm >= 0.0 && "Bin margin must be non-negative");
    assert(obstacleWidthMm >= 0.0 && "Obstacle width must be non-negative");
    assert(obstacleHeightMm >= 0.0 && "Obstacle height must be non-negative");

    // The engine has always read the obstacle's two dimensions as a conjunction
    // — an obstacle of null width is no obstacle — and the zone list keeps that
    // reading rather than inventing a second one.
    if (obstacleWidthMm > 0.0 && obstacleHeightMm > 0.0)
    {
        // The corner is the caller's, and the caller's corner sits past the margin: read off the
        // bin's own, which on this bin are the one value the caller passed.
        m_zones.push_back(Zone{m_margins.left, m_margins.top, obstacleWidthMm, obstacleHeightMm});
    }
}

Bin::Bin(double widthMm, double marginMm, std::vector<Zone> zones)
    : m_widthMm(widthMm)
    , m_margins(Margins::Uniform(marginMm))
    , m_zones(std::move(zones))
{
    assert(widthMm > 0.0 && "Bin width must be positive");
    assert(marginMm >= 0.0 && "Bin margin must be non-negative");

    for (const Zone &z : m_zones)
    {
        assert(z.width >= 0.0 && "Zone width must be non-negative");
        assert(z.height >= 0.0 && "Zone height must be non-negative");
    }
}

Bin::Bin(double widthMm, Margins margins, std::vector<Zone> zones)
    : m_widthMm(widthMm)
    , m_margins(margins)
    , m_zones(std::move(zones))
{
    assert(widthMm > 0.0 && "Bin width must be positive");
    assert(margins.left >= 0.0 && "Bin margin must be non-negative");
    assert(margins.top >= 0.0 && "Bin margin must be non-negative");
    assert(margins.right >= 0.0 && "Bin margin must be non-negative");
    assert(margins.bottom >= 0.0 && "Bin margin must be non-negative");

    for (const Zone &z : m_zones)
    {
        assert(z.width >= 0.0 && "Zone width must be non-negative");
        assert(z.height >= 0.0 && "Zone height must be non-negative");
    }
}
