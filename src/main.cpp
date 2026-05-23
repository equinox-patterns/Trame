/************************************************************************
 **
 **  @file   main.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Entry point for the NestingApp standalone.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include <QApplication>
#include <QCommandLineParser>

#include "io/SvgReader.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("NestingApp"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("PDF nesting tool for sewing patterns"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("file"),
                                 QStringLiteral("SVG file to load"));
    parser.addOption({{QStringLiteral("l"), QStringLiteral("layer")},
                      QStringLiteral("Size layer index (0 = smallest, default: 0)"),
                      QStringLiteral("index"), QStringLiteral("0")});

    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        qWarning() << "Usage: NestingApp <file.svg>";
        return 1;
    }

    const QString filePath = args.at(0);
    qWarning() << "Loading:" << filePath;

    SvgReader reader;

    // Load all layers and dump summary
    QVector<SizeLayer> layers;
    if (!reader.read(filePath, layers)) {
        qWarning() << "Failed to parse SVG";
        return 1;
    }

    qWarning() << "Size layers:" << layers.size();
    for (const auto &layer : layers) {
        qWarning() << "";
        qWarning() << "── Layer:" << layer.label << "(sizeValue:" << layer.sizeValue << ")";
        qWarning() << "   Pieces:" << layer.pieces.size();
        for (const auto &piece : layer.pieces) {
            const auto c = piece.contour();
            const QString grp = piece.groupName.isEmpty()
                                ? QStringLiteral("(standalone)")
                                : piece.groupName;
            qWarning() << "     " << piece.name
                        << " | contour:" << c.size() << "pts"
                        << " | internal:" << piece.internalPaths.size()
                        << " | group:" << grp;
        }
    }

    qWarning() << "";
    qWarning() << "Done.";
    return 0;
}
