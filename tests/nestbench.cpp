/************************************************************************
 **
 **  @file   nestbench.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   12/09/2026
 **
 **  @brief  Harnais headless : rejoue le pipeline de nesting complet sans UI,
 **          et écrit le SVG niché. Sert d'instrument de vérification pour la
 **          scission Rabnest → Trame + Rabnest.
 **
 **  Usage : nestbench <entree.svg> <largeur_bin_mm> <sortie.svg>
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

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <QVector>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "io/NestedSvgWriter.h"
#include "io/SvgReader.h"
#include "nesting/Bin.h"
#include "nesting/Engine.h"
#include "nesting/Footprint.h"

// ═══════════════════════════════════════════════════════════════════════════
//  Réplique EXACTE de NestingDialog::runPipeline() (src/ui/NestingDialog.cpp).
//
//  Mêmes constantes, mêmes planchers, même ordre d'appels. Toute divergence
//  ferait diverger le SVG de sortie et rendrait la comparaison avant/après
//  ininterprétable.
//
//  ⚠️ Duplication assumée et temporaire : ces ~40 lignes de géométrie vivent
//  aujourd'hui dans l'UI alors qu'elles relèvent du noyau. Elles seront
//  extraites dans Trame lors d'un chantier dédié — à ce moment-là, ce fichier
//  appellera la fonction extraite au lieu de la recopier.
// ═══════════════════════════════════════════════════════════════════════════

struct NestingResult
{
    bool                   success      = false;
    QString                error;
    Footprint::Result      fp;
    std::vector<Placement> placements;
    double                 usedHeightMm = 0;
    double                 elapsed      = 0;
    int                    totalPieces  = 0;
};

static NestingResult runPipeline(const QString &svgPath, double binWidthMm)
{
    NestingResult r;
    constexpr double kMmToPx = 96.0 / 25.4;

    // ── 1. Read SVG ───────────────────────────────────────────────────────
    QVector<SizeLayer> layers;
    SvgReader reader;
    if (!reader.read(svgPath, layers)) {
        r.error = QStringLiteral("Failed to parse SVG.");
        return r;
    }

    for (const auto &layer : layers)
        r.totalPieces += layer.pieces.size();

    // ── 2. Compute footprints ─────────────────────────────────────────────
    r.fp = Footprint::compute(layers);
    if (r.fp.polygons.empty()) {
        r.error = QStringLiteral("No footprints — nothing to nest.");
        return r;
    }

    // ── 3. Nesting ────────────────────────────────────────────────────────
    const double binWidthPx = binWidthMm * kMmToPx;
    const bool isA0 = (binWidthMm <= 841.0);
    const double obsWPx = isA0 ? 210.0 * kMmToPx : 0.0;
    const double obsHPx = isA0 ? 297.0 * kMmToPx : 0.0;
    Bin bin(binWidthPx, 5.0 * kMmToPx, obsWPx, obsHPx);

    Engine::Config cfg;
    cfg.gap = 5.0 * kMmToPx;
    Engine engine(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    r.placements = engine.place(bin, r.fp.polygons);
    const auto t1 = std::chrono::steady_clock::now();
    r.elapsed = std::chrono::duration<double>(t1 - t0).count();

    // ── 4. Compute used height ────────────────────────────────────────────
    // Engine::UsedHeight is the single implementation of the envelope; this
    // harness used to carry its own copy, as did the (now deleted) UI.
    r.usedHeightMm = Engine::UsedHeight(r.placements, r.fp.polygons, bin);
    r.usedHeightMm *= 25.4 / 96.0;
    if (r.usedHeightMm < 100) r.usedHeightMm = 100;
    const double obstacleBottomMm = (bin.marginMm() + bin.obstacleHeightMm()) * 25.4 / 96.0;
    if (r.usedHeightMm < obstacleBottomMm) r.usedHeightMm = obstacleBottomMm;

    r.success = true;
    return r;
}

// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    if (argc != 4) {
        std::fprintf(stderr,
                     "Usage : nestbench <entree.svg> <largeur_bin_mm> <sortie.svg>\n");
        return 2;
    }

    const QString inputPath  = QString::fromLocal8Bit(argv[1]);
    const double  binWidthMm = QString::fromLocal8Bit(argv[2]).toDouble();
    const QString outputPath = QString::fromLocal8Bit(argv[3]);

    if (binWidthMm <= 0.0) {
        std::fprintf(stderr, "Largeur de bin invalide.\n");
        return 2;
    }

    const NestingResult r = runPipeline(inputPath, binWidthMm);
    if (!r.success) {
        std::fprintf(stderr, "Echec : %s\n", qPrintable(r.error));
        return 1;
    }

    NestedSvgWriter writer;
    if (!writer.write(inputPath, outputPath, r.placements, r.fp, binWidthMm, r.usedHeightMm)) {
        std::fprintf(stderr, "Echec : ecriture du SVG impossible.\n");
        return 1;
    }

    std::printf("%s | bin %.0f mm | %d pieces | %zu placements | hauteur %.3f mm | %.3f s\n",
                qPrintable(QFileInfo(inputPath).fileName()),
                binWidthMm,
                r.totalPieces,
                r.placements.size(),
                r.usedHeightMm,
                r.elapsed);

    return 0;
}
