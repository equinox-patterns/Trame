/************************************************************************
 **
 **  @file   SvgTextFace.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   22/10/2026
 **
 **  @brief  The face a <text> asks for — read once, for every consumer.
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

#ifndef SVGTEXTFACE_H
#define SVGTEXTFACE_H

#include <QFont>

class QDomElement;

/// @brief Which face a document's <text> was drawn with.
///
/// Two things now read it — the PDF writer, which re-emits the words as path operators, and the
/// outliner, which re-emits them as <path> in the SVG a PDF carries. They must resolve the SAME
/// face, or the plan and the file it came from would disagree about their own cartouche. That is
/// why this is written once.
///
/// The style NAME matters more than the weight number, and that is measured rather than
/// supposed: the same 300 is « Light » in one family and « ExtraLight » in the next, and asking
/// for 900 draws « ExtraBold ». Ostara writes the face it drew with — `ostara:font-face` — and
/// carrying it by name keeps the very face the application used, whatever font the machine has.
class SvgTextFace
{
public:
    /// @brief The font @p text asks for: family, pixel size, weight, and style name.
    ///
    /// Defaults are the writer's own: family « Jost », size 12, weight 400 — so a document that
    /// states nothing still gets a face rather than a zero font.
    static QFont fontOf(const QDomElement &text);
};

#endif // SVGTEXTFACE_H
