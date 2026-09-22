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
 **  Usage : gapbench <entree.svg> <largeur_bin_mm> <jeu_mm>
 **                  [colonnes hauteur_cadre_mm [raffiner [pages_interdites]]]
 **
 **          Sans les deux premiers arguments optionnels, l'instrument ne mesure
 **          que le jeu. Avec eux, il construit la grille des cadres — exactement
 **          comme Ostara la construit — et compte les bords d'empreinte trop
 **          proches d'une ligne de coupe. La tolérance est le jeu lui-même :
 **          c'est la décision de Sophie du 15-09-2026.
 **
 **          `raffiner` = 0 mesure la grille SANS lancer la passe 2 — c'est le
 **          « avant » reproductible, et l'inverse de ce que le nom laisse croire.
 **
 **          `pages_interdites` = « ligne:colonne,ligne:colonne », 1-based. Chaque
 **          case nommée devient une zone interdite du Bin : le moteur doit poser
 **          toutes les pièces ailleurs, ou échouer. C'est le mécanisme des « pages
 **          interdites » (ExportPDF.md §16), mesuré AVANT qu'Ostara ne s'en serve.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <QStringList>
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
#include "nesting/TiledEngine.h"

// ═══════════════════════════════════════════════════════════════════════════
//  La construction du Bin et du Config reproduit celle de nestbench.cpp — le
//  harnais de non-régression — pour que la mesure porte sur les conditions
//  réelles et non sur un cas de laboratoire. Duplication assumée : nestbench
//  fige le jeu à 5 mm, cet instrument doit le faire varier.
//
//  Une exception, et une seule : l'obstacle A4, que nestbench pose dès que le bin
//  tombe à 841 mm, n'est posé ici que sur un run NON tuilé. Voir §2.
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

/// La marge du document, en millimètres. Le JEU, lui, varie : c'est l'argument 3.
/// Nommée parce que le plancher de surface la retire deux fois en largeur, et qu'un
/// second `5.0` écrit ailleurs serait un second endroit à faire diverger.
constexpr double kMarginMm = 5.0;

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

