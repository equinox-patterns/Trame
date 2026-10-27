/************************************************************************
 **
 **  @file   GradedOverlays.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   26/10/2026
 **
 **  @brief  The two closed layers the Projector format puts UNDER the pieces.
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

#ifndef GRADEDOVERLAYS_H
#define GRADEDOVERLAYS_H

#include <QString>
#include <QStringList>
#include <QVector>

class QXmlStreamWriter;

/**
 * @brief The full-page background and the calibration grids, written by whoever composes the SVG.
 *
 * **They belong to the SOCLE, not to one application.** Ostara writes them for its Projector
 * export and Rabnest writes them for its own; the two files must be the same file, so the code
 * that emits them is here, once — a second copy would drift at the first change of colour or of
 * step, and nothing would say which one is the projector.
 *
 * ⚠️ **Under the pieces, never above.** The plan is drawn in strokes: a light ray goes through a
 * line, not through a sheet. A grid laid over the pieces would mask them; laid underneath, it is
 * read through them.
 *
 * ⚠️ **Each one carries `ostara:role`.** Without it Trame takes it for a SIZE: it would enter the
 * reader's panel as a size, Rabnest would offer it as a size to nest, and the painting order would
 * no longer hold. The role is what makes it a background or a calibration layer, and it also
 * decides where it stands in `/Order`.
 *
 * The numbers are pixels — the unit of the file's `viewBox` — because that is the unit the drawing
 * is written in. Converting once, in the caller, is what keeps a step of 5 cm from being written in
 * millimetres on a page that does not speak of them.
 *
 * @see Projector.md §4 and §14
 */
namespace GradedOverlays
{
/// One calibration grid: a step, and the name the reader shows.
struct Grid
{
    QString name{};    ///< what the reader reads in its panel
    QString id{};      ///< the key of the PDF — a name, not a sentence
    double stepPx{0.0}; ///< the step of the grid
};

/// What a Projector document adds under the pieces.
struct Set
{
    /// The background, as an SVG colour. **Empty: no background** — the state of every other
    /// format, which has nothing to write here.
    QString background{};
    QString backgroundName{};
    QString backgroundId{};

    QVector<Grid> grids{};
};

/**
 * @brief Write the overlays under @p widthPx × @p heightPx.
 *
 * Writes nothing when @p overlays holds neither a background nor a grid — a caller that carries no
 * overlay is a caller of another format, and it must not see an empty layer appear.
 *
 * @param writer       the writer already positioned on the document's root element
 * @param widthPx      the page's width, in the file's own pixels
 * @param heightPx     the page's height, in the same pixels
 * @param strokePx     the grid's stroke, in the same pixels
 * @param strokeColour the grid's stroke colour, as an SVG colour
 */
auto Write(QXmlStreamWriter &writer, const Set &overlays, double widthPx, double heightPx,
           double strokePx, const QString &strokeColour) -> void;
}  // namespace GradedOverlays

#endif  // GRADEDOVERLAYS_H
