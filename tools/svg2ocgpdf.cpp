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
#include <QStringList>

#include "pdf/SvgToOcgPdf.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QStringList args;
    args.reserve(argc);
    for (int i = 1; i < argc; ++i)
        args.append(QString::fromLocal8Bit(argv[i]));

    // The underlay: a second document, drawn on every page under the layers and outside
    // every optional content group. Optional here, and named rather than positional, so
    // that an existing command line keeps its meaning.
    QString underlayPath;
    const int underlayAt = args.indexOf(QStringLiteral("--underlay"));
    if (underlayAt >= 0) {
        if (underlayAt + 1 >= args.size()) {
            qWarning() << "Usage: svg2ocgpdf <file.svg> [output.pdf] [--underlay <file.svg>]";
            return 1;
        }
        underlayPath = args.at(underlayAt + 1);
        args.removeAt(underlayAt + 1);
        args.removeAt(underlayAt);
    }

    // Whether the layout stays one of the reader's layers. Default yes, which is what this
    // tool has always produced; --no-layout-layer writes it into the file with no marker at
    // all. Named rather than positional, like --underlay, so an existing command line keeps
    // its meaning — and here for the same reason: without it that path is exercisable only
    // through Ostara.
    bool layoutLayer = true;
    const int noLayoutAt = args.indexOf(QStringLiteral("--no-layout-layer"));
    if (noLayoutAt >= 0) {
        layoutLayer = false;
        args.removeAt(noLayoutAt);
    }

    if (args.isEmpty()) {
        qWarning() << "Usage: svg2ocgpdf <file.svg> [output.pdf] [--underlay <file.svg>]"
                      " [--no-layout-layer]";
        return 1;
    }

    const QString svgPath = args.at(0);
    const QFileInfo fi(svgPath);

    QString pdfPath;
    if (args.size() >= 2) {
        pdfPath = args.at(1);
    } else {
        pdfPath = fi.absolutePath() + QLatin1Char('/')
                  + fi.completeBaseName() + QStringLiteral("_ocg.pdf");
    }

    SvgToOcgPdf converter;
    if (!converter.convert(svgPath, pdfPath, {}, underlayPath, layoutLayer)) {
        qWarning() << "Conversion failed";
        return 1;
    }

    return 0;
}
