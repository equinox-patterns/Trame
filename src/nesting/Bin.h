/************************************************************************
 **
 **  @file   Bin.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Strip bin for nesting — fixed width, unbounded height.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef BIN_H
#define BIN_H

#include <vector>

/// A strip bin with fixed width and unbounded height (strip packing).
///
/// The nesting engine places pieces freely in a continuous strip.
/// The height is not declared upfront — it is determined by the envelope
/// of placed pieces after nesting completes.
///
/// Artboards (for PDF export) are a post-processing grid overlaid on the
/// result — they are not part of the bin model.
class Bin
{
public:
    /// A rectangle nothing may be placed in, at a place the caller chooses.
    ///
    /// This is the general form of what used to be a single obstacle pinned to
    /// the top-left corner. The A0 sheet emulation is now literally "a bin with
    /// one zone at the corner", and a page a tiled plan must not print on is
    /// another zone beside it — one rule, no special case.
    ///
    /// ⚠️ The unit is the CALLER's — pixels in this repository, like every other
    /// length the engine receives. The `…Mm` accessors below predate that and
    /// lie; these fields do not repeat the lie.
    struct Zone
    {
        double x      = 0.0;  ///< Left edge, measured from the bin's left edge
        double y      = 0.0;  ///< Top edge, measured from the bin's top edge
        double width  = 0.0;
        double height = 0.0;
    };

    /// Construct a strip bin with the given width and margin in millimetres.
    /// An optional obstacle rectangle can be placed at the top-left corner
    /// (after margin). Pieces will not be placed on top of it.
    ///
    /// The obstacle is appended to the zone list, at the corner. The two
    /// accessors below are kept because hosts report the obstacle's bottom.
    explicit Bin(double widthMm, double marginMm = 5.0,
                 double obstacleWidthMm = 0.0, double obstacleHeightMm = 0.0);

    /// Construct a strip bin whose forbidden zones are given outright.
    Bin(double widthMm, double marginMm, std::vector<Zone> zones);

    /// Strip width in millimetres (total physical width, margins included).
    double widthMm() const noexcept { return m_widthMm; }

    /// Margin inside the bin edges where pieces cannot be placed, in millimetres.
    double marginMm() const noexcept { return m_marginMm; }

    /// Obstacle rectangle dimensions at the top-left corner, in millimetres.
    ///
    /// Zero when the bin was built with an explicit zone list: the corner
    /// obstacle is then the caller's own zone, not this one.
    double obstacleWidthMm()  const noexcept { return m_obstacleWidthMm; }
    double obstacleHeightMm() const noexcept { return m_obstacleHeightMm; }

    /// Every rectangle the engine must keep clear, in the caller's unit.
    const std::vector<Zone> &zones() const noexcept { return m_zones; }

private:
    double m_widthMm;
    double m_marginMm;
    double m_obstacleWidthMm  = 0.0;
    double m_obstacleHeightMm = 0.0;
    std::vector<Zone> m_zones;
};

#endif // BIN_H
