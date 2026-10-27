/************************************************************************
 **  @file   MarkPainter.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   22/10/2026
 **  @brief  The paper's own paint: frame, marks, numbering.
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

#include "decor/MarkPainter.h"

#include <QDomDocument>
#include <QDomElement>
#include <QFont>
#include <QStringList>
#include <QXmlStreamWriter>
#include <cmath>

namespace
{
constexpr double kPxPerMm = 96.0 / 25.4;

/// One attribute, as a number, with a default that is the template's own.
auto Number(const QDomElement &element, const QString &name, double fallback) -> double
{
    bool ok = false;
    const double value = element.attribute(name).toDouble(&ok);
    return ok ? value : fallback;
}

/// A boolean written as "1", "true" or "0" — the three spellings the templates use.
auto Flag(const QDomElement &element, const QString &name, bool fallback) -> bool
{
    const QString value = element.attribute(name);
    if (value.isEmpty())
    {
        return fallback;
    }
    return value != QLatin1String("0") && value.compare(QLatin1String("false"), Qt::CaseInsensitive) != 0;
}
}  // namespace

//---------------------------------------------------------------------------------------------------------------------
auto MarkPainter::Settings::From(const QByteArray &payload, Settings &out) -> bool
{
    QDomDocument svg;
    if (!static_cast<bool>(svg.setContent(payload)))
    {
        return false;
    }

    const QDomElement recipe = svg.documentElement()
                                   .firstChildElement(QStringLiteral("metadata"))
                                   .firstChildElement(QStringLiteral("exportRecipe"));
    if (recipe.isNull())
    {
        return false;
    }

    const QDomElement format = recipe.firstChildElement(QStringLiteral("format"));
    if (!format.isNull())
    {
        out.framed = Flag(format, QStringLiteral("framed"), false);
        out.columns = format.attribute(QStringLiteral("columns")).toInt();
    }

    const QDomElement decor = recipe.firstChildElement(QStringLiteral("decor"));
    if (decor.isNull())
    {
        // No template: the plan carries no frame and no numbering. That is a fact of the file, not a
        // default — Rabnest's own decor is a decision the application has still to make.
        return false;
    }

    const QDomElement frame = decor.firstChildElement(QStringLiteral("frame"));
    if (!frame.isNull())
    {
        out.frameWidthPt = Number(frame, QStringLiteral("widthPt"), out.frameWidthPt);
        out.frameDashed = Flag(frame, QStringLiteral("dashed"), out.frameDashed);
        out.frameOpacity = static_cast<int>(Number(frame, QStringLiteral("opacity"), out.frameOpacity));
        out.scissorsHorizontal = Flag(frame, QStringLiteral("scissorsHorizontal"), true);
        out.scissorsVertical = Flag(frame, QStringLiteral("scissorsVertical"), true);
        for (const QString &part : frame.attribute(QStringLiteral("dashes")).split(QLatin1Char(' ')))
        {
            bool ok = false;
            const double value = part.toDouble(&ok);
            if (ok)
            {
                out.frameDashes.append(value);
            }
        }
    }

    const QDomElement marks = decor.firstChildElement(QStringLiteral("marks"));
    if (!marks.isNull())
    {
        out.markShape = marks.attribute(QStringLiteral("shape"), out.markShape);
        out.markSizeMm = Number(marks, QStringLiteral("sizeMm"), out.markSizeMm);
        out.markOpacity = static_cast<int>(Number(marks, QStringLiteral("opacity"), out.markOpacity));
        out.markCorner = Flag(marks, QStringLiteral("corner"), true);
    }

    const QDomElement pagination = decor.firstChildElement(QStringLiteral("pagination"));
    if (!pagination.isNull())
    {
        out.numberForm = pagination.attribute(QStringLiteral("form"), out.numberForm);
        const QDomElement centre = pagination.firstChildElement(QStringLiteral("centre"));
        out.numberCentre = !centre.isNull() && Flag(centre, QStringLiteral("enabled"), false);
        out.centreStyle.family = centre.attribute(QStringLiteral("fontFamily"), out.centreStyle.family);
        out.centreStyle.weight = centre.attribute(QStringLiteral("fontWeight"), out.centreStyle.weight);
        out.centreStyle.sizePx = static_cast<int>(Number(centre, QStringLiteral("fontSize"), out.centreStyle.sizePx));
        out.centreStyle.opacity = static_cast<int>(Number(centre, QStringLiteral("opacity"), out.centreStyle.opacity));
        const QDomElement corner = pagination.firstChildElement(QStringLiteral("corner"));
        out.numberCorner = !corner.isNull() && Flag(corner, QStringLiteral("enabled"), false);
        out.cornerStyle.family = corner.attribute(QStringLiteral("fontFamily"), out.cornerStyle.family);
        out.cornerStyle.weight = corner.attribute(QStringLiteral("fontWeight"), out.cornerStyle.weight);
        out.cornerStyle.sizePx = static_cast<int>(Number(corner, QStringLiteral("fontSize"), out.cornerStyle.sizePx));
        out.cornerStyle.opacity = static_cast<int>(Number(corner, QStringLiteral("opacity"), out.cornerStyle.opacity));
        const QDomElement vertical = pagination.firstChildElement(QStringLiteral("verticalCentre"));
        out.numberVerticalCentre = !vertical.isNull() && Flag(vertical, QStringLiteral("enabled"), false);
        out.verticalStyle.family = vertical.attribute(QStringLiteral("fontFamily"), out.verticalStyle.family);
        out.verticalStyle.weight = vertical.attribute(QStringLiteral("fontWeight"), out.verticalStyle.weight);
        out.verticalStyle.sizePx = static_cast<int>(Number(vertical, QStringLiteral("fontSize"), out.verticalStyle.sizePx));
        out.verticalStyle.opacity = static_cast<int>(Number(vertical, QStringLiteral("opacity"), out.verticalStyle.opacity));
        const QDomElement horizontal = pagination.firstChildElement(QStringLiteral("horizontalCentre"));
        out.numberHorizontalCentre = !horizontal.isNull() && Flag(horizontal, QStringLiteral("enabled"), false);
        out.horizontalStyle.family = horizontal.attribute(QStringLiteral("fontFamily"), out.horizontalStyle.family);
        out.horizontalStyle.weight = horizontal.attribute(QStringLiteral("fontWeight"), out.horizontalStyle.weight);
        out.horizontalStyle.sizePx = static_cast<int>(Number(horizontal, QStringLiteral("fontSize"), out.horizontalStyle.sizePx));
        out.horizontalStyle.opacity = static_cast<int>(Number(horizontal, QStringLiteral("opacity"), out.horizontalStyle.opacity));
    }

    out.numbering = out.numberCentre || out.numberCorner || out.numberVerticalCentre
                     || out.numberHorizontalCentre;
    return true;
}

//---------------------------------------------------------------------------------------------------------------------
auto MarkPainter::Settings::Default(bool framed) -> Settings
{
    Settings settings;
    settings.framed = framed;
    settings.columns = 1;
    settings.frameDashed = true;
    settings.frameDashes = {20.0, 10.0};
    settings.frameOpacity = 50;
    settings.markShape = QStringLiteral("octagon");
    settings.markSizeMm = 40.0;
    settings.markOpacity = 50;
    settings.markCorner = true;
    settings.numbering = true;
    settings.numberForm = QStringLiteral("number+letter");
    settings.numberCentre = true;
    settings.numberCorner = true;
    settings.numberSizePx = 85;
    settings.numberOpacity = 25;
    return settings;
}

//---------------------------------------------------------------------------------------------------------------------
void MarkPainter::Paint(QXmlStreamWriter &writer, const Settings &settings, int columns, int rows,
                  double sheetWidthPx, double sheetHeightPx)
{
    if (!settings.framed && !settings.numbering && !settings.markCorner)
    {
        return;
    }

    const double widthPtToPx = 96.0 / 72.0;

    // The frame: one line per sheet edge, drawn once across the whole plan so a seam shared by two
    // sheets is one line and its dashes do not restart at every edge.
    if (settings.framed)
    {
        writer.writeStartElement(QStringLiteral("g"));
        writer.writeAttribute(QStringLiteral("fill"), QStringLiteral("none"));
        writer.writeAttribute(QStringLiteral("stroke"), QStringLiteral("#000000"));
        writer.writeAttribute(QStringLiteral("stroke-width"),
                             QString::number(settings.frameWidthPt * widthPtToPx, 'f', 3));
        writer.writeAttribute(QStringLiteral("stroke-opacity"),
                             QString::number(settings.frameOpacity / 100.0, 'f', 2));
        if (settings.frameDashed && !settings.frameDashes.isEmpty())
        {
            QStringList pattern;
            for (double dash : settings.frameDashes)
            {
                pattern.append(QString::number(dash, 'f', 2));
            }
            writer.writeAttribute(QStringLiteral("stroke-dasharray"), pattern.join(QLatin1Char(' ')));
        }

        for (int row = 0; row < rows; ++row)
        {
            for (int column = 0; column < columns; ++column)
            {
                writer.writeStartElement(QStringLiteral("rect"));
                writer.writeAttribute(QStringLiteral("x"),
                                     QString::number(column * sheetWidthPx, 'f', 3));
                writer.writeAttribute(QStringLiteral("y"),
                                     QString::number(row * sheetHeightPx, 'f', 3));
                writer.writeAttribute(QStringLiteral("width"), QString::number(sheetWidthPx, 'f', 3));
                writer.writeAttribute(QStringLiteral("height"), QString::number(sheetHeightPx, 'f', 3));
                writer.writeEndElement();
            }
        }
        writer.writeEndElement();
    }

    // The scissors: where the paper is cut. They stand ON the seam, at a quarter and three quarters
    // of the sheet — never at its ends, where the frame is, and never in the middle, where a piece is.
    // A seam belongs to the two sheets that share it, so its marks come from the plan's own grid and
    // not from either page: drawn per page they would be cut in half by the very cut they describe.
    if (settings.framed && (settings.scissorsHorizontal || settings.scissorsVertical))
    {
        const double across = 7.468 * kPxPerMm;
        const double along = 13.333 * kPxPerMm;
        const double quarter = 0.25;

        writer.writeStartElement(QStringLiteral("g"));
        writer.writeAttribute(QStringLiteral("stroke"), QStringLiteral("#000000"));
        writer.writeAttribute(QStringLiteral("stroke-width"), QStringLiteral("1"));
        writer.writeAttribute(QStringLiteral("fill"), QStringLiteral("none"));

        // A scissors, drawn rather than imported: two rings away from the cut and two blades that
        // meet on it. The template's own icon is a picture; this is the same figure in geometry, so
        // the decor needs no image and no font — and it is drawn along the seam it describes, which
        // a picture could not be without a second copy for each direction.
        const auto Scissors = [&](double x, double y, bool alongX) {
            const double length = along / 2.0;
            const double spread = across / 2.0;
            const double ring = across / 5.0;

            for (const double side : {-1.0, 1.0})
            {
                const double endX = alongX ? x + side * spread : x + length;
                const double endY = alongX ? y + length : y + side * spread;
                const double startX = alongX ? x - side * spread * 0.2 : x - length;
                const double startY = alongX ? y - length : y - side * spread * 0.2;

                writer.writeStartElement(QStringLiteral("path"));
                writer.writeAttribute(QStringLiteral("d"),
                                      QStringLiteral("M %1 %2 L %3 %4")
                                          .arg(startX, 0, 'f', 3)
                                          .arg(startY, 0, 'f', 3)
                                          .arg(endX, 0, 'f', 3)
                                          .arg(endY, 0, 'f', 3));
                writer.writeEndElement();

                writer.writeStartElement(QStringLiteral("circle"));
                writer.writeAttribute(QStringLiteral("cx"),
                                     QString::number(alongX ? endX : endX - ring * 0.4, 'f', 3));
                writer.writeAttribute(QStringLiteral("cy"),
                                     QString::number(alongX ? endY - ring * 0.4 : endY, 'f', 3));
                writer.writeAttribute(QStringLiteral("r"), QString::number(ring, 'f', 3));
                writer.writeEndElement();
            }
        };

        if (settings.scissorsHorizontal)
        {
            for (int row = 1; row < rows; ++row)
            {
                const double y = row * sheetHeightPx;
                for (int column = 0; column < columns; ++column)
                {
                    for (const double ratio : {quarter, 1.0 - quarter})
                    {
                        Scissors(column * sheetWidthPx + ratio * sheetWidthPx, y, false);
                    }
                }
            }
        }

        if (settings.scissorsVertical)
        {
            for (int column = 1; column < columns; ++column)
            {
                const double x = column * sheetWidthPx;
                for (int row = 0; row < rows; ++row)
                {
                    for (const double ratio : {quarter, 1.0 - quarter})
                    {
                        Scissors(x, row * sheetHeightPx + ratio * sheetHeightPx, true);
                    }
                }
            }
        }

        Q_UNUSED(along)
        writer.writeEndElement();
    }

    // The registration marks: the same shape the template names, at every junction, because that is
    // where a mark can be lined up with the sheet beside it.
    if (settings.markCorner)
    {
        const double size = settings.markSizeMm * kPxPerMm;
        writer.writeStartElement(QStringLiteral("g"));
        // Hollow, like the template draws them: a mark is a registration figure to line a sheet up
        // with its neighbour, not a dot. Filled, it covers the drawing underneath and prints as a
        // blot.
        writer.writeAttribute(QStringLiteral("fill"), QStringLiteral("none"));
        writer.writeAttribute(QStringLiteral("stroke"), QStringLiteral("#000000"));
        writer.writeAttribute(QStringLiteral("stroke-width"), QStringLiteral("1"));
        writer.writeAttribute(QStringLiteral("stroke-opacity"),
                             QString::number(settings.markOpacity / 100.0, 'f', 2));

        for (int row = 0; row <= rows; ++row)
        {
            for (int column = 0; column <= columns; ++column)
            {
                const double x = column * sheetWidthPx;
                const double y = row * sheetHeightPx;
                const double radius = size / 2.0;

                if (settings.markShape == QLatin1String("circle"))
                {
                    writer.writeStartElement(QStringLiteral("circle"));
                    writer.writeAttribute(QStringLiteral("cx"), QString::number(x, 'f', 3));
                    writer.writeAttribute(QStringLiteral("cy"), QString::number(y, 'f', 3));
                    writer.writeAttribute(QStringLiteral("r"), QString::number(radius, 'f', 3));
                    writer.writeEndElement();
                    continue;
                }

                // A square and an octagon are polygons: the same figure the template's icons draw,
                // built here so a mark needs no image and no font.
                const int sides = settings.markShape == QLatin1String("octagon") ? 8 : 4;
                QStringList points;
                for (int side = 0; side < sides; ++side)
                {
                    const double angle = (2.0 * M_PI * side) / sides
                                         + (settings.markShape == QLatin1String("octagon") ? M_PI / 8.0
                                                                                          : M_PI / 4.0);
                    points.append(QStringLiteral("%1,%2")
                                      .arg(x + radius * std::cos(angle), 0, 'f', 3)
                                      .arg(y + radius * std::sin(angle), 0, 'f', 3));
                }
                writer.writeStartElement(QStringLiteral("polygon"));
                writer.writeAttribute(QStringLiteral("points"), points.join(QLatin1Char(' ')));
                writer.writeEndElement();
            }
        }
        writer.writeEndElement();
    }

    // The numbering. Each placement has its own text style — the centre is huge at a quarter opacity,
    // the corners are small at a half — because that is how a template states them, and sharing one
    // style between them is what printed a solid black number where Ostara printed a grey one.
    const auto Text = [&](double x, double y, const QString &text,
                          const Settings::TextStyle &style) {
        writer.writeStartElement(QStringLiteral("text"));
        writer.writeAttribute(QStringLiteral("x"), QString::number(x, 'f', 3));
        writer.writeAttribute(QStringLiteral("y"), QString::number(y, 'f', 3));
        writer.writeAttribute(QStringLiteral("font-family"), style.family);
        writer.writeAttribute(QStringLiteral("font-size"), QString::number(style.sizePx));
        writer.writeAttribute(QStringLiteral("font-weight"), style.weight);
        writer.writeAttribute(QStringLiteral("fill"), QStringLiteral("#000000"));
        writer.writeAttribute(QStringLiteral("fill-opacity"),
                             QString::number(style.opacity / 100.0, 'f', 2));
        writer.writeAttribute(QStringLiteral("text-anchor"), QStringLiteral("middle"));
        writer.writeCharacters(text);
        writer.writeEndElement();
    };

    const auto SheetText = [&](int row, int column) {
        if (settings.numberForm == QLatin1String("number+letter"))
        {
            return QString(QLatin1Char(static_cast<char>('A' + column))) + QString::number(row + 1);
        }
        return QString::number(row * columns + column + 1);
    };

    if (settings.numberCentre)
    {
        for (int row = 0; row < rows; ++row)
        {
            for (int column = 0; column < columns; ++column)
            {
                Text(column * sheetWidthPx + sheetWidthPx / 2.0,
                     row * sheetHeightPx + sheetHeightPx / 2.0, SheetText(row, column),
                     settings.centreStyle);
            }
        }
    }

    if (settings.numberCorner)
    {
        const double inset = 5.0 * kPxPerMm;
        for (int row = 0; row < rows; ++row)
        {
            for (int column = 0; column < columns; ++column)
            {
                Text(column * sheetWidthPx + inset, (row + 1) * sheetHeightPx - inset,
                     SheetText(row, column), settings.cornerStyle);
            }
        }
    }

    if (settings.numberVerticalCentre)
    {
        for (int row = 0; row < rows; ++row)
        {
            for (int column = 0; column + 1 < columns; ++column)
            {
                Text((column + 1) * sheetWidthPx,
                     row * sheetHeightPx + sheetHeightPx / 2.0, SheetText(row, column),
                     settings.verticalStyle);
            }
        }
    }

    if (settings.numberHorizontalCentre)
    {
        const double rail = 5.0 * kPxPerMm;
        for (int row = 0; row + 1 < rows; ++row)
        {
            for (int column = 0; column < columns; ++column)
            {
                Text(column * sheetWidthPx + sheetWidthPx / 2.0,
                     (row + 1) * sheetHeightPx - rail, SheetText(row, column),
                     settings.horizontalStyle);
            }
        }
    }
}
