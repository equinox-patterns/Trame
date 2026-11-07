/************************************************************************
 **
 **  @file   TiledEngine.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   17/09/2026
 **
 **  @brief  Moteur de nidification du format tuilé — voir TiledEngine.h.
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

#include "TiledEngine.h"
#include "Bin.h"
#include "ZoneGeom.h"

#include <algorithm>
#include <cmath>
#include <set>

using NestGeom::kScale;
using NestGeom::bboxFromPath;
using NestGeom::makeRect;
using NestGeom::translatePath;
using NestGeom::translatePaths;
using NestGeom::rotatePath;
using NestGeom::findBottomLeft;
using NestGeom::nfpOutsideIFP;

namespace {

/// A piece the engine has already put down, in engine coordinates.
struct Placed
{
    int     pieceIdx;
    int     rotIdx;
    int64_t px;   ///< Min-bbox corner of the piece BEFORE rotation
    int64_t py;
};

/// Les bandes d'un axe où un bord du contour tomberait sur une jonction de feuilles.
///
/// **La contrainte est MONO-AXE, et c'est ce qui la rend exacte et bon marché.** La distance d'un
/// contour à une jonction VERTICALE ne dépend que de x, celle à une jonction HORIZONTALE que de y.
///
/// **Deux bandes par ligne, jamais une de plus** — et ce n'est pas une simplification, c'est
/// l'égalité. La mesure de `NestGeom::borderToLine` ne dépend que des DEUX extrémités du contour sur
/// l'axe : une pièce est « près » de la ligne quand son bord droit (resp. bas) est à moins du jeu,
/// ou son bord gauche (resp. haut), ou bien quand elle traverse mais ne dépasse pas du jeu d'un côté.
/// Ces trois cas se réduisent à deux intervalles de largeur `2 * jeu`, centrés sur `(ligne - max)`
/// et sur `(ligne - min)` ; entre les deux, la pièce traverse franchement et n'a rien à craindre.
///
/// ⚠️ **Une version « un sommet près de la ligne » a été écrite d'abord, et elle était fausse** : elle
/// interdisait aussi les pièces qui traversent une jonction avec de grandes parties de chaque côté —
/// c'est-à-dire des pièces parfaitement saines, et des plans qui auraient perdu des feuilles pour
/// rien. C'est la mesure du moteur qui fait foi, pas une idée de ce qu'elle devrait être.
///
/// Mesuré le 07-11-2026 sur un plan d'impression réel : une pièce posée à **1,7 mm** d'une jonction,
/// bord parallèle. En A4, cette bande-là est dans la zone que l'imprimante n'imprime pas — le PDF
/// était inutilisable, et rien dans ce moteur ne l'empêchait.
auto JunctionBands(const Clipper2Lib::Path64 &path,
                   const std::vector<int64_t> &lines, bool vertical,
                   int64_t gapR, int64_t spanX, int64_t spanY) -> Clipper2Lib::Paths64
{
    if (path.empty() || lines.empty())
        return {};

    const BBox bb = bboxFromPath(path);
    const int64_t lo = vertical ? bb.xMin : bb.yMin;
    const int64_t hi = vertical ? bb.xMax : bb.yMax;

    std::vector<std::pair<int64_t, int64_t>> bands;
    bands.reserve(lines.size() * 2);

    for (const int64_t line : lines)
    {
        // Le bord « max » de la pièce entre dans le jeu de la ligne, puis le bord « min ».
        bands.emplace_back(line - gapR - hi, line + gapR - hi);
        bands.emplace_back(line - gapR - lo, line + gapR - lo);
    }

    std::sort(bands.begin(), bands.end());

    std::size_t kept = 0;
    for (std::size_t i = 1; i < bands.size(); ++i)
    {
        if (bands[i].first <= bands[kept].second + 1)
            bands[kept].second = std::max(bands[kept].second, bands[i].second);
        else
            bands[++kept] = bands[i];
    }
    bands.resize(kept + 1);

    Clipper2Lib::Paths64 out;
    out.reserve(bands.size());
    for (const auto &band : bands)
    {
        out.push_back(vertical ? makeRect(band.first, -1, band.second, spanY + 1)
                               : makeRect(-1, band.first, spanX + 1, band.second));
    }
    return out;
}

/// Where the piece (pieceIdx, rIdx) may go, inside a bounded frame.
///
/// This is `Engine::placeSequential`'s inner block, with ONE difference: the
/// frame is closed on all four sides. The strip engine bounds it below and
/// lets the upper bound be `infY` — an open band, which is why the plan grows
/// whenever a piece does not fit. Here the top edge is a row of sheets, and a
/// piece that does not fit has to open one.
Clipper2Lib::Paths64 feasibleRegion(const std::vector<Placed> &placed,
                                    const std::vector<std::vector<NestGeom::RotGeom>> &rotBank,
                                    const NestGeom::RotGeom &bg,
                                    int pieceIdx, int rIdx,
                                    int64_t binW, int64_t envH, int64_t margin,
                                    int64_t sheetW, int64_t sheetH, int64_t gapR,
                                    bool closedSheets,
                                    const std::vector<NestZone::Geom> &zones,
                                    NestGeom::NFPCache &nfpCache,
                                    NestGeom::NFPCache &obstacleCache)
{
    const int64_t ifpX0 = margin - bg.bbox.xMin;
    const int64_t ifpX1 = binW - margin - bg.bbox.xMax;
    const int64_t ifpY0 = margin - bg.bbox.yMin;
    const int64_t ifpY1 = envH - margin - bg.bbox.yMax;

    Clipper2Lib::Paths64 out;
    if (ifpX1 <= ifpX0 || ifpY1 <= ifpY0)
        return out;

    Clipper2Lib::Clipper64 clipper;
    if (closedSheets)
    {
        // ── Feuilles fermées : le cadre n'est plus un rectangle, c'est l'UNION des feuilles ──
        //
        // Chaque feuille rend la fenêtre des positions de référence qui y mettent la pièce
        // ENTIÈRE, à la marge du bin sur chacun de ses quatre bords. Deux fenêtres voisines ne se
        // touchent pas dès que la marge n'est pas nulle : une pièce ne peut donc pas enjamber une
        // frontière — et il n'y a rien à soustraire après coup, la règle EST le cadre.
        //
        // C'est la MARGE du bin qui borne chaque feuille, et non le jeu entre pièces : le bord
        // d'une feuille fermée est le bord du PAPIER, pas un trait de coupe. Voir
        // TiledEngine::Spec::closedSheets.
        const int64_t cols = sheetW > 0 ? (binW + sheetW - 1) / sheetW : 0;
        Clipper2Lib::Paths64 windows;
        windows.reserve(static_cast<std::size_t>(std::max<int64_t>(cols, 1)) * 4);

        for (int64_t r = 0; r * sheetH < envH; ++r)
        {
            const int64_t y0 = r * sheetH + margin - bg.bbox.yMin;
            const int64_t y1 = (r + 1) * sheetH - margin - bg.bbox.yMax;
            if (y1 <= y0)
                continue;

            for (int64_t c = 0; c < cols; ++c)
            {
                const int64_t x0 = c * sheetW + margin - bg.bbox.xMin;
                const int64_t x1 = (c + 1) * sheetW - margin - bg.bbox.xMax;
                if (x1 <= x0)
                    continue;
                windows.push_back(makeRect(x0, y0, x1, y1));
            }
        }

        if (windows.empty())
            return out;

        clipper.AddSubject(windows);
    }
    else
    {
        clipper.AddSubject({makeRect(ifpX0, ifpY0, ifpX1, ifpY1)});
    }

    for (const Placed &p : placed)
    {
        const NestGeom::RotGeom &ag = rotBank[p.pieceIdx][p.rotIdx];

        if (nfpOutsideIFP(ag.bbox, bg.bbox, p.px, p.py, ifpX0, ifpX1, ifpY0))
            continue;

        const Engine::NFPCacheKey key =
            NestGeom::makeNFPCacheKey(p.pieceIdx, p.rotIdx, pieceIdx, rIdx);

        const Clipper2Lib::Paths64 *nfp = nullptr;
        auto it = nfpCache.find(key);
        if (it != nfpCache.end())
        {
            nfp = &it->second;
        }
        else
        {
            // The full polygon is kept so concavities stay exploitable — c'est
            // ce qui permet d'imbriquer dans le creux d'une courbe, et c'est
            // toute la raison d'être de ce moteur.
            Clipper2Lib::Paths64 nfpResult =
                Clipper2Lib::MinkowskiSum(ag.aref, bg.neg, true);

            for (auto &np : nfpResult)
            {
                if (Clipper2Lib::Area(np) < 0)
                    std::reverse(np.begin(), np.end());
            }

            auto [ins, _] = nfpCache.emplace(key, std::move(nfpResult));
            nfp = &ins->second;
        }

        if (!nfp->empty())
            clipper.AddClip(translatePaths(*nfp, p.px, p.py));
    }

    // Les zones interdites du bin — le bloc où se tient une page de garde, et toute
    // page sur laquelle un run ne doit pas s'imprimer. Soustraites par l'UNE des
    // implémentations, celle que les deux moteurs appellent : aucun des deux ne peut
    // répondre « garder ce rectangle libre » à sa façon.
    //
    // Le pré-filtre lit les mêmes `ifpX0/ifpX1/ifpY0` que les pièces posées
    // ci-dessus — le bord haut fermé de ce cadre n'est délibérément PAS ajouté comme
    // quatrième clause : voir la note dans ZoneGeom.h.
    NestZone::Subtract(clipper, zones, bg, pieceIdx, rIdx, ifpX0, ifpX1, ifpY0,
                       obstacleCache);

    clipper.Execute(Clipper2Lib::ClipType::Difference,
                    Clipper2Lib::FillRule::NonZero, out);

    // ── Aucun bord de pièce sur une jonction de feuilles ──────────────────
    //
    // Retiré de la RÉGION, et pas vérifié après coup : une position qui rase une jonction n'est plus
    // une position candidate. C'est ce qui manquait — le plan tombait sinon sur un bord à 1,7 mm
    // qu'aucune passe ultérieure ne pouvait plus rattraper.
    //
    // ⚠️ Seules les jonctions INTERNES comptent : les bords du plan sont les marges, tenues par
    // l'IFP ci-dessus, pas des lignes de coupe.
    std::vector<int64_t> vLines;
    for (int64_t k = 1; sheetW > 0 && k * sheetW < binW; ++k)
        vLines.push_back(k * sheetW);

    std::vector<int64_t> hLines;
    for (int64_t k = 1; sheetH > 0 && k * sheetH < envH; ++k)
        hLines.push_back(k * sheetH);

    // ⚠️ **Rien à faire quand les feuilles sont fermées** : les fenêtres ci-dessus ont déjà porté la
    // marge de part et d'autre de chaque frontière, ce qui interdit de la traverser en même temps
    // que de la frôler. Les bandes de jonction ne servent qu'aux plans qu'on DÉCOUPE.
    if (!closedSheets && (!vLines.empty() || !hLines.empty()))
    {
        Clipper2Lib::Paths64 bands =
            JunctionBands(bg.path, vLines, true, gapR, binW, envH);
        const Clipper2Lib::Paths64 horizontal =
            JunctionBands(bg.path, hLines, false, gapR, binW, envH);
        bands.insert(bands.end(), horizontal.begin(), horizontal.end());

        if (!bands.empty())
            out = Clipper2Lib::Difference(out, bands, Clipper2Lib::FillRule::NonZero);
    }

    return out;
}

/// Les points remarquables d'une région faisable — et pas seulement le coin bas-gauche.
///
/// **C'est là que la passe se jouait, et elle n'y arrivait pas.** `findBottomLeft` rend UN point :
/// le plus bas puis le plus à gauche. Or **vider une case ne demande pas d'aller au plus bas** —
/// ça demande d'en SORTIR, de quelques millimètres le long d'une frontière. Un minimum n'est
/// jamais ce point-là, donc la passe n'avait jamais le bon candidat sous la main : mesuré le
/// 22-09-2026, elle n'acceptait AUCUN déplacement, même en autorisant les déplacements neutres.
///
/// Tout point d'une région faisable est une position légale de l'origine de la pièce — bornes et
/// dégagement compris, puisque c'est la région qui les porte. On rend donc ses extrémités : le
/// coin bas-gauche (le placement naturel), le plus haut, le plus à gauche, le plus à droite.
auto Extremes(const Clipper2Lib::Paths64 &region) -> std::vector<Clipper2Lib::Point64>
{
    std::vector<Clipper2Lib::Point64> out;
    if (region.empty())
        return out;

    Clipper2Lib::Point64 lowest    = findBottomLeft(region);
    Clipper2Lib::Point64 topmost   = lowest;
    Clipper2Lib::Point64 leftmost  = lowest;
    Clipper2Lib::Point64 rightmost = lowest;

    for (const auto &path : region)
    {
        for (const auto &pt : path)
        {
            if (pt.y < topmost.y || (pt.y == topmost.y && pt.x > topmost.x))
                topmost = pt;
            if (pt.x < leftmost.x || (pt.x == leftmost.x && pt.y < leftmost.y))
                leftmost = pt;
            if (pt.x > rightmost.x || (pt.x == rightmost.x && pt.y < rightmost.y))
                rightmost = pt;
        }
    }

    out = {lowest, topmost, leftmost, rightmost};
    return out;
}

/// La pièce POSÉE telle que l'hôte la dessine — contour absolu, boîte, et les cases rencontrées.
///
/// ⚠️ **Un seul endroit sait dessiner une pièce posée.** Le compte du plan (`TouchedCells`) et le
/// compte d'UNE pièce (la passe page par pièce) doivent tomber d'accord à la case près : deux
/// formulations de la même convention, et le juge compte faux d'une case.
struct DrawnPiece
{
    Clipper2Lib::Path64 abs;    ///< Le contour absolu, dans les coordonnées du bin
    BBox                bbox;   ///< Sa boîte — c'est elle que la grille découpe
    std::set<int64_t>   cells;  ///< Les cases qu'il RENCONTRE, indexées `r * cols + c`
};

auto DrawPiece(const std::vector<std::vector<NestGeom::RotGeom>> &rotBank,
               int pieceIdx, int rotIdx, double stepDeg,
               int64_t px, int64_t py,
               int64_t sheetW, int64_t sheetH, int64_t cols, int64_t rows) -> DrawnPiece
{
    DrawnPiece d;

    // ⚠️ **Le contour tel qu'il est DESSINÉ, et non tel que le moteur le range.** La
    // convention de l'hôte — et celle de `NestedSvgWriter` — est
    // `translate(x, y) · rotate(deg) · translate(-ox, -oy)` : le placement porte le coin min
    // de la pièce **NON tournée**, et l'angle s'applique autour de lui.
    //
    // On repart donc de la rotation 0, on tourne, on translate — la reconstruction que répliquent
    // déjà `Engine::UsedHeight` et `NestedSvgWriter`.
    d.abs  = NestGeom::translatePath(
        NestGeom::rotatePath(rotBank[pieceIdx][0].path, rotIdx * stepDeg), px, py);
    d.bbox = bboxFromPath(d.abs);

    if (cols <= 0 || rows <= 0 || sheetW <= 0 || sheetH <= 0)
        return d;

    const int64_t c0 = std::max<int64_t>(0, d.bbox.xMin / sheetW);
    const int64_t c1 = std::min<int64_t>(cols - 1, d.bbox.xMax / sheetW);
    const int64_t r0 = std::max<int64_t>(0, d.bbox.yMin / sheetH);
    const int64_t r1 = std::min<int64_t>(rows - 1, d.bbox.yMax / sheetH);

    for (int64_t r = r0; r <= r1; ++r)
    {
        for (int64_t c = c0; c <= c1; ++c)
        {
            const Clipper2Lib::Path64 cell =
                makeRect(c * sheetW, r * sheetH, (c + 1) * sheetW, (r + 1) * sheetH);
            if (!Clipper2Lib::Intersect({d.abs}, {cell}, Clipper2Lib::FillRule::NonZero).empty())
            {
                d.cells.insert(r * cols + c);
            }
        }
    }

    return d;
}

/// Les cases de la grille de tuliage qu'un ensemble de pièces posées rencontre.
///
/// ⚠️ **Second exemplaire assumé d'une règle dont `OExportSheets::Compute` est l'autorité** dans
/// l'application : « une case est imprimée si le contour d'une pièce la rencontre ». Elle vit ici
/// pour la même raison que `feasibleRegion` est déjà un jumeau documenté de celui du moteur de
/// bande — **ce moteur paie en pages, il lui faut savoir ce qu'une page coûte** — et les deux
/// doivent rester d'accord : c'est l'application qui décide de ce qui s'imprime, jamais ceci.
///
/// **Rencontre, et non recouvre** : une pièce à cheval sur une frontière s'imprime sur les DEUX
/// cases, et c'est précisément ce qui donne à l'alignement son prix.
auto TouchedCells(const std::vector<Placed> &placed,
                  const std::vector<std::vector<NestGeom::RotGeom>> &rotBank, double stepDeg,
                  int64_t sheetW, int64_t sheetH, int64_t cols, int64_t rows,
                  int64_t *maxY = nullptr) -> std::set<int64_t>
{
    std::set<int64_t> touched;
    int64_t deepest = 0;

    for (const Placed &p : placed)
    {
        const DrawnPiece d = DrawPiece(rotBank, p.pieceIdx, p.rotIdx, stepDeg, p.px, p.py,
                                       sheetW, sheetH, cols, rows);
        deepest = std::max(deepest, d.bbox.yMax);
        touched.insert(d.cells.begin(), d.cells.end());
    }

    if (maxY != nullptr)
        *maxY = deepest;

    return touched;
}

/// Le nombre de RANGÉES que la hauteur réelle d'un plan coûte — **le compte de l'hôte**, pas
/// celui de l'enveloppe gelée de la passe.
///
/// ⚠️ **C'est ce qui manquait, et ça valait une page.** La passe jugeait sur une grille gelée
/// (`rows` de la passe 1) : un déplacement qui descend une pièce ne changeait donc pas son score,
/// alors qu'il change la hauteur, donc le nombre de rangées, donc **le compte que l'hôte
/// recalcule à la fin**. Mesuré le 22-09-2026 : la passe large faisait passer A3/Tabloid framed
/// de 11 à 12 feuilles *sans jamais accepter une augmentation* — parce qu'elle ne regardait pas
/// la même grille.
auto HeightOf(const std::vector<Placed> &placed,
              const std::vector<std::vector<NestGeom::RotGeom>> &rotBank, double stepDeg,
              int64_t margin) -> int64_t
{
    int64_t deepest = 0;
    for (const Placed &p : placed)
    {
        // ⚠️ **La rotation, et le margin — les deux manquaient, et les deux valaient une page.**
        //
        // Sans la rotation, une pièce que la passe a TOURNÉE était mesurée sur sa forme droite :
        // le plan paraissait plus court qu'il n'est, donc le nombre de rangées était sous-estimé,
        // donc **les cases que l'hôte va imprimer étaient invisibles à la passe** — et son
        // `after <= before` pouvait être vrai pendant que le compte de l'hôte montait. C'est
        // exactement pour ça qu'une passe qui n'acceptait « jamais » d'augmentation en produisait
        // une, trois fois de suite.
        //
        // Sans le margin, on oubliait ce que `Engine::UsedHeight` ajoute (`return usedHeight +
        // bin.marginMm()`), donc la dernière rangée pouvait être comptée en moins.
        const BBox bb = bboxFromPath(NestGeom::translatePath(
            NestGeom::rotatePath(rotBank[p.pieceIdx][0].path, p.rotIdx * stepDeg), p.px, p.py));
        deepest = std::max(deepest, bb.yMax);
    }
    return deepest + margin;
}

auto RowsOf(int64_t deepestY, int64_t sheetH) -> int64_t
{
    if (sheetH <= 0 || deepestY <= 0)
        return 1;
    return std::max<int64_t>(1, (deepestY + sheetH - 1) / sheetH);
}

/// Whether any of the piece's orientations fits inside ONE closed sheet.
///
/// **Asked before the row loop, and it is not an optimisation.** Sans elle, une pièce plus grande
/// qu'une feuille n'a de position dans aucune rangée : la boucle en ouvrirait jusqu'à sa borne avant
/// de déclarer la pièce impossible, et l'utilisatrice attendrait ce temps-là pour apprendre un refus
/// qui était décidable d'avance.
///
/// Les quatre orientations sont essayées, comme le placement : une pièce tournée d'un quart de tour
/// peut tenir dans une feuille où sa forme droite ne tient pas.
auto FitsOneSheet(const std::vector<NestGeom::RotGeom> &rots, int64_t usableW, int64_t usableH) -> bool
{
    if (usableW <= 0 || usableH <= 0)
        return false;

    for (const NestGeom::RotGeom &rot : rots)
    {
        const int64_t w = rot.bbox.xMax - rot.bbox.xMin;
        const int64_t h = rot.bbox.yMax - rot.bbox.yMin;
        if (w <= usableW && h <= usableH)
            return true;
    }
    return false;
}

} // anonymous namespace

//---------------------------------------------------------------------------------------------------------------------
TiledEngine::Floors TiledEngine::ComputeFloors(const std::vector<Clipper2Lib::Path64> &pieces,
                                               const Spec &spec)
{
    const double kAreaScale = kScale * kScale;

    Floors f;
    for (const Clipper2Lib::Path64 &p : pieces)
    {
        f.footprintArea += std::abs(Clipper2Lib::Area(p)) / kAreaScale;

        const BBox b = bboxFromPath(p);
        const double w = static_cast<double>(b.xMax - b.xMin) / kScale;
        const double h = static_cast<double>(b.yMax - b.yMin) / kScale;
        f.holderArea += w * h;
    }

    f.margin    = f.holderArea - f.footprintArea;
    f.sheetArea = spec.sheetWidth * spec.sheetHeight;

    if (f.sheetArea > 0.0)
    {
        f.footprintSheets = static_cast<int>(std::ceil(f.footprintArea / f.sheetArea));
        f.holderSheets    = static_cast<int>(std::ceil(f.holderArea / f.sheetArea));
    }

    return f;
}

//---------------------------------------------------------------------------------------------------------------------
TiledEngine::TiledEngine(const Spec &spec) noexcept
    : m_spec(spec)
{
}

//---------------------------------------------------------------------------------------------------------------------
std::vector<Placement> TiledEngine::place(const Bin &bin,
                                          const std::vector<Clipper2Lib::Path64> &pieces,
                                          bool *abandoned) const
{
    if (abandoned != nullptr)
        *abandoned = false;

    const int N = static_cast<int>(pieces.size());
    if (N == 0)
        return {};

    // ⚠️ **This engine takes a bin with ONE margin for all four sides** — the print run's, never
    // the folded cloth's (the arbitration refuses those, see oexportnesting.cpp): the four sides of
    // its closed frame are the same number, and `marginMm()` says it. A bin built with four
    // different margins would silently lose three of them here.
    const int64_t binW   = static_cast<int64_t>(bin.widthMm() * kScale);
    const int64_t margin = static_cast<int64_t>(bin.marginMm() * kScale);
    const int64_t gapR   = static_cast<int64_t>(m_spec.gap * kScale);
    const int64_t sheetH = static_cast<int64_t>(m_spec.sheetHeight * kScale);
    const int64_t sheetW = static_cast<int64_t>(m_spec.sheetWidth * kScale);

    if (binW <= 0 || sheetH <= 0)
        return {};

    // Quarter turns, and only quarter turns. A hole is filled by a quarter
    // turn; the 180 orientations the annealing pays for would multiply this
    // engine's whole cost by forty-five, for orientations a garment piece
    // never uses. `allowRotation = false` leaves the piece as it came.
    const double stepDeg = m_spec.allowRotation ? 90.0 : 360.0;
    const int    numRots = m_spec.allowRotation ? 4 : 1;

    const std::vector<Clipper2Lib::Path64> local = NestGeom::normalizeToLocal(pieces);
    const std::vector<std::vector<NestGeom::RotGeom>> rotBank =
        NestGeom::buildRotBank(local, stepDeg, numRots, gapR);

    // ── L'ordre : les grosses pièces d'abord ──────────────────────────────
    //
    // Le rangement se fait sur la surface du HOLDER, pas sur celle du contour.
    // C'est tout l'intérêt du holder : il révèle les grosses pièces, et ce sont
    // elles qui forcent l'ouverture de rangées. Une grande pièce très ajourée a
    // une petite surface de contour et un grand encombrement — c'est
    // l'encombrement qui décide, pas l'encre.
    //
    // Départage par index : l'itération d'un conteneur n'est pas déterministe,
    // et un moteur qui rend deux plans différents pour la même entrée ne se
    // mesure pas.
    std::vector<int> order(N);
    {
        std::vector<std::pair<double, int>> byHolder;
        byHolder.reserve(N);
        for (int i = 0; i < N; ++i)
        {
            const BBox b = bboxFromPath(pieces[i]);
            const double w = static_cast<double>(b.xMax - b.xMin);
            const double h = static_cast<double>(b.yMax - b.yMin);
            // L'ordre demandé — la surface par défaut, la hauteur ou la largeur sinon. C'est le
            // seul levier de cette variante : mêmes pièces, même moteur, un autre départ.
            const double key = m_spec.order == Spec::Order::HolderHeight ? h
                               : m_spec.order == Spec::Order::HolderWidth ? w
                                                                          : w * h;
            byHolder.emplace_back(key, i);
        }
        std::sort(byHolder.begin(), byHolder.end(),
                  [](const auto &a, const auto &b)
                  {
                      if (a.first != b.first)
                          return a.first > b.first;
                      return a.second < b.second;
                  });
        for (int i = 0; i < N; ++i)
            order[i] = byHolder[i].second;
    }

    std::vector<Placement> out(N);
    for (auto &pl : out)
        pl.rotation = -1.0;   // sentinel: not placed yet

    std::vector<Placed> placed;
    placed.reserve(N);

    NestGeom::NFPCache nfpCache;
    nfpCache.reserve(std::size_t(N) * numRots * 4);

    // The bin's forbidden zones as engine geometry, built ONCE — the row loop below asks
    // feasibleRegion four times per piece per attempt, and rebuilding K inflations there
    // is exactly what the strip engine's own note warns against. Separate cache, keyed
    // (piece, rot, zone): a zone's NFP is independent of context, and it is not a
    // piece-vs-piece NFP, so it must not share their key space.
    const std::vector<NestZone::Geom> zones = NestZone::Build(bin.zones(), gapR);
    NestGeom::NFPCache obstacleCache;
    obstacleCache.reserve(std::size_t(N) * numRots * std::max<std::size_t>(zones.size(), 1));

    // ── L'enveloppe ───────────────────────────────────────────────────────
    //
    // La largeur est celle du bin — c'est-à-dire le MAXIMUM de colonnes que
    // l'utilisatrice a saisi. Une rangée peut en utiliser moins : les cases
    // qu'elle ne touche pas ne s'impriment pas, et c'est là que le plan cesse
    // d'être un rectangle. Seul le nombre de RANGÉES s'ouvre.
    int64_t rows = 1;

    // ── La borne de la boucle de rangées ──────────────────────────────────
    //
    // L'analogue, côté moteur de bande, est `infY` : une hauteur au-delà de
    // laquelle aucun arrangement n'existe, donc un placement qui l'atteint est un
    // échec et non une recherche. Ici l'unité est la RANGÉE — et sans cette borne
    // la boucle ci-dessous ouvrirait des rangées sans fin dès qu'une zone
    // interdite ne laisse de place dans aucune : ce serait une boucle infinie, pas
    // une lenteur.
    //
    // Le pire cas légal : chaque pièce seule dans sa bande. Une pièce qui n'est pas
    // entrée en si peu de rangées n'entrera jamais.
    //
    // ⚠️ **Lue TOUJOURS, zones ou pas.** La borne ne parle pas des zones : elle dit qu'une
    // pièce qui n'est pas entrée en autant de rangées que ses propres hauteurs n'entrera
    // jamais. Tant qu'elle n'était armée que s'il y avait des zones, un run sans page de
    // garde — A4, rouleau A0 — n'avait aucune borne : la boucle ouvrait des rangées
    // indéfiniment et le run ne rendait pas la main.
    int64_t maxRows = 1;
    for (int i = 0; i < N; ++i)
    {
        const BBox &b = rotBank[i][0].bbox;
        maxRows += (b.yMax - b.yMin + sheetH - 1) / sheetH;
    }

    for (const int pieceIdx : order)
    {
        // **Asked once a piece**, which is where this engine spends its time: each one is tried
        // against every row the plan has opened. See Engine::Config::abandoned.
        if (IsAbandoned())
        {
            if (abandoned != nullptr)
                *abandoned = true;
            return out;
        }

        const BBox &b0 = rotBank[pieceIdx][0].bbox;

        // ⚠️ **Une empreinte invalide ne se place jamais** — et pire, elle rend tout le reste
        // incomparable : un NaN dans la boîte fait échouer `rows >= maxRows` comme tous les tests
        // de placement, et la boucle ouvre des rangées sans fin. Une géométrie non finie est donc
        // déclarée « pièce impossible », comme une pièce trop large : même marqueur, `rotation < 0`.
        if (!(b0.xMax >= b0.xMin) || !(b0.yMax >= b0.yMin))
        {
            out[pieceIdx].pieceIndex = pieceIdx;
            out[pieceIdx].rotation   = -1.0;
            return out;
        }

        // Une pièce plus large que le bin ne se placera jamais, quel que soit
        // le nombre de rangées : ouvrir boucherait sans fin. Même marqueur que
        // le moteur de bande — l'hôte teste `rotation < 0`.
        if (margin - b0.xMin >= binW - margin - b0.xMax)
        {
            out[pieceIdx].pieceIndex = pieceIdx;
            out[pieceIdx].rotation   = -1.0;
            return out;
        }

        // **Une pièce plus grande qu'une feuille, quand les feuilles sont fermées.** Aucune rangée
        // ne la tiendra, et la boucle en ouvrirait jusqu'à sa borne avant de l'admettre.
        //
        // ⚠️ `continue` et non `return`, à la différence du test ci-dessus : le run va être refusé,
        // et le refus doit nommer les pièces qui ne tiennent VRAIMENT pas — pas toutes celles qui
        // auraient été posées après elle.
        if (m_spec.closedSheets
            && !FitsOneSheet(rotBank[pieceIdx], sheetW - 2 * margin, sheetH - 2 * margin))
        {
            out[pieceIdx].pieceIndex = pieceIdx;
            out[pieceIdx].rotation   = -1.0;
            continue;
        }

        bool done = false;
        while (!done)
        {
            const int64_t envH = rows * sheetH;

            // Parmi les orientations qui entrent, la plus basse puis la plus à
            // gauche. Les essayer toutes coûte quatre régions faisables, et
            // choisir la meilleure vaut mieux que prendre la première venue :
            // c'est ce qui fait qu'une petite pièce se glisse dans un trou au
            // lieu de rester au bord.
            int     bestRot = -1;
            int64_t bestX = 0, bestY = 0;
            int64_t bestBottom = 0;

            for (int rIdx = 0; rIdx < numRots; ++rIdx)
            {
                const Clipper2Lib::Paths64 region =
                    feasibleRegion(placed, rotBank, rotBank[pieceIdx][rIdx],
                                   pieceIdx, rIdx, binW, envH, margin,
                                   sheetW, sheetH, gapR, m_spec.closedSheets,
                                   zones, nfpCache, obstacleCache);

                const Clipper2Lib::Point64 pos = findBottomLeft(region);

                // `findBottomLeft` rend y = INT64_MAX sur une région vide, donc
                // ce test couvre les deux cas : rien à placer, ou rien qui
                // tienne dans l'enveloppe courante.
                if (pos.y >= envH)
                    continue;

                // **Le BAS le plus haut, et non le HAUT le plus haut.** C'est la règle qui
                // posait la pièce dans la mauvaise orientation — le diagnostic de Sophie, mot pour
                // mot : « la rotation validée par le moteur est fausse puisqu'elle ne permet pas de
                // mettre la pièce plus haut ».
                //
                // Comparer `pos.y` seul garde l'orientation dont le coin de départ monte le plus,
                // **sans regarder la hauteur** : une pièce peut partir de haut et descendre loin,
                // occuper beaucoup de place vers le bas, et ne plus pouvoir remonter ensuite. Ce
                // qu'elle fait à la main, c'est TOURNER la pièce pour qu'elle descende moins — donc
                // le chiffre à comparer est son bord inférieur : `pos.y + bg.bbox.yMax`.
                const int64_t bottom = pos.y + rotBank[pieceIdx][rIdx].bbox.yMax;

                if (bestRot < 0 || pos.y < bestY || (pos.y == bestY && pos.x < bestX))
                {
                    bestRot    = rIdx;
                    bestX      = pos.x;
                    bestY      = pos.y;
                    bestBottom = bottom;
                }
            }

            if (bestRot >= 0)
            {
                out[pieceIdx].pieceIndex = pieceIdx;
                out[pieceIdx].x          = static_cast<double>(bestX) / kScale;
                out[pieceIdx].y          = static_cast<double>(bestY) / kScale;
                out[pieceIdx].rotation   = bestRot * stepDeg;

                placed.push_back({pieceIdx, bestRot, bestX, bestY});
                done = true;
            }
            else
            {
                // Aucune orientation n'entre : c'est ici, et seulement ici,
                // qu'une rangée s'ouvre.
                //
                // Sauf quand le plafond est atteint : les rangées ne rendront pas la
                // place, et en ouvrir indéfiniment serait une boucle sans fin. Même
                // marqueur que la pièce trop large, et pour la même raison —
                // `rotation < 0` est ce que la passe 2 et l'hôte lisent — et le plan
                // entier est rendu tel quel. Ostara le déclarera incomplet par
                // `AllPlaced` et gardera celui du moteur de bande, qui a déjà posé
                // toutes les pièces.
                // Un plafond calculé peut être inutilisable (NaN dans une hauteur) ; celui-ci est
                // une constante, et il ne peut pas être neutralisé par une géométrie douteuse.
                constexpr int64_t kAbsoluteRowLimit = 100000;
                if (rows >= maxRows || rows >= kAbsoluteRowLimit)
                {
                    out[pieceIdx].pieceIndex = pieceIdx;
                    out[pieceIdx].rotation   = -1.0;
                    return out;
                }

                ++rows;
            }
        }
    }

    // ── La passe des pages ────────────────────────────────────────────────
    //
    // Le glouton ne revient jamais sur ce qu'il a posé — c'est la limite de méthode consignée en
    // §23 de la spec du nesting — et il laisse donc des pièces dans les cases du FOND qu'un
    // déplacement vers un trou des rangées du dessus viderait. **Une case vidée est une page
    // gagnée** : c'est la seule monnaie de ce moteur.
    //
    // Mesuré le 22-09-2026 sur le patron de Sophie, Tabloid en 4 colonnes : la dernière rangée
    // tient QUATRE pièces sur trois cases, dont deux n'en portent qu'une — une seule pièce à
    // déplacer, une page. Et cela **avec ou sans page de garde** : ce que la page de garde a fait,
    // c'est rendre le défaut visible, pas le créer.
    //
    // Le score est le NOMBRE DE CASES TOUCHÉES, et rien d'autre : ni une hauteur (celle du moteur
    // de bande), ni des rangées (celles de la passe 2 d'EngineRefine). Un déplacement n'est donc
    // accepté que s'il fait **strictement** baisser ce compte — **le plan ne peut pas sortir pire
    // de cette passe**, et c'est ce qui la rend sûre à poser sur tous les runs tuilés, page de
    // garde ou non.
    const int64_t cols = sheetW > 0 ? binW / sheetW : 0;

    // Le meilleur plan traversé par la passe. Vide quand elle n'a pas tourné — et c'est ce vide
    // qui dit « rien à restaurer ».
    std::vector<Placed> bestPlaced;

    if (sheetW > 0 && cols > 1 && rows > 1 && placed.size() > 1)
    {
        // Règle d'arrêt simple et borne dure : on refait un tour tant qu'un déplacement a été
        // accepté, plafonné. Le coût par run est un point ouvert de la spec (§24.7), et une borne
        // se mesure ; une boucle qui ne se termine pas, non.
        // **L'étroite a ses propres tours, et le large vient après.** Ce n'est pas un réglage :
        // mesuré le 22-09-2026, ouvrir les candidates larges dès le premier tour fait repasser
        // A3/Tabloid framed de 11 à 12 feuilles — la marche part ailleurs AVANT que l'étroite ait
        // fini de gagner sa page, et ne revient jamais sur le chemin qu'elle avait pris.
        constexpr int kNarrowRounds = 3;
        constexpr int kMaxRounds    = 6;

        // **Le déplacement NEUTRE est accepté** (`<=` plus bas, et non `<`), et c'est toute
        // l'idée. Mesuré le 22-09-2026 : avec la règle stricte la passe n'accepte RIEN sur
        // Tabloid 4 colonnes — les rangées du dessus sont trop pleines pour accueillir une pièce
        // du fond, donc aucun coup ne gagne seul. Un déplacement neutre ne gagne rien sur le
        // moment, mais **il ouvre un trou là où il en manquait un**, et la descente qui suit
        // trouve alors l'amélioration qu'elle ne pouvait pas atteindre.
        //
        // Le plan ne peut pas devenir pire pour autant : une AUGMENTATION reste refusée.
        //
        // Sans garde-fous, la marche neutre devient une marche au hasard. Il y en a deux, et ils
        // sont durs : on ne repasse jamais par un plan déjà vu, et le nombre de déplacements
        // acceptés est plafonné.
        std::set<std::vector<int64_t>> seen;
        const auto StateKey = [&placed]() {
            std::vector<int64_t> key;
            key.reserve(placed.size() * 4);
            for (const Placed &q : placed)
            {
                key.push_back(q.pieceIdx);
                key.push_back(q.rotIdx);
                key.push_back(q.px);
                key.push_back(q.py);
            }
            return key;
        };
        seen.insert(StateKey());

        // ⚠️ **LE MEILLEUR VU, et pas celui où la marche s'arrête.** Une marche qui accepte des
        // déplacements NEUTRES dérive : elle ne perd jamais de page, mais elle peut s'éloigner du
        // chemin qui en gagnait une et **rendre un état moins bon que celui qu'elle a traversé**.
        // Mesuré le 22-09-2026 : les tours larges faisaient repasser A3/Tabloid framed de 11 à 12
        // feuilles alors qu'aucun déplacement n'acceptait d'augmentation.
        //
        // On mémorise donc le meilleur plan rencontré, sur la grille RÉELLE — celle que l'hôte
        // recalculera — et c'est lui qu'on rend. Les tours larges ne peuvent alors qu'aider.
        const int64_t bestRows0 = RowsOf(HeightOf(placed, rotBank, stepDeg, margin), sheetH);
        int bestCount = static_cast<int>(
            TouchedCells(placed, rotBank, stepDeg, sheetW, sheetH, cols, bestRows0).size());
        bestPlaced = placed;

        const auto HarvestBest = [&]() {
            const int64_t rowsNow = RowsOf(HeightOf(placed, rotBank, stepDeg, margin), sheetH);
            const int now = static_cast<int>(
                TouchedCells(placed, rotBank, stepDeg, sheetW, sheetH, cols, rowsNow).size());
            if (now < bestCount)
            {
                bestCount  = now;
                bestPlaced = placed;
            }
        };

        int accepted = 0;
        const int maxAccepted = static_cast<int>(placed.size()) * 4;

        for (int round = 0; round < kMaxRounds && accepted < maxAccepted; ++round)
        {
            bool improved = false;

            for (std::size_t i = 0; i < placed.size(); ++i)
            {
                const Placed candidate = placed[i];

                // **Le premier tour est ÉTROIT, les suivants sont LARGES — et cet ordre est
                // tout.** Le tour étroit ne considère que les pièces qui touchent la dernière
                // rangée : c'est là que se trouve le gain évident, celui que Sophie reprend à la
                // main. Les tours suivants s'ouvrent à TOUTES les pièces, parce qu'un gain plus
                // profond commence ailleurs — mesuré le 22-09-2026 sur A3/Tabloid framed : il
                // passe par une GROSSE PIÈCE DU HAUT qu'il faut TOURNER pour qu'elle prenne
                // moins de place et libère le passage à une pièce du fond.
                //
                // ⚠️ **L'ordre n'est pas un détail de réglage : il décide du résultat.** Ouverte
                // d'emblée, la passe large accepte trop de déplacements neutres, part ailleurs, et
                // **ne retrouve plus le chemin que l'étroite trouvait** — mesuré : A3/Tabloid
                // framed repassait de 11 à 12, et le run doublait. Étroite d'abord, large ensuite :
                // le compte ne peut jamais remonter, donc rien de gagné ne se perd.
                if (round < kNarrowRounds)
                {
                    const BBox candidateBB = bboxFromPath(NestGeom::translatePath(
                        NestGeom::rotatePath(rotBank[candidate.pieceIdx][0].path,
                                             candidate.rotIdx * stepDeg),
                        candidate.px, candidate.py));
                    if (candidateBB.yMax < (rows - 1) * sheetH)
                    {
                        continue;
                    }
                }

                // ⚠️ La grille RÉELLE, pas l'enveloppe gelée : c'est celle que l'hôte
                // recalculera sur la hauteur finale, et deux grilles différentes font deux
                // comptes différents.
                const int64_t rowsBefore = RowsOf(HeightOf(placed, rotBank, stepDeg, margin), sheetH);
                const std::set<int64_t> before =
                    TouchedCells(placed, rotBank, stepDeg, sheetW, sheetH, cols, rowsBefore);

                // La pièce est retirée du plan, puis reposée au plus haut possible : la mécanique
                // de la passe 1, avec une pièce de moins. L'enveloppe est GELÉE — une pièce ne
                // peut pas ouvrir une rangée de plus, sans quoi la passe pourrait faire monter le
                // nombre de pages qu'elle est là pour faire baisser.
                std::vector<Placed> without;
                without.reserve(placed.size() - 1);
                for (std::size_t j = 0; j < placed.size(); ++j)
                {
                    if (j != i)
                    {
                        without.push_back(placed[j]);
                    }
                }

                // Chaque orientation ET chaque point remarquable de sa région : on essaie POUR
                // DE VRAI, on recompte, et on garde le premier candidat qui ne coûte rien. Ce qui
                // est refusé est remis exactement où c'était — d'où la promesse que le plan ne
                // peut pas sortir pire.
                bool moved = false;
                for (int rIdx = 0; rIdx < numRots && !moved; ++rIdx)
                {
                    const Clipper2Lib::Paths64 region =
                        feasibleRegion(without, rotBank, rotBank[candidate.pieceIdx][rIdx],
                                       candidate.pieceIdx, rIdx, binW, rows * sheetH, margin,
                                       sheetW, sheetH, gapR, m_spec.closedSheets,
                                       zones, nfpCache, obstacleCache);

                    for (const Clipper2Lib::Point64 &pos : Extremes(region))
                    {
                        if (pos.y >= rows * sheetH)
                        {
                            continue;
                        }

                        placed[i] = {candidate.pieceIdx, rIdx, pos.x, pos.y};
                        const int64_t rowsAfter = RowsOf(HeightOf(placed, rotBank, stepDeg, margin), sheetH);
                        const std::set<int64_t> after =
                            TouchedCells(placed, rotBank, stepDeg, sheetW, sheetH, cols, rowsAfter);

                        // Neutre accepté, augmentation refusée — voir la note au-dessus de
                        // `seen` — et jamais deux fois le même plan.
                        if (after.size() <= before.size() && seen.insert(StateKey()).second)
                        {
                            out[candidate.pieceIdx].x        = static_cast<double>(pos.x) / kScale;
                            out[candidate.pieceIdx].y        = static_cast<double>(pos.y) / kScale;
                            out[candidate.pieceIdx].rotation = rIdx * stepDeg;
                            ++accepted;
                            improved = true;
                            moved    = true;
                            HarvestBest();
                            break;
                        }

                        placed[i] = candidate;
                    }
                }
            }

            if (!improved)
            {
                break;
            }
        }

        // **Le meilleur plan traversé, et non celui où la marche s'est arrêtée.** Sans cela, une
        // marche qui accepte des déplacements neutres dérive et rend moins que ce qu'elle a vu.
        for (const Placed &q : bestPlaced)
        {
            out[q.pieceIdx].x        = static_cast<double>(q.px) / kScale;
            out[q.pieceIdx].y        = static_cast<double>(q.py) / kScale;
            out[q.pieceIdx].rotation = q.rotIdx * stepDeg;
        }
    }

    // ⚠️ **Le moins de feuilles PAR PIÈCE ne se juge pas ici.** Un plan tuilé n'est reposé qu'une
    // fois choisi : c'est `PageRepack` qui le fait, sur le plan qui s'imprime réellement — celui du
    // moteur tuilé quand il gagne l'arbitrage, celui de la bande quand il ne le gagne pas. Le faire
    // ici aurait rendu le gain invisible dans le second cas, ce qui est arrivé.

    return out;
}
