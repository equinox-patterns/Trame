/************************************************************************
 **
 **  @file   MarkPainter.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   22/10/2026
 **
 **  @brief  The paint a plan carries besides its pieces — shared by Ostara and Rabnest.
 **
 **  @copyright
 **
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

#ifndef MARKPAINTER_H
#define MARKPAINTER_H

#include <QByteArray>
#include <QString>
#include <QVector>

class QXmlStreamWriter;

/// What a plan's paper carries besides its pieces: the frame, the registration marks, the numbers.
///
/// Rabnest does not copy the imported PDF's frame — it **rebuilds** it, because a frame drawn for A0
/// is wrong the moment the client asks for A4: the columns change, the seams move, the numbers
/// renumber. What travels is not the drawing but the RECIPE, and the recipe says which format, framed
/// or not, how many columns, and which template drew the rest.
///
/// One substitution, decided with Sophie: the pagination's face. The creator's Jost lives on her
/// machine and on nobody else's — the client composed the numbers herself, so they are drawn in
/// Inter, with the size, the weight and the opacity the recipe states.
namespace MarkPainter
{
/// The decor block of @p payload, or empty when the file carries none — a rouleau, an A0 sheet, or
/// a PDF whose author did not want any.
struct Settings
{
    bool framed = false;
    int columns = 1;

    double frameWidthPt = 1.0;
    bool frameDashed = false;
    QVector<double> frameDashes;
    int frameOpacity = 100;
    bool scissorsHorizontal = true;
    bool scissorsVertical = true;

    QString markShape = QStringLiteral("circle");
    double markSizeMm = 40.0;
    int markOpacity = 100;
    bool markCorner = true;

    /// One style per placement, because that is how a template states them: the centre number and the
    /// corner numbers are set independently, and a painter that shares one style between them prints
    /// a quarter-opacity centre and a solid corner — which is exactly how it looked beside Ostara.
    struct TextStyle
    {
        QString family = QStringLiteral("Inter");
        QString weight = QStringLiteral("400");
        int sizePx = 16;
        int opacity = 100;
    };

    bool numbering = false;
    QString numberForm = QStringLiteral("number");
    bool numberCentre = false;
    bool numberCorner = false;
    TextStyle centreStyle;
    TextStyle cornerStyle;
    TextStyle verticalStyle;
    TextStyle horizontalStyle;
    bool numberVerticalCentre = false;   ///< numbers on the vertical seams
    bool numberHorizontalCentre = false; ///< numbers on the horizontal seams
    int numberSizePx = 16;
    int numberOpacity = 100;
    QString numberWeight = QStringLiteral("400");

    /// Rabnest's own decor — the one applied when the imported PDF transmits none, which is the case
    /// for a rouleau and for an A0 sheet: nothing was drawn there, so there is nothing to reproduce
    /// and the application decides. The user never sees it and never chooses it.
    static auto Default(bool framed) -> Settings;

    /// Reads the recipe out of the payload. False when there is none.
    static auto From(const QByteArray &payload, Settings &out) -> bool;
};

/// Draws @p settings on the plan: `columns × rows` sheets, @p sheetWidthPx by @p sheetHeightPx.
///
/// Appended to the run's own SVG, so the paper and the drawing are one document — which is what
/// Trame's PDF writer expects, and what makes the frame impossible to switch off in a reader.
void Paint(QXmlStreamWriter &writer, const Settings &settings, int columns, int rows,
           double sheetWidthPx, double sheetHeightPx);
}  // namespace MarkPainter

#endif  // MARKPAINTER_H
