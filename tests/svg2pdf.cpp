/************************************************************************
 **
 **  @file   svg2pdf.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   26/05/2026
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

#include <QApplication>
#include <QPdfWriter>
#include <QPainter>
#include <QSvgRenderer>
#include <QPageSize>
#include <QMarginsF>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    if (argc < 2) {
        qWarning() << "Usage: svg2pdf <file.svg>";
        return 1;
    }

    const QString svgPath = QString::fromLocal8Bit(argv[1]);
    const QFileInfo fi(svgPath);
    const QString pdfPath = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(".pdf");

    QDomDocument svgDoc;
    QFile svgFile(svgPath);
    svgFile.open(QIODevice::ReadOnly);
    svgDoc.setContent(&svgFile);
    svgFile.close();

    const QDomElement svgEl = svgDoc.documentElement();
    auto mmFromAttr = [](const QString &s) -> qreal {
        if (s.endsWith(QStringLiteral("mm"))) return s.chopped(2).toDouble();
        return s.toDouble() * 25.4 / 96.0;
    };
    const qreal pageWmm = mmFromAttr(svgEl.attribute(QStringLiteral("width")));
    const qreal pageHmm = mmFromAttr(svgEl.attribute(QStringLiteral("height")));
    const QStringList vb = svgEl.attribute(QStringLiteral("viewBox")).split(QLatin1Char(' '));
    const QRectF viewBox(vb.value(0).toDouble(), vb.value(1).toDouble(),
                         vb.value(2).toDouble(), vb.value(3).toDouble());

    QSvgRenderer renderer(svgPath);

    QPdfWriter writer(pdfPath);
    writer.setResolution(96);
    writer.setPageSize(QPageSize(QSizeF(pageWmm, pageHmm), QPageSize::Millimeter));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));

    QPainter painter(&writer);
    renderer.render(&painter, viewBox);
    painter.end();

    qWarning() << "PDF exported:" << pdfPath;
    return 0;
}
