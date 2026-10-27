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

    /// What the bin leaves clear along each of its four edges, in the caller's unit.
    ///
    /// **Four numbers rather than one** because a folded fabric has four different ones: the edge
    /// a piece is cut ON THE FOLD against pays nothing — its fold line has to sit exactly on the
    /// fold, and a margin would push it off by that much, so the piece comes out too narrow by
    /// twice as much once the cloth is opened — while a selvedge edge pays.
    ///
    /// A bin with one margin for everything — the print run's, Rabnest's, both benches' — has all
    /// four equal, and therefore behaves **exactly** as it did when there was only one.
    ///
    /// ⚠️ The unit is the CALLER's, like @ref Zone and for the same reason.
    ///
    /// ⚠️ **This is not a job for @ref Zone**, tempting as it looks: a zone is inflated by the whole
    /// piece gap before it forbids anything, so a zone saying "keep 20 mm off the edge" would keep
    /// `20 mm + gap`. Two different rules, two different places.
    struct Margins
    {
        double left{0.0};
        double top{0.0};
        double right{0.0};
        double bottom{0.0};

        /// The same value on all four sides — what `Bin(width, margin)` builds, and all any caller
        /// but one ever asks for.
        ///
        /// Deliberately a named function and not a one-argument constructor: a constructor would
        /// make `Bin(1400, 5.0, zones)` ambiguous between the two of them.
        static auto Uniform(double all) noexcept -> Margins
        {
            return Margins{all, all, all, all};
        }
    };

    /// Construct a strip bin with the given width and margin in millimetres.
    /// An optional obstacle rectangle can be placed at the top-left corner
    /// (after margin). Pieces will not be placed on top of it.
    ///
    /// **The same margin on all four sides.** The obstacle is appended to the zone
    /// list, at the corner. The two accessors below are kept because hosts report
    /// the obstacle's bottom.
    explicit Bin(double widthMm, double marginMm = 5.0,
                 double obstacleWidthMm = 0.0, double obstacleHeightMm = 0.0);

    /// Construct a strip bin whose forbidden zones are given outright.
    Bin(double widthMm, double marginMm, std::vector<Zone> zones);

    /// Construct a strip bin whose four margins are given one by one.
    ///
    /// **No corner obstacle here**: a caller that wants one already has it in @p zones — the list
    /// form is how it asks for it — and the bin that needs four margins is the folded cloth's,
    /// which never has an obstacle to place.
    Bin(double widthMm, Margins margins, std::vector<Zone> zones);

    /// Strip width in millimetres (total physical width, margins included).
    double widthMm() const noexcept { return m_widthMm; }

    /// The margin of a bin built with ONE value — **the same on all four sides**.
    ///
    /// A bin built with @ref Margins answers its LEFT margin here. A reader that needs all four
    /// asks @ref margins instead; this stays for the callers that have only ever wanted the one
    /// number, and answering it with the left is what keeps them reading exactly what they read
    /// before.
    double marginMm() const noexcept { return m_margins.left; }

    /// What the bin leaves clear on each of its four edges. All four are equal on a bin built with
    /// a single margin.
    const Margins &margins() const noexcept { return m_margins; }

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
    Margins m_margins;
    double m_obstacleWidthMm  = 0.0;
    double m_obstacleHeightMm = 0.0;
    std::vector<Zone> m_zones;
};

#endif // BIN_H
