/************************************************************************
 **
 **  @file   SvgOutliner.cpp
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

#include "io/SvgOutliner.h"

#include <QDebug>
#include <QDomDocument>
#include <QDomElement>
#include <QFont>
#include <QFontInfo>
#include <QPainterPath>
#include <QTextStream>

#include "io/SvgTextFace.h"

namespace
{
/// One number, written the way an SVG path wants it: fixed, and short enough not to bloat.
QString num(double value)
{
    // Five significant digits: about a hundredth of a pixel at the plan's own scale, which no
    // eye, no printer and no plotter reaches — and it is a third of the bytes of a fixed
    // three decimals, on a payload that carries a thousand glyphs.
    return QString::number(value, 'g', 5);
}

/// The path data drawing @p path — curves kept as curves, never flattened.
///
/// A glyph outline is made of cubic curves, and `toSubpathPolygons` would turn each of them
/// into a run of short lines: the drawing would survive at one zoom and fall apart at the next.
/// So the commands are read from the path's own elements, which is what makes this lossless.
QString pathData(const QPainterPath &path)
{
    QString d;
    QTextStream ts(&d);
    bool open = false;

    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        switch (element.type) {
        case QPainterPath::MoveToElement:
            if (open)
                ts << "Z ";
            ts << "M " << num(element.x) << ' ' << num(element.y) << ' ';
            open = true;
            break;
        case QPainterPath::LineToElement:
            ts << "L " << num(element.x) << ' ' << num(element.y) << ' ';
            break;
        case QPainterPath::CurveToElement: {
            // A cubic arrives as three elements: the first control point, the second, the end.
            if (i + 2 >= path.elementCount()) {
                i = path.elementCount();
                break;
            }
            const QPainterPath::Element control2 = path.elementAt(i + 1);
            const QPainterPath::Element end = path.elementAt(i + 2);
            ts << "C " << num(element.x) << ' ' << num(element.y) << ' '
               << num(control2.x) << ' ' << num(control2.y) << ' '
               << num(end.x) << ' ' << num(end.y) << ' ';
            i += 2;
            break;
        }
        case QPainterPath::CurveToDataElement:
            // Consumed by the CurveToElement above; nothing of its own to write.
            break;
        }
    }

    if (open)
        ts << "Z";

    return d.trimmed();
}

/// The first family of an SVG @c font-family list — a list is a fallback chain, and what a
/// document asks for first is what it was written with.
QString firstFamily(const QString &value)
{
    const QString first = value.section(QLatin1Char(','), 0, 0).trimmed();
    if (first.startsWith(QLatin1Char('\'')) || first.startsWith(QLatin1Char('"')))
        return first.mid(1, first.size() - 2);
    return first;
}

int weightOf(const QDomElement &el)
{
    const QString value = el.attribute(QStringLiteral("font-weight")).trimmed();
    if (value.compare(QStringLiteral("bold"), Qt::CaseInsensitive) == 0)
        return 700;
    if (value.compare(QStringLiteral("normal"), Qt::CaseInsensitive) == 0)
        return 400;
    bool ok = false;
    const int weight = value.toInt(&ok);
    if (!ok || weight <= 0)
        return 400;
    return weight;
}
}  // namespace

QByteArray SvgOutliner::outline(const QByteArray &svgData, QStringList *missingFontsOut)
{
    QDomDocument doc;
    if (!doc.setContent(svgData)) {
        // Not XML, or not an SVG: there is no text to outline and nothing to say about it.
        return svgData;
    }

    QDomNodeList texts = doc.elementsByTagName(QStringLiteral("text"));
    if (texts.isEmpty())
        return svgData;

    // Walked backwards: replacing an element moves the ones that follow it, and a node list
    // is live.
    for (int i = texts.size() - 1; i >= 0; --i) {
        const QDomElement text = texts.at(i).toElement();
        if (text.isNull())
            continue;

        const QString content = text.text().simplified();
        if (content.isEmpty()) {
            // A text whose words live in <tspan> children, or an empty one. Left as it is —
            // an empty path would silently delete the words.
            qWarning() << "SvgOutliner: a <text> with no direct content is left as it was";
            continue;
        }

        const QString family = firstFamily(text.attribute(QStringLiteral("font-family")));
        if (family.isEmpty()) {
            qWarning() << "SvgOutliner: a <text> names no font family; left as it was";
            continue;
        }

        // The face comes from the one function that reads it — the same one the PDF writer
        // uses, so the cartouche of the plan and the cartouche of the file it came from cannot
        // disagree. Its size, its weight and its named style are all in there.
        const QFont font = SvgTextFace::fontOf(text);

        // A substituted face is the one failure nobody sees: the outline would be a real
        // drawing of the wrong letters, and no reader, no test and no eye could tell it from
        // the right one afterwards. So the family is checked, and a missing one is reported
        // and left unresolved rather than guessed.
        const QString asked = text.attribute(QStringLiteral("font-family"));
        if (asked.isEmpty()
            || QFontInfo(font).family().compare(firstFamily(asked), Qt::CaseInsensitive) != 0) {
            if (!asked.isEmpty() && missingFontsOut != nullptr
                && !missingFontsOut->contains(firstFamily(asked))) {
                missingFontsOut->append(firstFamily(asked));
            }
            continue;
        }

        const double x = text.attribute(QStringLiteral("x")).toDouble();
        const double y = text.attribute(QStringLiteral("y")).toDouble();

        // The outline is built at the ORIGIN and the text's own position moves it, through a
        // transform. Two reasons, and the second is the one that shows: the shape is then a
        // property of the words alone, so the same sentence drawn twice is the same bytes
        // twice — which is what lets a compressor find it, and what a glyph set could one day
        // share outright.
        QPainterPath path;
        path.addText(QPointF(0.0, 0.0), font, content);
        if (path.isEmpty()) {
            qWarning() << "SvgOutliner: no outline for" << content;
            continue;
        }

        // `existing translate(x,y)` and not the other way round: an SVG transform list is
        // applied right to left, so the text's own position has to be the innermost — it is
        // what places the glyphs in the text's own frame, before any wrapper turns them.
        QString transform = QStringLiteral("translate(%1 %2)").arg(num(x), num(y));
        if (text.hasAttribute(QStringLiteral("transform")))
            transform = text.attribute(QStringLiteral("transform")) + QLatin1Char(' ') + transform;

        QDomElement replacement = doc.createElement(QStringLiteral("path"));
        replacement.setAttribute(QStringLiteral("d"), pathData(path));
        replacement.setAttribute(QStringLiteral("transform"), transform);
        replacement.setAttribute(QStringLiteral("fill"),
                                  text.attribute(QStringLiteral("fill"), QStringLiteral("black")));
        for (const char *carried : {"opacity", "fill-opacity"}) {
            const QString name = QString::fromLatin1(carried);
            if (text.hasAttribute(name))
                replacement.setAttribute(name, text.attribute(name));
        }

        text.parentNode().replaceChild(replacement, text);
    }

    return doc.toByteArray(4);
}
