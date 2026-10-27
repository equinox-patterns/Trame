/************************************************************************
 **
 **  @file   GradedOverlays.cpp
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

#include "io/GradedOverlays.h"

#include "io/GradedSvgContract.h"

#include <QXmlStreamWriter>

namespace
{
/// An id an SVG reader accepts: letters, digits, `_` and `-`, everything else folded to `_`.
///
/// The callers pass names a human wrote (« Magenta background »), and a space in an `id` is not an
/// id. This is the writer's own guard, so that no caller has to remember the rule.
auto SanitizeId(const QString &id) -> QString
{
    QString clean;
    clean.reserve(id.size());

    for (const QChar character : id)
    {
        const bool kept = character.isLetterOrNumber() || character == u'_' || character == u'-';
        clean.append(kept ? character : u'_');
    }

    return clean;
}

/// Writes one overlay layer and its `<title>`, and leaves the element open for its content.
///
/// `i:layer` and `inkscape:groupmode` are both written because the two readers of these files do
/// not look at the same one: Trame reads either, an editor reads the inkscape one.
void OpenOverlay(QXmlStreamWriter &writer, const QString &id, const QString &name, const char *role)
{
    writer.writeStartElement(QStringLiteral("g"));
    writer.writeAttribute(QStringLiteral("id"), SanitizeId(id));
    writer.writeAttribute(QStringLiteral("i:layer"), QStringLiteral("yes"));
    writer.writeAttribute(QStringLiteral("inkscape:label"), name);
    writer.writeAttribute(QStringLiteral("inkscape:groupmode"), QStringLiteral("layer"));
    writer.writeAttribute(QLatin1String(kGradedRoleAttr), QLatin1String(role));
    writer.writeTextElement(QStringLiteral("title"), name);
}
}  // namespace

//---------------------------------------------------------------------------------------------------------------------
auto GradedOverlays::Write(QXmlStreamWriter &writer, const Set &overlays, double widthPx,
                           double heightPx, double strokePx, const QString &strokeColour) -> void
{
    if (overlays.background.isEmpty() && overlays.grids.isEmpty())
    {
        return;
    }

    if (!overlays.background.isEmpty())
    {
        OpenOverlay(writer, overlays.backgroundId, overlays.backgroundName, kGradedRoleBackground);

        // Le fond est la PAGE : elle part de l'origine et fait toute la surface écrite.
        writer.writeStartElement(QStringLiteral("rect"));
        writer.writeAttribute(QStringLiteral("x"), QStringLiteral("0"));
        writer.writeAttribute(QStringLiteral("y"), QStringLiteral("0"));
        writer.writeAttribute(QStringLiteral("width"), QString::number(widthPx));
        writer.writeAttribute(QStringLiteral("height"), QString::number(heightPx));
        writer.writeAttribute(QStringLiteral("fill"), overlays.background);
        writer.writeAttribute(QStringLiteral("stroke"), QStringLiteral("none"));
        writer.writeEndElement();  // rect

        writer.writeEndElement();  // g (background layer)
    }

    for (const Grid &grid : overlays.grids)
    {
        if (grid.stepPx <= 0.0)
        {
            continue;
        }

        OpenOverlay(writer, grid.id, grid.name, kGradedRoleCalibration);

        // **Une seule trajectoire par grille**, jamais un chemin par trait : c'est une grille, et un
        // lecteur qui la sélectionne doit la prendre d'un bloc.
        QStringList moves;
        for (double x = 0.0; x <= widthPx; x += grid.stepPx)
        {
            moves << QStringLiteral("M%1 0V%2").arg(x).arg(heightPx);
        }
        for (double y = 0.0; y <= heightPx; y += grid.stepPx)
        {
            moves << QStringLiteral("M0 %1H%2").arg(y).arg(widthPx);
        }

        writer.writeStartElement(QStringLiteral("path"));
        writer.writeAttribute(QStringLiteral("d"), moves.join(QLatin1Char(' ')));
        writer.writeAttribute(QStringLiteral("fill"), QStringLiteral("none"));
        writer.writeAttribute(QStringLiteral("stroke"), strokeColour);
        writer.writeAttribute(QStringLiteral("stroke-width"), QString::number(strokePx));
        writer.writeEndElement();  // path

        writer.writeEndElement();  // g (calibration layer)
    }
}