/// La cellule (k, j) de la grille, en unités moteur, comme un contour fermé : le
/// rectangle à quatre sommets, dans l'ordre. Sert à demander à Clipper2 si une
/// pièce touche cette cellule — la question même que pose la règle d'impression.
Path64 RectPath(int64_t x, int64_t y, int64_t w, int64_t h)
{
    return Path64{{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
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

/// « ligne:colonne » ou « mm:x:y:largeur:hauteur », séparés par des virgules.
///
/// La PREMIÈRE forme — 1-based, comme on lit un plan — devient UNE cellule de la
/// grille, construite comme Ostara construit la sienne (`OExportArtboards::Compute`) :
/// la largeur du bin divisée par le nombre de colonnes, la hauteur du cadre telle
/// quelle. Une « page interdite » est une case de la grille, et rien d'autre.
///
/// La SECONDE est un rectangle libre, en millimètres depuis le coin du bin, et elle
/// existe parce que la première ne sait nommer qu'une CASE ENTIÈRE : le bloc de page
/// de garde — 210 x 297 dans une feuille de 297 x 420 — n'est ni une cellule ni un
/// ratio, donc le cas réel n'était pas mesurable. Ses quatre champs sont séparés par
/// « : » comme les deux de la première forme, et NON par des virgules : la virgule
/// sépare déjà les éléments de la liste.
///
/// Rend false quand la spécification est illisible : un instrument qui mesure
/// autre chose que ce qu'on lui a demandé ne mesure rien.
bool ParseZones(const QString &spec, double binWidthPx, int columns,
                double frameHeightMm, std::vector<Bin::Zone> &out)
{
    out.clear();
    if (spec.isEmpty())
    {
        return true;
    }

    const QStringList elements = spec.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString &element : elements)
    {
        const QStringList fields = element.split(QLatin1Char(':'), Qt::SkipEmptyParts);

        // ── Le rectangle libre, en millimètres ────────────────────────────
        if (fields.size() == 5
            && fields[0].compare(QLatin1String("mm"), Qt::CaseInsensitive) == 0)
        {
            bool okX = false;
            bool okY = false;
            bool okW = false;
            bool okH = false;
            const double x = fields[1].toDouble(&okX);
            const double y = fields[2].toDouble(&okY);
            const double w = fields[3].toDouble(&okW);
            const double h = fields[4].toDouble(&okH);

            if (!okX || !okY || !okW || !okH || w <= 0.0 || h <= 0.0)
            {
                std::fprintf(stderr,
                             "Zone illisible : « %s » (attendu mm:x:y:largeur:hauteur,"
                             " en millimètres, largeur et hauteur positives).\n",
                             qPrintable(element));
                return false;
            }

            out.push_back({x * kMmToPx, y * kMmToPx, w * kMmToPx, h * kMmToPx});
            continue;
        }

        // ── La case de la grille ──────────────────────────────────────────
        if (columns < 1 || frameHeightMm <= 0.0)
        {
            std::fprintf(stderr,
                         "Les pages interdites en cases exigent <colonnes> et"
                         " <hauteur_cadre_mm>.\n");
            return false;
        }

        if (fields.size() != 2)
        {
            std::fprintf(stderr,
                         "Page interdite illisible : « %s » (attendu ligne:colonne,"
                         " ou mm:x:y:largeur:hauteur).\n",
                         qPrintable(element));
            return false;
        }

        const int row = fields[0].toInt();
        const int col = fields[1].toInt();
        if (row < 1 || col < 1 || col > columns)
        {
            std::fprintf(stderr, "Page interdite hors grille : %d:%d (%d colonnes).\n",
                         row, col, columns);
            return false;
        }

        const double cellWpx = binWidthPx / columns;
        const double cellHpx = frameHeightMm * kMmToPx;
        out.push_back({(col - 1) * cellWpx, (row - 1) * cellHpx, cellWpx, cellHpx});
    }

    return true;
}

/// Une zone du Bin, dans les unités du moteur — c'est-à-dire en pixels multipliés
/// par `kScale`, comme les contours qu'on lui confronte.
auto ZonePath(const Bin::Zone &zone) -> Path64
{
    const int64_t x0 = static_cast<int64_t>(std::llround(zone.x * kScale));
    const int64_t y0 = static_cast<int64_t>(std::llround(zone.y * kScale));
    const int64_t x1 = static_cast<int64_t>(std::llround((zone.x + zone.width) * kScale));
    const int64_t y1 = static_cast<int64_t>(std::llround((zone.y + zone.height) * kScale));

    return {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    if (argc != 4 && argc != 6 && argc != 7 && argc != 8 && argc != 9)
    {
        std::fprintf(stderr,
                     "Usage : gapbench <entree.svg> <largeur_bin_mm> <jeu_mm>"
                     " [colonnes hauteur_cadre_mm [raffiner [pages_interdites [moteur]]]]\n"
                     "        raffiner = 0 mesure la grille SANS lancer la passe 2 :\n"
                     "        c'est le « avant » reproductible.\n"
                     "        pages_interdites = « ligne:colonne,ligne:colonne » (1-based) :\n"
                     "        chaque case nommee est une page sur laquelle RIEN ne doit\n"
                     "        s'imprimer. Le moteur doit la contourner, ou echouer.\n"
                     "        Un element peut aussi etre « mm:x:y:largeur:hauteur », en\n"
                     "        millimetres depuis le coin du bin : un rectangle libre, la\n"
                     "        ou la forme en cases ne sait nommer qu'une case entiere.\n"
                     "        Exemple : « mm:0:0:210:297 » est le bloc d'une page de garde.\n"
                     "        moteur = bande (defaut, minimise une hauteur) ou tiled\n"
                     "        (paie en pages : grosses pieces d'abord, puis les petites\n"
                     "        dans les trous au vrai contour).\n");
        return 2;
    }

    const QString inputPath     = QString::fromLocal8Bit(argv[1]);
    const double  binWidthMm    = QString::fromLocal8Bit(argv[2]).toDouble();
    const double  gapMm         = QString::fromLocal8Bit(argv[3]).toDouble();
    const int     columns       = (argc >= 6) ? QString::fromLocal8Bit(argv[4]).toInt() : 0;
    const double  frameHeightMm = (argc >= 6) ? QString::fromLocal8Bit(argv[5]).toDouble() : 0.0;
    const bool    refineWanted  = (argc < 7) || (QString::fromLocal8Bit(argv[6]) != QLatin1String("0"));
    const QString zoneSpec      = (argc >= 8) ? QString::fromLocal8Bit(argv[7]) : QString();
    const QString engineSpec    = (argc >= 9) ? QString::fromLocal8Bit(argv[8]) : QString();

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

    // ── 2. Nesting ────────────────────────────────────────────────────────
    //
    // L'obstacle A4 de nestbench n'est posé QUE sur un run NON tuilé.
    //
    // La règle d'origine — « bin ≤ 841 mm » — décrit le roulé A0, dont c'est la
    // largeur. Sur un tuilage elle est simplement fausse : un bin étroit y est un
    // petit nombre de colonnes, et le mode Export ne pose jamais d'obstacle
    // (Nesting.md:129 — « Aucun obstacle dans le bin »). Le recuit en serait
    // perturbé pour rien, et l'obstacle occuperait une case que la grille compte
    // déjà.
    //
    // ⚠️ La raison qui figurait ici — « la passe 2 refuse carrément de tourner »
    // — n'est plus vraie depuis le 17-09-2026 : la passe 2 cesse de refuser et
    // respecte désormais les zones interdites. L'écart A0 / tuilage reste, mais
    // il ne tient plus à un refus.
    const double binWidthPx = binWidthMm * kMmToPx;
    const bool   isA0       = (binWidthMm <= 841.0) && (columns < 1);
    const double obsWPx     = isA0 ? 210.0 * kMmToPx : 0.0;
    const double obsHPx     = isA0 ? 297.0 * kMmToPx : 0.0;

    // ── Les pages interdites ──────────────────────────────────────────────
    //
    // Une case de la grille nommée par l'appelant devient une zone interdite du
    // Bin — la même mécanique que l'obstacle A4, sans le coin ni la taille
    // figée. Le moteur doit alors poser toutes les pièces ailleurs, ou échouer :
    // c'est cet échec, et le nombre de feuilles imprimées, que l'instrument
    // chiffre — avant qu'Ostara n'écrive la boucle qui s'en servira.
    std::vector<Bin::Zone> zones;
    if (!ParseZones(zoneSpec, binWidthPx, columns, frameHeightMm, zones))
    {
        return 2;
    }

    if (!zones.empty())
    {
        // « zones » et non « pages » : un élément peut être un rectangle libre, qui n'est
        // la case d'aucune grille. Le mot doit dire ce que l'instrument a reçu.
        std::printf("zones interdites : %s  (%zu)\n",
                    qPrintable(zoneSpec), zones.size());
        for (std::size_t i = 0; i < zones.size(); ++i)
        {
            const Bin::Zone &z = zones[i];
            std::printf("  zone %zu : x=%.2f y=%.2f  %.2f x %.2f mm\n", i + 1,
                        z.x / kMmToPx, z.y / kMmToPx,
                        z.width / kMmToPx, z.height / kMmToPx);
        }
    }

    // L'obstacle A4 et les pages interdites ne se cumulent pas : le premier est
    // un cas non tuilé, les secondes exigent une grille. Le constructeur qui
    // reçoit une liste explicite ne pose donc rien au coin.
    const Bin bin = zones.empty()
        ? Bin(binWidthPx, kMarginMm * kMmToPx, obsWPx, obsHPx)
        : Bin(binWidthPx, kMarginMm * kMmToPx, std::move(zones));

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

    // ── Le moteur ─────────────────────────────────────────────────────────
    //
    // `bande` (défaut) est le moteur historique : il minimise une HAUTEUR, la
    // page n'existe pas pour lui, et la grille est une grille posée après coup
    // sur le résultat.
    //
    // `tiled` est le second moteur : il paie en PAGES. Les grosses pièces
    // d'abord, puis les petites glissées dans les trous au VRAI CONTOUR — c'est
    // l'imbrication dans le creux d'une courbe qui rend la place. Il n'ouvre
    // une rangée que lorsqu'aucune pièce restante n'entre dedans.
    const bool useTiled =
        (engineSpec.compare(QLatin1String("tiled"), Qt::CaseInsensitive) == 0);

    if (!engineSpec.isEmpty() && !useTiled
        && engineSpec.compare(QLatin1String("bande"), Qt::CaseInsensitive) != 0)
    {
        std::fprintf(stderr, "Moteur inconnu : « %s » (attendu bande ou tiled).\n",
                     qPrintable(engineSpec));
        return 2;
    }
    if (useTiled && (columns < 1 || frameHeightMm <= 0.0))
    {
        std::fprintf(stderr, "Le moteur tiled exige <colonnes> et <hauteur_cadre_mm>.\n");
        return 2;
    }

    std::vector<Placement> placements;
    std::chrono::steady_clock::time_point t0, t1;

    if (useTiled)
    {
        TiledEngine::Spec spec;
        spec.sheetWidth    = binWidthPx / columns;
        spec.sheetHeight   = frameHeightMm * kMmToPx;
        spec.gap           = gapMm * kMmToPx;
        spec.allowRotation = true;

        // Les deux surfaces et les deux planchers — les chiffres de Sophie.
        const TiledEngine::Floors fl = TiledEngine::ComputeFloors(fp.polygons, spec);
        const double kMm2 = kMmToPx * kMmToPx;

        std::printf("moteur                 :    tiled\n");
        std::printf("surface empreintes     : %10.1f mm2  (%8.1f cm2)\n",
                    fl.footprintArea / kMm2, fl.footprintArea / kMm2 / 100.0);
        std::printf("surface holders        : %10.1f mm2  (%8.1f cm2)\n",
                    fl.holderArea / kMm2, fl.holderArea / kMm2 / 100.0);
        std::printf("marge (holders - empr.) : %9.1f mm2  (%8.1f cm2)\n",
                    fl.margin / kMm2, fl.margin / kMm2 / 100.0);
        std::printf("plancher de feuilles   : %6d   (surface des empreintes — optimiste)\n",
                    fl.footprintSheets);
        std::printf("plancher des holders   : %6d   (surface des holders — realiste)\n",
                    fl.holderSheets);

        t0 = std::chrono::steady_clock::now();
        placements = TiledEngine(spec).place(bin, fp.polygons);
        t1 = std::chrono::steady_clock::now();
    }
    else
    {
        const Engine engine(cfg);
        t0 = std::chrono::steady_clock::now();
        placements = engine.place(bin, fp.polygons);
        t1 = std::chrono::steady_clock::now();
    }

    // ── 2 bis. Un plan incomplet ne se mesure pas ─────────────────────────
    //
    // Une pièce qui n'a pas pu être placée porte `rotation < 0` : c'est le marqueur
    // que le moteur pose sur un échec, et le seul que la passe 2 et Ostara lisent.
    // Tout ce qui suit mesurerait une géométrie qui n'existe pas — le 17-09-2026,
    // un bin trop étroit a rendu ici « remplissage 302 % » et « 4 colonnes sur 3 »,
    // c'est-à-dire des chiffres tirés de pièces fantômes restées à l'origine. Un
    // tel run ne se mesure pas, il se signale.
    std::size_t unplaced = 0;
    for (const Placement &p : placements)
    {
        if (p.rotation < 0.0)
        {
            ++unplaced;
        }
    }

    if (unplaced > 0)
    {
        std::printf("NON PLACE               : %8zu piece(s) sur %zu"
                    " — aucune mise en page n'existe, rien a mesurer\n",
                    unplaced, placements.size());
        return 2;
    }

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

    // ── 4c. Les zones interdites ──────────────────────────────────────────
    //
    // Le compteur du dessus confronte les pièces ENTRE ELLES. Celui-ci les confronte
    // à ce sur quoi rien ne doit s'imprimer — la page de garde, le jour venu.
    //
    // Sans lui l'instrument imprime les zones et les placements côte à côte et
    // n'affirme RIEN sur leur relation : il était incapable d'échouer sur le défaut
    // même qu'il existe pour attraper, et un moteur qui ignore `Bin::zones()` — ce que
    // fait TiledEngine aujourd'hui — passait pour un moteur qui les respecte.
    //
    // Les zones sont lues SUR LE BIN, jamais sur la liste d'arguments : c'est ce que
    // le moteur a réellement reçu, et une zone perdue en route se voit ici.
    if (!bin.zones().empty() && !contours.empty())
    {
        Clipper2Lib::Paths64 forbidden;
        forbidden.reserve(bin.zones().size());
        for (const Bin::Zone &z : bin.zones())
        {
            forbidden.push_back(ZonePath(z));
        }

        int inside = 0;
        for (const Path64 &c : contours)
        {
            if (!Clipper2Lib::Intersect({c}, forbidden, Clipper2Lib::FillRule::NonZero).empty())
            {
                ++inside;
            }
        }

        std::printf("contours dans une zone : %6d   (doit valoir 0)\n", inside);
    }

    // ── 4 bis. La surface, et ce qu'elle interdit ─────────────────────────
    //
    // Les pièces ne se chevauchent pas — le compteur ci-dessus vient de le dire —
    // donc leur surface totale doit tenir dans la largeur utile fois la hauteur.
    // C'est un PLANCHER, pas une estimation : aucun arrangement, aucune stratégie,
    // aucun moteur ne peut descendre en dessous. Il sert à éliminer l'impossible,
    // jamais à garantir le possible — un très bon nesting remplit 75 à 80 % de la
    // place disponible, pas 100 %.
    //
    // La surface est celle des EMPREINTES telles qu'elles sont dessinées, jamais
    // celle de leur boîte englobante : une empreinte est le contour de l'union de
    // toutes les tailles d'une pièce, et son aire est celle de l'encre.
    const double usedPx = Engine::UsedHeight(placements, fp.polygons, bin);
    const double usedMm = usedPx / kMmToPx;

    double areaUnits = 0.0;
    for (const Path64 &p : fp.polygons)
    {
        areaUnits += std::abs(Clipper2Lib::Area(p));
    }
    const double areaMm2 = areaUnits * kToMm * kToMm;

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

    // ── 5 bis. Le plancher de surface ─────────────────────────────────────
    //
    // La hauteur incompressible, et le taux de remplissage qui en découle. Le
    // second est le chiffre utile : confronté au plafond réel d'un nesting de
    // formes irrégulières — 75 à 80 % — il dit si le moteur laisse quelque chose
    // sur la table, ou s'il est déjà au bout de ce que les formes permettent.
    // Sans lui, on ne sait pas si un objectif peut encore gagner quoi que ce
    // soit, et on réécrit l'objectif pour rien.
    if (!contours.empty())
    {
        const double usableWidthMm = binWidthMm - 2.0 * kMarginMm;
        const double inkHeightMm   = usedMm - kMarginMm;
        const double floorMm       = usableWidthMm > 0.0 ? areaMm2 / usableWidthMm : 0.0;
        const double fill          = (usableWidthMm > 0.0 && inkHeightMm > 0.0)
                                         ? areaMm2 / (usableWidthMm * inkHeightMm) * 100.0
                                         : 0.0;

        std::printf("surface empreintes     : %10.1f mm2  (%8.1f cm2)\n", areaMm2, areaMm2 / 100.0);
        std::printf("plancher de hauteur    : %10.3f mm   (surface / largeur utile %.1f mm)\n",
                    floorMm, usableWidthMm);
        std::printf("taux de remplissage    : %10.1f %%\n", fill);
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

        // Combien de colonnes l'encre atteint réellement. C'est la question du
        // tuilage : une colonne que rien ne touche n'est pas imprimée — la règle
        // est qu'une feuille vide ne compte pas — donc le nombre de colonnes du bin
        // n'est pas le nombre de colonnes imprimées.
        double maxX = 0.0;
        for (const Path64 &c : contours)
        {
            for (const Point64 &p : c)
            {
                maxX = std::max(maxX, static_cast<double>(p.x));
            }
        }
        const int usedColumns = cellW > 0
                                    ? static_cast<int>(maxX / static_cast<double>(cellW)) + 1
                                    : columns;

        // Le plancher de feuilles : la surface doit tenir dans les feuilles
        // imprimées, donc feuilles >= surface / surface d'une feuille. De même
        // nature que le plancher de hauteur — il élimine l'impossible.
        const double cellAreaMm2 = (binWidthMm / columns) * frameHeightMm;
        const int    floorSheets = cellAreaMm2 > 0.0
                                       ? static_cast<int>(std::ceil(areaMm2 / cellAreaMm2))
                                       : 0;

        // Le compte EXACT des feuilles imprimées, et c'est celui-là qui décide.
        //
        // Une cellule qui ne contient rien n'est pas imprimée — la règle du mode
        // Export — donc une rangée peut perdre une page et garder les autres. Le
        // produit « colonnes atteintes x rangées » est le contour de la boîte, pas
        // le nombre de pages : il majore, et comparer deux dispositions sur un
        // majorant, c'est comparer deux choses qu'on n'a pas mesurées.
        int printed = 0;
        QVector<bool> usedCells(rows * columns, false);
        for (int j = 0; j < rows; ++j)
        {
            for (int k = 0; k < columns; ++k)
            {
                const Path64 cell = RectPath(cellW * k, cellH * j, cellW, cellH);

                for (const Path64 &c : contours)
                {
                    if (!Clipper2Lib::Intersect({c}, {cell}, Clipper2Lib::FillRule::NonZero).empty())
                    {
                        ++printed;
                        usedCells[j * columns + k] = true;
                        break;
                    }
                }
            }
        }

        const double envelopeMm = rows * frameHeightMm;

        std::printf("grille                 : %d colonnes x %d rangees (cadre %.1f mm)\n",
                    columns, rows, frameHeightMm);
        std::printf("colonnes atteintes     : %6d sur %d\n", usedColumns, columns);
        std::printf("hauteur                : utilisee %.3f mm   enveloppe %.3f mm"
                    "   bande morte %.3f mm\n",
                    usedMm, envelopeMm, envelopeMm - usedMm);
        std::printf("feuilles au plus       : %6d   (colonnes atteintes x rangees)\n",
                    usedColumns * rows);
        std::printf("feuilles imprimees     : %6d   (cellules qui portent quelque chose)\n",
                    printed);

        // OÙ est le trou, en ligne:colonne — parce qu'un compte ne dit pas où il est, et que deux
        // plans au même compte peuvent avoir leur case vide à deux endroits qui ne se valent pas :
        // une case vide au COIN est la place d'une page de garde, une case vide au fond est du
        // papier perdu. C'est ce que le mode Export a besoin de distinguer.
        {
            QStringList emptyCells;
            for (int j = 0; j < rows; ++j)
            {
                for (int k = 0; k < columns; ++k)
                {
                    if (!usedCells[j * columns + k])
                    {
                        emptyCells.append(
                            QStringLiteral("%1:%2").arg(j + 1).arg(k + 1));
                    }
                }
            }

            if (!emptyCells.isEmpty())
            {
                std::printf("cellules vides         : %s\n",
                            qPrintable(emptyCells.join(QLatin1Char(' '))));
            }
        }

        // ── Combien de pièces par case imprimée ──────────────────────────
        //
        // C'est la TAILLE DU CHANTIER, et rien d'autre : une passe qui doit vider une case du
        // fond déplace les pièces de cette case, et c'est ce compte-là qui dit si l'on parle de
        // trois déplacements ou de quarante. Le temps d'une passe se déduit de ce chiffre avant
        // qu'une ligne de passe soit écrite.
        //
        // ⚠️ Une pièce à cheval sur une frontière est comptée dans les DEUX cases : c'est
        // exactement ce qui rend une case imprimée, et c'est pourquoi l'alignement compte.
        {
            QStringList counts;
            for (int j = 0; j < rows; ++j)
            {
                for (int k = 0; k < columns; ++k)
                {
                    if (!usedCells[j * columns + k])
                    {
                        continue;
                    }

                    const Path64 cell = RectPath(cellW * k, cellH * j, cellW, cellH);
                    int touching = 0;
                    for (const Path64 &c : contours)
                    {
                        if (!Clipper2Lib::Intersect({c}, {cell},
                                                    Clipper2Lib::FillRule::NonZero).empty())
                        {
                            ++touching;
                        }
                    }

                    counts.append(QStringLiteral("%1:%2=%3").arg(j + 1).arg(k + 1).arg(touching));
                }
            }

            std::printf("pieces par case        : %s\n",
                        qPrintable(counts.join(QLatin1Char(' '))));
        }
        std::printf("plancher de feuilles   : %6d   (surface / surface d'une feuille)\n",
                    floorSheets);
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
