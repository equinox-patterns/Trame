/************************************************************************
 **
 **  @file   main.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  CLI entry point — SVG graded → nesting → nested SVG → PDF OCG.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include <chrono>
#include <cstdint>
#include <limits>
#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>

#include "io/NestedSvgWriter.h"
#include "io/SvgReader.h"
#include "nesting/Bin.h"
#include "nesting/Engine.h"
#include "nesting/Footprint.h"
#include "pdf/SvgToOcgPdf.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Rabnest"));
    QApplication::setApplicationVersion(QStringLiteral("0.2.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Nesting + PDF OCG for sewing pattern SVG exports"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("file"),
                                 QStringLiteral("Graded SVG file from Valentina"));
    parser.addOption({{QStringLiteral("w"), QStringLiteral("width")},
                      QStringLiteral("Bin width in mm (default: 841 = A0)"),
                      QStringLiteral("mm"), QStringLiteral("841")});
    parser.addOption({{QStringLiteral("o"), QStringLiteral("output")},
                      QStringLiteral("Output PDF file path"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("no-nesting"),
                      QStringLiteral("Skip nesting, keep pieces at original positions")});

    parser.process(app);

    const QStringList args = parser.positionalArguments();
    if (args.isEmpty()) {
        qWarning() << "Usage: Rabnest <file.svg> [-o output.pdf] [-w width_mm] [--no-nesting]";
        return 1;
    }

    const QString svgPath = args.at(0);
    const double binWidthMm = parser.value(QStringLiteral("width")).toDouble();
    const QFileInfo fi(svgPath);

    const QString baseName = fi.completeBaseName();
    const QString outputPdf = parser.isSet(QStringLiteral("output"))
        ? parser.value(QStringLiteral("output"))
        : QStringLiteral("tests/outputs/") + baseName + QStringLiteral("_nested.pdf");
    const QFileInfo pdfFi(outputPdf);
    const QString nestedSvg = pdfFi.absolutePath() + QLatin1Char('/')
        + baseName + QStringLiteral("_nested.svg");

    qWarning() << "=== Rabnest V4 — SVG → Nesting → Nested SVG → PDF OCG ===";
    qWarning() << "Input:" << svgPath;

    // ── 1. Read SVG ──────────────────────────────────────────────────────────
    QVector<SizeLayer> layers;
    SvgReader reader;
    if (!reader.read(svgPath, layers)) {
        qWarning() << "Failed to parse SVG";
        return 1;
    }

    qWarning() << "Size layers:" << layers.size();
    int totalPieces = 0;
    for (const auto &layer : layers)
        totalPieces += layer.pieces.size();
    qWarning() << "Pieces per layer:" << totalPieces / (layers.isEmpty() ? 1 : layers.size());

    // ── 2. Compute footprints ─────────────────────────────────────────────────
    const Footprint::Result fp = Footprint::compute(layers);
    qWarning() << "Footprints:" << fp.polygons.size();
    for (size_t i = 0; i < fp.names.size(); ++i) {
        qWarning().noquote()
            << QStringLiteral("  [%1] %2 | %3 sizes | %4 mm²")
               .arg(i, 2)
               .arg(fp.keys[i], -25)
               .arg(fp.sizeCounts[i], 2)
               .arg(Clipper2Lib::Area(fp.polygons[i]) / 1e6, 8, 'f', 0);
    }

    if (fp.polygons.empty()) {
        qWarning() << "No footprints — nothing to nest";
        return 1;
    }

    // ── 3. Nesting ──────────────────────────────────────────────────────────
    constexpr double kMmToPx = 96.0 / 25.4;
    const double binWidthPx = binWidthMm * kMmToPx;
    Bin bin(binWidthPx);

    qWarning() << "";
    qWarning() << "Bin:" << binWidthMm << "mm x unlimited (strip)";

    std::vector<Placement> placements;
    const bool noNesting = parser.isSet(QStringLiteral("no-nesting"));
    if (noNesting) {
        qWarning() << "Nesting disabled — pieces stay at original positions";
        for (size_t i = 0; i < fp.polygons.size(); ++i)
            placements.push_back({0, 0.0, 0.0});
    } else {
        Engine::Config cfg;
        cfg.gap = 5.0 * kMmToPx;
        Engine engine(cfg);

        qWarning() << "Nesting...";
        const auto t0 = std::chrono::steady_clock::now();
        placements = engine.place(bin, fp.polygons);
        const auto t1 = std::chrono::steady_clock::now();

        const double elapsed = std::chrono::duration<double>(t1 - t0).count();
        qWarning() << "Done in" << elapsed << "s";
        qWarning() << "Placements:" << placements.size();
    }

    // ── 4. Compute used height ─────────────────────────────────────────────
    double usedHeightMm = 0;
    for (size_t i = 0; i < placements.size() && i < fp.polygons.size(); ++i) {
        int64_t yMin = std::numeric_limits<int64_t>::max();
        int64_t yMax = std::numeric_limits<int64_t>::min();
        for (const auto &pt : fp.polygons[i]) {
            if (pt.y < yMin) yMin = pt.y;
            if (pt.y > yMax) yMax = pt.y;
        }
        const double height = (yMax - yMin) / 1e6;           // Clipper2 → SVG px
        const double bottom = placements[i].y + height;       // SVG px
        if (bottom > usedHeightMm)
            usedHeightMm = bottom;
    }
    usedHeightMm *= 25.4 / 96.0;  // SVG px → mm
    if (usedHeightMm < 100) usedHeightMm = 100; // minimum page height

    qWarning() << "Used height:" << usedHeightMm << "mm";

    // ── 5. Generate nested SVG ──────────────────────────────────────────────
    qWarning() << "";
    qWarning() << "Generating nested SVG...";
    NestedSvgWriter writer;
    if (!writer.write(svgPath, nestedSvg, placements, fp, binWidthMm, usedHeightMm)) {
        qWarning() << "Failed to write nested SVG";
        return 1;
    }

    // ── 6. Export PDF via SvgToOcgPdf ────────────────────────────────────────
    qWarning() << "";
    qWarning() << "Converting to PDF OCG...";
    SvgToOcgPdf converter;
    if (!converter.convert(nestedSvg, outputPdf)) {
        qWarning() << "PDF conversion failed";
        return 1;
    }

    qWarning() << "Done:" << outputPdf;
    return 0;
}
