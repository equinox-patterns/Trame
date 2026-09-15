/************************************************************************
 **
 **  @file   gapbench.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   15/09/2026
 **
 **  @brief  Instrument de mesure : rejoue le nesting et MESURE le
 **          dégagement réellement obtenu entre les contours posés, au lieu
 **          de le déduire de la lecture du code.
 **
 **          Une seule question posée : le jeu demandé (`gap`, en mm) est-il
 **          l'écart effectif entre deux contours, ou seulement la moitié ?
 **          Le moteur gonfle la pièce de référence de gap/2 et n'inflate pas
 **          l'autre — d'où le soupçon. Mesurer à deux jeux tranche : si le
 **          dégagement mesuré double quand le jeu demandé double, le facteur
 **          est confirmé.
 **
 **  Usage : gapbench <entree.svg> <largeur_bin_mm> <jeu_mm> [colonnes hauteur_cadre_mm]
 **
 **          Sans les deux derniers arguments, l'instrument ne mesure que le jeu.
 **          Avec eux, il construit la grille des cadres — exactement comme
 **          Ostara la construit — et compte les bords d'empreinte trop proches
 **          d'une ligne de coupe. La tolérance est le jeu lui-même : c'est la
 **          décision de Sophie du 15-09-2026.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <QVector>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "io/SvgReader.h"
#include "nesting/Bin.h"
#include "nesting/Engine.h"
#include "nesting/Footprint.h"

// ═══════════════════════════════════════════════════════════════════════════
//  La construction du Bin et du Config reproduit celle de nestbench.cpp — le
//  harnais de non-régression — pour que la mesure porte sur les conditions
//  réelles et non sur un cas de laboratoire. Duplication assumée : nestbench
//  fige le jeu à 5 mm, cet instrument doit le faire varier.
// ═══════════════════════════════════════════════════════════════════════════

namespace {

constexpr double kMmToPx = 96.0 / 25.4;

/// Les polygones circulent en pixels SVG multipliés par 1e6 (SvgReader), alors
/// que le moteur ramène le placement en pixels (Engine.cpp:309, `pl.x = px /
/// kScale`). Les deux ne sont donc PAS dans la même unité, et toute
/// reconstruction de contour absolu doit remettre le placement à l'échelle des
/// polygones. Les oublier fait glisser une pièce d'un facteur 1e6.
constexpr double kScale = 1e6;

constexpr double kPi = 3.14159265358979323846;

using Clipper2Lib::Path64;
using Clipper2Lib::Point64;

/// Boîte englobante d'un contour. Le type est celui du moteur (Engine.h) : un
/// second BBox local, même identique, rendrait le nom ambigu à la compilation.
BBox Bounds(const Path64 &path)
{
    BBox b{0, 0, 0, 0};
    if (path.empty())
    {
        return b;
    }

    b.xMin = b.xMax = path[0].x;
    b.yMin = b.yMax = path[0].y;

    for (const Point64 &p : path)
    {
        b.xMin = std::min(b.xMin, p.x);
        b.yMin = std::min(b.yMin, p.y);
        b.xMax = std::max(b.xMax, p.x);
        b.yMax = std::max(b.yMax, p.y);
    }

    return b;
}

/// Position absolue d'une pièce — réplique la transformation qu'écrit
/// NestedSvgWriter : translate(x,y) · rotate(deg) · translate(-ox,-oy), avec
/// (ox,oy) le coin min-bbox de la pièce NON tournée, et la rotation arrondie
/// sommet par sommet comme le fait Engine::rotatePath.
///
/// On mesure donc la géométrie telle qu'elle est dessinée et exportée, pas une
/// reconstruction théorique.
Path64 ToAbsolute(const Path64 &piece, const Placement &pl)
{
    const BBox bb   = Bounds(piece);
    const double rad = pl.rotation * kPi / 180.0;
    const double c   = std::cos(rad);
    const double s   = std::sin(rad);

    Path64 out;
    out.reserve(piece.size());

    for (const Point64 &p : piece)
    {
        const double lx = static_cast<double>(p.x - bb.xMin);
        const double ly = static_cast<double>(p.y - bb.yMin);

        const int64_t rx = static_cast<int64_t>(std::llround(lx * c - ly * s));
        const int64_t ry = static_cast<int64_t>(std::llround(lx * s + ly * c));

        out.push_back({rx + static_cast<int64_t>(std::llround(pl.x * kScale)),
                       ry + static_cast<int64_t>(std::llround(pl.y * kScale))});
    }

    return out;
}

/// Distance d'un point au segment [a,b].
double PointToSegment(double px, double py,
                      double ax, double ay, double bx, double by)
{
    const double dx   = bx - ax;
    const double dy   = by - ay;
    const double len2 = dx * dx + dy * dy;

    double t = 0.0;
    if (len2 > 0.0)
    {
        t = std::clamp(((px - ax) * dx + (py - ay) * dy) / len2, 0.0, 1.0);
    }

    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

double Cross(double ox, double oy, double ax, double ay, double bx, double by)
{
    return (ax - ox) * (by - oy) - (ay - oy) * (bx - ox);
}

bool SegmentsCross(const Point64 &a1, const Point64 &a2,
                   const Point64 &b1, const Point64 &b2)
{
    const double d1 = Cross(a1.x, a1.y, a2.x, a2.y, b1.x, b1.y);
    const double d2 = Cross(a1.x, a1.y, a2.x, a2.y, b2.x, b2.y);
    const double d3 = Cross(b1.x, b1.y, b2.x, b2.y, a1.x, a1.y);
    const double d4 = Cross(b1.x, b1.y, b2.x, b2.y, a2.x, a2.y);

    return ((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0))
        && ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0));
}

/// Distance entre deux segments. Zéro s'ils se croisent.
double SegmentDistance(const Point64 &a1, const Point64 &a2,
                       const Point64 &b1, const Point64 &b2)
{
    if (SegmentsCross(a1, a2, b1, b2))
    {
        return 0.0;
    }

    return std::min({PointToSegment(a1.x, a1.y, b1.x, b1.y, b2.x, b2.y),
                     PointToSegment(a2.x, a2.y, b1.x, b1.y, b2.x, b2.y),
                     PointToSegment(b1.x, b1.y, a1.x, a1.y, a2.x, a2.y),
                     PointToSegment(b2.x, b2.y, a1.x, a1.y, a2.x, a2.y)});
}

/// Dégagement minimal entre deux contours fermés.
double ContourDistance(const Path64 &a, const Path64 &b)
{
    double best = std::numeric_limits<double>::max();

    const std::size_t na = a.size();
    const std::size_t nb = b.size();

    for (std::size_t i = 0; i < na; ++i)
    {
        const Point64 &a1 = a[i];
        const Point64 &a2 = a[(i + 1) % na];

        for (std::size_t j = 0; j < nb; ++j)
        {
            const double d = SegmentDistance(a1, a2, b[j], b[(j + 1) % nb]);
            if (d < best)
            {
                best = d;

                if (best <= 0.0)
                {
                    return 0.0;
                }
            }
        }
    }

    return best;
}

/// Proximité d'un contour à une ligne de coupe — le nombre qui compte vraiment.
struct LineGap
{
    double proximity;  ///< distance du BORD de la pièce à la ligne
    bool   crosses;    ///< la ligne traverse-t-elle la pièce ?
};

/// **Franchir une ligne n'est pas le problème.** Une pièce plus grande qu'un
/// cadre est coupée : c'est le principe du tuilage, et c'est attendu.
///
/// Le problème est qu'un BORD de pièce longe la ligne — soit qu'il s'en approche
/// sans la franchir, soit qu'il ne la dépasse que d'un cheveu. Dans les deux cas
/// le trait de coupe et le contour se confondent, et la découpe abîme la pièce.
///
/// D'où la mesure. Si le contour ne franchit pas la droite : la distance
/// ordinaire. S'il la franchit : **la plus petite des deux saillies** — de
/// combien la pièce dépasse du côté le plus mince. Une pièce coupée en plein
/// milieu a deux grandes saillies et ne compte pas ; une pièce qui dépasse d'un
/// millimètre compte, et c'est exactement le cas du `Devant` sur le bord droit.
///
/// Compter les franchissements confondait ces deux situations opposées, et
/// noyait le seul cas qui compte sous les dix inévitables.
LineGap ContourToLine(const Path64 &c, int64_t coord, bool vertical)
{
    double minAbs   = std::numeric_limits<double>::max();
    double maxLeft  = 0.0;
    double maxRight = 0.0;

    for (const Point64 &p : c)
    {
        const double d = static_cast<double>((vertical ? p.x : p.y) - coord);

        minAbs = std::min(minAbs, std::abs(d));
        if (d <= 0.0) { maxLeft  = std::max(maxLeft, -d); }
        if (d >= 0.0) { maxRight = std::max(maxRight,  d); }
    }

    LineGap g;
    g.crosses   = (maxLeft > 0.0 && maxRight > 0.0);
    g.proximity = g.crosses ? std::min(maxLeft, maxRight) : minAbs;

    return g;
}

/// Écart entre deux boîtes — minorant de la distance des contours, donc un
/// filtre sûr pour écarter les paires lointaines avant le calcul exact.
double BBoxGap(const BBox &a, const BBox &b)
{
    const double dx = std::max({0.0,
                                static_cast<double>(a.xMin - b.xMax),
                                static_cast<double>(b.xMin - a.xMax)});
    const double dy = std::max({0.0,
                                static_cast<double>(a.yMin - b.yMax),
                                static_cast<double>(b.yMin - a.yMax)});
    return std::hypot(dx, dy);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    if (argc != 4 && argc != 6 && argc != 7)
    {
        std::fprintf(stderr,
                     "Usage : gapbench <entree.svg> <largeur_bin_mm> <jeu_mm>"
                     " [colonnes hauteur_cadre_mm [raffiner]]\n"
                     "        raffiner = 0 mesure la grille SANS lancer la passe 2 :\n"
                     "        c'est le « avant » reproductible.\n");
        return 2;
    }

    const QString inputPath     = QString::fromLocal8Bit(argv[1]);
    const double  binWidthMm    = QString::fromLocal8Bit(argv[2]).toDouble();
    const double  gapMm         = QString::fromLocal8Bit(argv[3]).toDouble();
    const int     columns       = (argc >= 6) ? QString::fromLocal8Bit(argv[4]).toInt() : 0;
    const double  frameHeightMm = (argc >= 6) ? QString::fromLocal8Bit(argv[5]).toDouble() : 0.0;
    const bool    refineWanted  = (argc < 7) || (QString::fromLocal8Bit(argv[6]) != QLatin1String("0"));

    if (binWidthMm <= 0.0 || gapMm < 0.0)
    {
        std::fprintf(stderr, "Largeur de bin ou jeu invalide.\n");
        return 2;
    }

    // ── 1. Lecture et empreintes ──────────────────────────────────────────
    QVector<SizeLayer> layers;
    SvgReader reader;
    if (!reader.read(inputPath, layers))
    {
        std::fprintf(stderr, "Echec : lecture du SVG impossible.\n");
        return 1;
    }

    const Footprint::Result fp = Footprint::compute(layers);
    if (fp.polygons.empty())
    {
        std::fprintf(stderr, "Echec : aucune empreinte a nidifier.\n");
        return 1;
    }

    // ── 2. Nesting, dans les conditions de nestbench ──────────────────────
    const double binWidthPx = binWidthMm * kMmToPx;
    const bool   isA0       = (binWidthMm <= 841.0);
    const double obsWPx     = isA0 ? 210.0 * kMmToPx : 0.0;
    const double obsHPx     = isA0 ? 297.0 * kMmToPx : 0.0;

    Bin bin(binWidthPx, 5.0 * kMmToPx, obsWPx, obsHPx);

    Engine::Config cfg;
    cfg.gap = gapMm * kMmToPx;

    // La grille n'entre dans le moteur que si elle est demandée ET si la passe 2
    // doit tourner : c'est ce qui rend le « avant » reproductible, et c'est aussi
    // la démonstration que le recuit lui-même ne la voit jamais.
    if (columns >= 1 && frameHeightMm > 0.0 && refineWanted)
    {
        cfg.artboardCount  = columns;
        cfg.artboardHeight = frameHeightMm * kMmToPx;
    }

    Engine engine(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<Placement> placements = engine.place(bin, fp.polygons);
    const auto t1 = std::chrono::steady_clock::now();

    // ── 3. Contours absolus ───────────────────────────────────────────────
    std::vector<Path64> contours;
    std::vector<BBox>   boxes;
    std::vector<int>    pieceOf; // index de contour -> index d'empreinte

    for (std::size_t i = 0; i < placements.size(); ++i)
    {
        // rotation == -1 marque une piece que le moteur n'a pas posee
        if (placements[i].rotation < 0.0)
        {
            continue;
        }

        contours.push_back(ToAbsolute(fp.polygons[i], placements[i]));
        boxes.push_back(Bounds(contours.back()));
        pieceOf.push_back(static_cast<int>(i));
    }

    // Les contours sont en unites moteur : pixels SVG x 1e6.
    const double kToMm = 25.4 / 96.0 / kScale;

    std::printf("jeu demande            : %8.3f mm\n", gapMm);
    std::printf("pieces posees          : %8zu / %zu\n", contours.size(), fp.polygons.size());

    // ── 4. Dégagement entre pièces ────────────────────────────────────────
    if (contours.size() >= 2)
    {
        // Les paires dont les boîtes sont proches d'abord : le meilleur
        // minimum tombe tôt, et le filtre écarte ensuite le reste d'un coup.
        struct Pair
        {
            double gap;
            int    i;
            int    j;
        };

        std::vector<Pair> pairs;
        pairs.reserve(contours.size() * (contours.size() - 1) / 2);

        for (std::size_t i = 0; i < contours.size(); ++i)
        {
            for (std::size_t j = i + 1; j < contours.size(); ++j)
            {
                pairs.push_back({BBoxGap(boxes[i], boxes[j]),
                                 static_cast<int>(i), static_cast<int>(j)});
            }
        }

        std::sort(pairs.begin(), pairs.end(),
                  [](const Pair &a, const Pair &b) { return a.gap < b.gap; });

        double best  = std::numeric_limits<double>::max();
        int    bestI = -1;
        int    bestJ = -1;

        for (const Pair &pair : pairs)
        {
            if (pair.gap >= best)
            {
                break;
            }

            const double d = ContourDistance(contours[pair.i], contours[pair.j]);
            if (d < best)
            {
                best  = d;
                bestI = pair.i;
                bestJ = pair.j;
            }
        }

        std::printf("degagement mini pieces : %8.3f mm\n", best * kToMm);

        // La paire qui realise le minimum, et son placement : sans cela un
        // resultat nul est inexploitable — on ne sait pas QUI se touche.
        for (const int c : {bestI, bestJ})
        {
            if (c < 0)
            {
                continue;
            }

            const int p = pieceOf[c];

            std::printf("   -> #%d %-20s pose (%.3f, %.3f) mm  rotation %.2f deg"
                        "  boite [%.3f, %.3f] - [%.3f, %.3f] mm\n",
                        p,
                        static_cast<std::size_t>(p) < fp.names.size() ? qPrintable(fp.names[p]) : "?",
                        placements[p].x / kMmToPx, placements[p].y / kMmToPx, placements[p].rotation,
                        static_cast<double>(boxes[c].xMin) * kToMm,
                        static_cast<double>(boxes[c].yMin) * kToMm,
                        static_cast<double>(boxes[c].xMax) * kToMm,
                        static_cast<double>(boxes[c].yMax) * kToMm);
        }
    }

    // ── 4b. Chevauchements ────────────────────────────────────────────────
    //
    // Une distance entre CONTOURS ne voit pas qu'une piece est dans une autre :
    // les contours sont loin l'un de l'autre, les surfaces sont au meme endroit.
    // C'est ce trou qui a laisse un poignet se poser dans sa manche — ce
    // compteur teste les SURFACES, et il doit valoir zero.
    {
        int overlapping = 0;

        for (std::size_t i = 0; i < contours.size(); ++i)
        {
            for (std::size_t j = i + 1; j < contours.size(); ++j)
            {
                const Clipper2Lib::Paths64 hit =
                    Clipper2Lib::Intersect({contours[i]}, {contours[j]},
                                           Clipper2Lib::FillRule::NonZero);
                if (!hit.empty())
                {
                    ++overlapping;
                }
            }
        }

        std::printf("paires en chevauchement : %6d   (doit valoir 0)\n", overlapping);
    }

    // ── 5. Dégagement au bord du bin ──────────────────────────────────────
    // Le bin est une bande : deux bords verticaux (x = 0 et x = largeur) et un
    // bord haut (y = 0). Pour une droite, la distance minimale d'un segment est
    // atteinte en un de ses sommets — le minimum sur les sommets suffit.
    if (!contours.empty())
    {
        double left  = std::numeric_limits<double>::max();
        double right = std::numeric_limits<double>::max();
        double top   = std::numeric_limits<double>::max();

        for (const Path64 &c : contours)
        {
            for (const Point64 &p : c)
            {
                left  = std::min(left, std::abs(static_cast<double>(p.x)));
                right = std::min(right, std::abs(binWidthPx * kScale - static_cast<double>(p.x)));
                top   = std::min(top, std::abs(static_cast<double>(p.y)));
            }
        }

        std::printf("degagement mini au bin : gauche %.3f  droite %.3f  haut %.3f mm\n",
                    left * kToMm, right * kToMm, top * kToMm);
    }

    // ── 6. Les lignes de coupe ────────────────────────────────────────────
    //
    // La grille est construite comme Ostara la construit (OExportArtboards::
    // Compute) : les rangees viennent de la hauteur UTILISEE, et seules les
    // lignes INTERIEURES sont des lignes de coupe sans marge. Les bords du plan
    // sont deja tenus par la marge du bin — le relevé plus haut le montre.
    //
    // La hauteur vient de Engine::UsedHeight, jamais d'une valeur locale : c'est
    // le chiffre meme qu'Ostara stocke, donc celui qui decide du nombre de pages.
    if (columns >= 1 && frameHeightMm > 0.0 && !contours.empty())
    {
        const double  kMmToEngine = kMmToPx * kScale;
        const int64_t cellW       = static_cast<int64_t>(std::llround(binWidthPx * kScale / columns));
        const int64_t cellH       = static_cast<int64_t>(std::llround(frameHeightMm * kMmToEngine));
        const int64_t tolerance   = static_cast<int64_t>(std::llround(gapMm * kMmToEngine));

        const double  usedPx     = Engine::UsedHeight(placements, fp.polygons, bin);
        const int64_t usedEngine = static_cast<int64_t>(std::llround(usedPx * kScale));

        int rows = 0;
        if (cellH > 0)
        {
            rows = std::max(1, static_cast<int>(
                std::ceil(static_cast<double>(usedEngine) / static_cast<double>(cellH) - 1e-9)));
        }

        int    grazing   = 0;
        int    crossed   = 0;
        double worstLine = std::numeric_limits<double>::max();

        for (const Path64 &c : contours)
        {
            for (int k = 1; k < columns; ++k)
            {
                const LineGap g = ContourToLine(c, cellW * k, true);
                grazing  += (g.proximity < static_cast<double>(tolerance)) ? 1 : 0;
                crossed  += g.crosses ? 1 : 0;
                worstLine = std::min(worstLine, g.proximity);
            }

            for (int j = 1; j < rows; ++j)
            {
                const LineGap g = ContourToLine(c, cellH * j, false);
                grazing  += (g.proximity < static_cast<double>(tolerance)) ? 1 : 0;
                crossed  += g.crosses ? 1 : 0;
                worstLine = std::min(worstLine, g.proximity);
            }
        }

        // Le minimum incompressible. Un contour qui atteint xMin et xMax traverse
        // TOUTE ligne verticale strictement comprise entre les deux — une courbe
        // fermée et continue ne peut pas sauter par-dessus. Le nombre de lignes
        // d'un intervalle de largeur W ne peut pas descendre sous ceil(W/pas) - 1,
        // et ce plancher ne dépend que de la TAILLE de la pièce, pas de l'endroit
        // où elle est posée. Si le compte mesuré égale ce plancher, aucun
        // ajustement ne peut retirer un seul franchissement.
        int minCrossings = 0;

        for (const BBox &b : boxes)
        {
            const double w = static_cast<double>(b.xMax - b.xMin);
            const double h = static_cast<double>(b.yMax - b.yMin);

            if (cellW > 0)
            {
                minCrossings += std::max(0, static_cast<int>(std::ceil(w / cellW)) - 1);
            }
            if (cellH > 0)
            {
                minCrossings += std::max(0, static_cast<int>(std::ceil(h / cellH)) - 1);
            }
        }

        const double envelopeMm = rows * frameHeightMm;
        const double usedMm     = usedPx / kMmToPx;

        std::printf("grille                 : %d colonnes x %d rangees (cadre %.1f mm)\n",
                    columns, rows, frameHeightMm);
        std::printf("hauteur                : utilisee %.3f mm   enveloppe %.3f mm"
                    "   bande morte %.3f mm\n",
                    usedMm, envelopeMm, envelopeMm - usedMm);
        std::printf("lignes de coupe        : %d verticales + %d horizontales\n",
                    std::max(0, columns - 1), std::max(0, rows - 1));
        std::printf("BORDS longeant une ligne : %6d   (a moins de %.1f mm)\n",
                    grazing, gapMm);
        std::printf("   dont franchissements   : %6d   (dont %d incompressibles"
                    " — la taille de la piece, pas son placement)\n",
                    crossed, minCrossings);
        if (worstLine < std::numeric_limits<double>::max())
        {
            std::printf("bord le plus proche d'une ligne : %6.3f mm\n", worstLine * kToMm);
        }
    }

    std::printf("duree                  : %8.3f s\n",
                std::chrono::duration<double>(t1 - t0).count());

    return 0;
}
