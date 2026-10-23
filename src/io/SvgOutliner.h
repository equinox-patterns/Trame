/************************************************************************
 **
 **  @file   SvgOutliner.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   22/10/2026
 **
 **  @brief  Turn every <text> of an SVG into its outline, so the document
 **          needs no font to be drawn.
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

#ifndef SVGOUTLINER_H
#define SVGOUTLINER_H

#include <QByteArray>
#include <QStringList>

/// @brief Text without a font: the same document, with every <text> replaced by its outline.
///
/// The contract between the two applications has one rule about text, and it is not a taste:
/// **no font travels**. A document delivered to a client has to look exactly the same on a
/// machine that has none of the fonts it was written with — and the fonts here (Jost, Yustine
/// Signature) are the pattern maker's, not the client's.
///
/// The PDF already writes its words as paths. This is the same move for the SVG a PDF carries:
/// what the client reopens is a drawing, and a drawing cannot be substituted by a reader.
///
/// What it costs is stated once and assumed: the words are no longer text. They cannot be
/// searched, selected or re-worded — and nothing in a client's application is supposed to
/// re-word them. The plan's cartouche says what it says; the cover page is carried as it was
/// delivered, not regenerated.
class SvgOutliner
{
public:
    /// @brief Replace every <text> of @p svgData by a <path> that draws the same thing.
    ///
    /// The path is placed where the text was: same parent, same own @c transform, same @c fill.
    /// Coordinates are left untouched, which is what keeps a text inside a rotated wrapper
    /// rotated with it.
    ///
    /// @param missingFontsOut  receives the families that are NOT installed on this machine,
    ///                 each once. A text whose font is missing is LEFT AS IT WAS rather than
    ///                 outlined with a substituted face: a wrong outline is a drawing that looks
    ///                 right, and nothing downstream could tell. The caller decides what to say.
    ///
    /// @return the document, outlined as far as it could be. A payload that is not an SVG, or
    ///         that carries no <text>, comes back unchanged — there is nothing to do to it.
    static QByteArray outline(const QByteArray &svgData, QStringList *missingFontsOut = nullptr);
};

#endif // SVGOUTLINER_H
