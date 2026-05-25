/************************************************************************
 **
 **  @file   svg2ocgpdf.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   25/05/2026
 **
 **  @brief  Convert a graded SVG to layered PDF (one OCG per size).
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include <QApplication>
#include <QFileInfo>

#include "pdf/SvgToOcgPdf.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    if (argc < 2) {
        qWarning() << "Usage: svg2ocgpdf <file.svg> [output.pdf]";
        return 1;
    }

    const QString svgPath = QString::fromLocal8Bit(argv[1]);
    const QFileInfo fi(svgPath);

    QString pdfPath;
    if (argc >= 3) {
        pdfPath = QString::fromLocal8Bit(argv[2]);
    } else {
        pdfPath = fi.absolutePath() + QLatin1Char('/')
                  + fi.completeBaseName() + QStringLiteral("_ocg.pdf");
    }

    SvgToOcgPdf converter;
    if (!converter.convert(svgPath, pdfPath)) {
        qWarning() << "Conversion failed";
        return 1;
    }

    return 0;
}
