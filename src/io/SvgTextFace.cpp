/************************************************************************
 **
 **  @file   SvgTextFace.cpp
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

#include "io/SvgTextFace.h"

#include <QDomElement>

#include <cmath>

QFont SvgTextFace::fontOf(const QDomElement &text)
{
    const QString family = text.attribute(QStringLiteral("font-family"), QStringLiteral("Jost"));
    const double size = text.attribute(QStringLiteral("font-size"), QStringLiteral("12")).toDouble();
    const QString weight = text.attribute(QStringLiteral("font-weight"), QStringLiteral("400"));

    QFont font(family);
    font.setPixelSize(static_cast<int>(std::round(size)));

    const QString fw = weight.toLower();
    if (fw == QStringLiteral("bold") || fw == QStringLiteral("700")
        || fw == QStringLiteral("800") || fw == QStringLiteral("900")) {
        font.setBold(true);
    } else if (fw == QStringLiteral("thin") || fw == QStringLiteral("100")) {
        font.setWeight(QFont::Thin);
    } else if (fw == QStringLiteral("light") || fw == QStringLiteral("300")) {
        font.setWeight(QFont::Light);
    } else if (fw == QStringLiteral("medium") || fw == QStringLiteral("500")) {
        font.setWeight(QFont::Medium);
    } else {
        bool ok = false;
        const int number = weight.toInt(&ok);
        if (ok)
            font.setWeight(static_cast<QFont::Weight>(number));
    }

    font.setStyleName(text.attribute(QStringLiteral("ostara:font-face")));

    return font;
}
