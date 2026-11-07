/************************************************************************
 **
 **  @file   PageRepack.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   07/11/2026
 **
 **  @brief  Reposage d'un plan tuilé — voir PageRepack.h.
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

#include "PageRepack.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

#include "Bin.h"
#include "NestGeom.h"

namespace PageRepack
{
namespace
{
using NestGeom::bboxFromPath;
using NestGeom::kScale;
using NestGeom::makeRect;
using NestGeom::rotatePath;
using NestGeom::translatePath;

auto ToFixed(double value) -> int64_t
{
    return static_cast<int64_t>(std::llround(value * kScale));
}

/// Une pièce du plan, telle qu'elle est DESSINÉE à une position donnée.
///
/// ⚠️ La convention est celle de l'hôte — `translate(x, y) · rotate(θ)` appliqué au contour ramené
/// à son propre coin min — et non une autre : le compte de feuilles doit tomber sur le même dessin
/// que celui qu'Ostara imprime, sinon la passe croit gagner là où rien ne change.
struct Ink
{
    Clipper2Lib::Path64 abs;
    BBox                bbox;
};

auto Draw(const Clipper2Lib::Path64 &local, double angleDeg, int64_t px, int64_t py) -> Ink
{
    Ink ink;
    ink.abs  = translatePath(rotatePath(local, angleDeg), px, py);
    ink.bbox = bboxFromPath(ink.abs);
    return ink;
}

/// Les cases de la grille que ce tracé RENCONTRE — rencontre, et non recouvre : une pièce à cheval
/// sur une frontière s'imprime sur les DEUX feuilles, et c'est tout le prix de l'alignement.
auto Cells(const Clipper2Lib::Path64 &abs, int64_t cellW, int64_t cellH, int64_t columns,
           int64_t rows) -> std::set<int64_t>
{
    std::set<int64_t> out;
    const BBox bb = bboxFromPath(abs);

    const int64_t c0 = std::max<int64_t>(0, bb.xMin / cellW);
    const int64_t c1 = std::min<int64_t>(columns - 1, bb.xMax / cellW);
    const int64_t r0 = std::max<int64_t>(0, bb.yMin / cellH);
    const int64_t r1 = std::min<int64_t>(rows - 1, bb.yMax / cellH);

    for (int64_t r = r0; r <= r1; ++r)
    {
        for (int64_t c = c0; c <= c1; ++c)
        {
            const Clipper2Lib::Path64 cell =
                makeRect(c * cellW, r * cellH, (c + 1) * cellW, (r + 1) * cellH);
            if (!Clipper2Lib::Intersect({abs}, {cell}, Clipper2Lib::FillRule::NonZero).empty())
            {
                out.insert(r * columns + c);
            }
        }
    }

    return out;
}

/// Le nombre de feuilles qu'une pièce de cette taille ne peut PAS éviter — borne basse, la pièce
/// glissée n'importe où, voisines ignorées.
///
/// C'est le crible : une orientation dont la borne basse n'est pas strictement meilleure que ce que
/// la pièce coûte déjà ne peut rien gagner, et ses positions n'ont pas à être essayées.
auto LowerBound(int64_t w, int64_t h, int64_t cellW, int64_t cellH) -> std::size_t
{
    if (w <= 0 || h <= 0 || cellW <= 0 || cellH <= 0)
        return 0;

    const int64_t cw = std::max<int64_t>(1, (w + cellW - 1) / cellW);
    const int64_t ch = std::max<int64_t>(1, (h + cellH - 1) / cellH);
    return static_cast<std::size_t>(cw * ch);
}

/// Vrai si les deux boîtes se recouvrent — le pré-filtre qui évite un test de Clipper pour une
/// voisine dont on sait déjà qu'elle est ailleurs.
auto Overlaps(const BBox &a, const BBox &b) -> bool
{
    return !(b.xMax <= a.xMin || b.xMin >= a.xMax || b.yMax <= a.yMin || b.yMin >= a.yMax);
}

/// Vrai si ce contour RASE une jonction — la règle de `NestGeom::borderToLine`, refusée et non
/// seulement comptée : un bord de pièce plus près d'une ligne de coupe que le jeu.
///
/// Les bords du PLAN ne sont pas des jonctions : ce sont les marges, et elles sont tenues par les
/// bornes du couloir, pas par cette fonction.
auto Grazes(const Clipper2Lib::Path64 &drawn, const BBox &box,
            const std::vector<int64_t> &vLines, const std::vector<int64_t> &hLines,
            int64_t gap) -> bool
{
    for (const int64_t line : vLines)
    {
        if (line + gap < box.xMin || line - gap > box.xMax)
            continue;   // trop loin pour raser quoi que ce soit

        if (NestGeom::borderToLine(drawn, line, true) < static_cast<double>(gap))
            return true;
    }

    for (const int64_t line : hLines)
    {
        if (line + gap < box.yMin || line - gap > box.yMax)
            continue;

        if (NestGeom::borderToLine(drawn, line, false) < static_cast<double>(gap))
            return true;
    }

    return false;
}

/// Qui empêche la pièce gonflée du jeu de s'arrêter ici : 0 personne, 1 une zone interdite, 2 une
/// autre pièce.
///
/// Le code, et non un booléen, parce que la différence est tout ce qu'on sait lire quand une passe ne
/// trouve rien : « bloqué par une pièce » et « bloqué par la page de garde » ne se corrigent pas au
/// même endroit.
auto Blocked(const Clipper2Lib::Path64 &cand, const BBox &box, const std::vector<Ink> &ink,
             int moving, const std::vector<Clipper2Lib::Path64> &forbidden) -> int
{
    for (const Clipper2Lib::Path64 &zone : forbidden)
    {
        if (!Clipper2Lib::Intersect({cand}, {zone}, Clipper2Lib::FillRule::NonZero).empty())
            return 1;
    }

    for (int j = 0; j < static_cast<int>(ink.size()); ++j)
    {
        if (j == moving)
            continue;

        const Ink &other = ink[static_cast<std::size_t>(j)];
        if (!Overlaps(box, other.bbox))
            continue;

        if (!Clipper2Lib::Intersect({cand}, {other.abs}, Clipper2Lib::FillRule::NonZero).empty())
            return 2;
    }

    return 0;
}

} // anonymous namespace

//---------------------------------------------------------------------------------------------------------------------
auto Minimize(const Bin &bin,
              const std::vector<Clipper2Lib::Path64> &pieces,
              const std::vector<Placement> &placements,
              const Frame &frame,
              Report *report) -> std::vector<Placement>
{
    if (report != nullptr)
    {
        *report         = Report{};
        report->pieces  = static_cast<int>(placements.size());
        report->columns = frame.columns;
        report->rows    = frame.rows;
    }

    const int64_t cellW  = ToFixed(frame.cellWidth);
    const int64_t cellH  = ToFixed(frame.cellHeight);
    const int64_t gap    = ToFixed(frame.gap);
    const int64_t margin = ToFixed(bin.marginMm());
    const int64_t binW   = ToFixed(bin.widthMm());

    const int64_t columns = frame.columns;
    const int64_t rows    = frame.rows;

    if (cellW <= 0 || cellH <= 0 || binW <= 0 || columns < 1 || rows < 1)
    {
        return placements;
    }

    const int N = static_cast<int>(placements.size());
    if (N < 2 || static_cast<int>(pieces.size()) != N)
        return placements;

    // Le plan s'arrête à la grille : une pièce ne descend jamais sous la dernière rangée, sinon le
    // plan grandit et la passe aurait payé un déplacement d'une feuille entière.
    const int64_t planH = rows * cellH;

    // Même tolérance que dans Breathe, et pour la même raison : le NFP du moteur est calculé sur un
    // contour simplifié à 0,5 px, donc le plan peut laisser un demi-pixel de moins que le jeu
    // nominal. Un test au bit près refuse alors des positions que le moteur a posées lui-même.
    const int64_t tol = ToFixed(1.0);

    // **Les pièces ramenées à leur propre coin min** — la convention de placement de tout ce
    // dépôt, et celle du dessin.
    const std::vector<Clipper2Lib::Path64> local = NestGeom::normalizeToLocal(pieces);

    // Les zones interdites, gonflées du jeu : tenues à distance comme une pièce le serait.
    // Le moteur les soustrait de la même façon, donc la passe ne peut pas rouvrir ce qu'il a fermé.
    std::vector<Clipper2Lib::Path64> forbidden;
    forbidden.reserve(bin.zones().size());
    for (const Bin::Zone &zone : bin.zones())
    {
        Clipper2Lib::Paths64 grown = Clipper2Lib::InflatePaths(
            {makeRect(ToFixed(zone.x), ToFixed(zone.y), ToFixed(zone.x + zone.width),
                      ToFixed(zone.y + zone.height))},
            static_cast<double>(gap), Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon);
        if (!grown.empty())
            forbidden.push_back(grown.front());
    }

    // ── Les lignes de coupe ────────────────────────────────────────────────
    //
    // Les jonctions entre deux feuilles, et **jamais les bords du plan** : un bord de plan est une
    // marge, un bord de feuille est une ligne qu'on coupe et qu'on recolle. Un contour posé dessus
    // est le défaut que Sophie voit — la pièce est imprimée pile sur la jonction.
    //
    // Le seuil est celui de la passe d'alignement du moteur de bande (`Engine::refine`) : un bord
    // « rase » la ligne dès qu'il en est plus près que le JEU. Même mesure, même valeur, et c'est
    // pour ça que `NestGeom::borderToLine` est partagé au lieu d'être recopié.
    std::vector<int64_t> vLines;
    for (int64_t k = 1; k < columns; ++k)
        vLines.push_back(k * cellW);

    std::vector<int64_t> hLines;
    for (int64_t k = 1; k < rows; ++k)
        hLines.push_back(k * cellH);

    std::vector<Placement> out = placements;

    struct State
    {
        int64_t px;
        int64_t py;
        double  angle;
    };

    std::vector<State>          state(static_cast<std::size_t>(N));
    std::vector<Ink>            ink(static_cast<std::size_t>(N));
    std::vector<std::set<int64_t>> cells(static_cast<std::size_t>(N));

    // `occupancy[c]` : combien de pièces touchent la feuille `c`. Le total imprimé est le nombre de
    // cases non nulles, et il se tient à jour sans jamais recompter le plan.
    std::vector<int> occupancy(static_cast<std::size_t>(columns * rows), 0);
    std::size_t      total = 0;

    for (int i = 0; i < N; ++i)
    {
        State &s = state[static_cast<std::size_t>(i)];
        s.px     = ToFixed(placements[static_cast<std::size_t>(i)].x);
        s.py     = ToFixed(placements[static_cast<std::size_t>(i)].y);
        s.angle  = placements[static_cast<std::size_t>(i)].rotation;

        ink[static_cast<std::size_t>(i)] = Draw(local[static_cast<std::size_t>(i)], s.angle, s.px, s.py);
        cells[static_cast<std::size_t>(i)] = Cells(ink[static_cast<std::size_t>(i)].abs, cellW, cellH,
                                                   columns, rows);

        for (const int64_t c : cells[static_cast<std::size_t>(i)])
        {
            if (occupancy[static_cast<std::size_t>(c)]++ == 0)
                ++total;
        }
    }

    // ── Ce que la passe a lu, AVANT de décider quoi que ce soit ──────────────
    //
    // Rempli même quand rien ne bougera : « combien de feuilles occupe chaque pièce » est une
    // question qui a une réponse sur tout plan tuilé, et c'est celle que Sophie demande.
    if (report != nullptr)
    {
        report->sheetsBefore = static_cast<int>(total);
        report->perPiece.resize(static_cast<std::size_t>(N));

        for (int i = 0; i < N; ++i)
        {
            SheetCount &one = report->perPiece[static_cast<std::size_t>(i)];
            one.before   = static_cast<int>(cells[static_cast<std::size_t>(i)].size());
            one.best     = one.before;
            one.bestSeen = one.before;

            if (one.before > 1)
                ++report->straddlingBefore;
        }
    }

    // Une seule feuille : toute position coûte la même, et il n'y a rien à reposer.
    if (columns == 1 && rows == 1)
        return placements;

    if (report != nullptr)
        report->ran = true;

    // Ce que le plan coûterait si la pièce en cours était à `next`.
    //
    // ⚠️ **`occupancy` ne compte PAS la pièce qu'on déplace** — elle est retirée avant l'essai, pour
    // qu'elle ne se gêne pas elle-même. C'est cette absence qui rend la fonction juste, et c'est
    // pourquoi elle ne peut pas se contenter d'ajuster `total`.
    const auto UnionSize = [&](const std::set<int64_t> &with) -> std::size_t
    {
        std::size_t after = 0;
        for (const int count : occupancy)
        {
            if (count > 0)
                ++after;
        }

        for (const int64_t c : with)
        {
            if (occupancy[static_cast<std::size_t>(c)] == 0)
                ++after;
        }
        return after;
    };

    // La pièce `moving` peut-elle occuper cette position ? On teste la pièce GONFLÉE du jeu contre
    // les contours exacts des autres : tout chevauchement est un écart violé, et c'est exactement
    // ce que la somme de Minkowski du moteur interdisait.

    // Trois tours : un déplacement peut dégager la place d'un second, et deux tours suffisent
    // presque toujours. Chaque acceptation fait STRICTEMENT baisser le compte d'une pièce, donc
    // rien de gagné ne peut se perdre, et le nombre de tours borne le coût.
    constexpr int kRounds = 3;

    for (int round = 0; round < kRounds; ++round)
    {
        bool improved = false;

        for (int i = 0; i < N; ++i)
        {
            const std::size_t idx = static_cast<std::size_t>(i);
            const std::size_t was = cells[idx].size();

            // Une pièce qui tient déjà sur une seule feuille tient le minimum.
            if (was <= 1)
                continue;

            const State origin = state[idx];

            // **Translation d'abord, rotation en secours.** Les quarts de tour n'entrent en jeu que
            // si aucune translation ne fait gagner une feuille.
            for (int turn = 0; turn < 4; ++turn)
            {
                const double angle = origin.angle + 90.0 * static_cast<double>(turn);

                const Clipper2Lib::Path64 turned = rotatePath(local[idx], angle);
                const BBox ext = bboxFromPath(turned);

                if (LowerBound(ext.xMax - ext.xMin, ext.yMax - ext.yMin, cellW, cellH) >= was)
                {
                    continue;   // cette orientation ne peut pas battre ce que la pièce coûte
                }

                // La pièce gonflée du jeu, une fois par orientation : c'est elle qu'on déplace et
                // qu'on teste, jamais la pièce nue.
                const Clipper2Lib::Paths64 grown = Clipper2Lib::InflatePaths(
                    {turned}, static_cast<double>(gap - tol), Clipper2Lib::JoinType::Miter,
                    Clipper2Lib::EndType::Polygon);
                const Clipper2Lib::Path64 ref = grown.empty() ? turned : grown.front();

                // Les bornes du plan, marges comprises. En sortir, c'est imprimer hors du papier ;
                // descendre plus bas, c'est ouvrir une rangée — les deux sont refusés.
                const int64_t xLo = margin - ext.xMin;
                const int64_t xHi = binW - margin - ext.xMax;
                const int64_t yLo = margin - ext.yMin;
                const int64_t yHi = planH - margin - ext.yMax;

                if (xHi < xLo || yHi < yLo)
                    continue;

                // La pièce quitte sa place le temps de l'essai : elle ne se gêne pas elle-même.
                Ink &self = ink[idx];
                for (const int64_t c : cells[idx])
                    --occupancy[static_cast<std::size_t>(c)];

                const int64_t pw = ext.xMax - ext.xMin;
                const int64_t ph = ext.yMax - ext.yMin;

                const int64_t minCols = std::max<int64_t>(1, (pw + cellW - 1) / cellW);
                const int64_t minRows = std::max<int64_t>(1, (ph + cellH - 1) / cellH);

                struct Hit
                {
                    bool              found = false;
                    int64_t           px    = 0;
                    int64_t           py    = 0;
                    int64_t           dist  = 0;
                    std::size_t       cells = 0;
                    std::set<int64_t> set;
                };

                // **La recherche, par BLOC de feuilles et non par position.**
                //
                // Le compte d'une pièce ne change qu'en traversant une frontière : les positions qui
                // donnent un compte donné forment donc un RECTANGLE. S'arrêter à ses coins — ce que
                // faisait la version précédente — c'est s'arrêter là où la pièce est le plus
                // contrainte, et dans un plan dense c'est exactement là qu'elle ne passe pas :
                // mesuré le 07-11-2026 sur un run réel, zéro position légale pour les neuf pièces.
                //
                // On énumère donc les BLOCS de feuilles qui coûteraient strictement moins que le
                // compte actuel, et on cherche une position légale n'importe où dedans : neuf points
                // par bloc — les coins, les milieux de bord et le centre. Le centre est le cas qui
                // compte, parce que la place libre est au milieu du bloc et pas collée à une
                // frontière.
                const Hit hit = [&]() -> Hit
                {
                    Hit best;

                    for (int64_t sc = minCols; sc <= columns; ++sc)
                    {
                        for (int64_t sr = minRows; sr <= rows; ++sr)
                        {
                            if (sc * sr >= static_cast<int64_t>(was))
                                continue;   // ce bloc ne peut pas coûter moins que maintenant

                            for (int64_t col = 0; col + sc <= columns; ++col)
                            {
                                const int64_t pxLo = std::max(xLo, col * cellW - ext.xMin);
                                const int64_t pxHi = std::min(xHi, (col + sc) * cellW - ext.xMax);
                                if (pxHi < pxLo)
                                    continue;

                                for (int64_t row = 0; row + sr <= rows; ++row)
                                {
                                    const int64_t pyLo = std::max(yLo, row * cellH - ext.yMin);
                                    const int64_t pyHi = std::min(yHi, (row + sr) * cellH - ext.yMax);
                                    if (pyHi < pyLo)
                                        continue;

                                    // Cinq points par axe, et non trois : le dégagement des
                                    // jonctions retire les bords de la bande, donc les milieux
                                    // sont ce qui reste — et un voisin mal placé peut encore
                                    // prendre le centre. Les points intermédiaires sont la marge
                                    // contre ce cas-là.
                                    for (int sy = 0; sy < 5; ++sy)
                                    {
                                        const int64_t py = pyLo + (pyHi - pyLo) * sy / 4;

                                        for (int sx = 0; sx < 5; ++sx)
                                        {
                                            const int64_t px = pxLo + (pxHi - pxLo) * sx / 4;

                                            const Clipper2Lib::Path64 drawn =
                                                translatePath(turned, px, py);

                                            // **Aucun bord de pièce sur une jonction.** C'est une
                                            // condition, pas un critère : la position est refusée.
                                            if (Grazes(drawn, bboxFromPath(drawn), vLines, hLines, gap - tol))
                                                continue;

                                            const Clipper2Lib::Path64 cand = translatePath(ref, px, py);
                                            const int blocked = Blocked(cand, bboxFromPath(cand), ink, i, forbidden);

                                            if (report != nullptr)
                                            {
                                                ++report->perPiece[idx].generated;
                                                if (blocked == 1)
                                                    ++report->perPiece[idx].zoneRefused;
                                            }

                                            if (blocked != 0)
                                                continue;

                                            const std::set<int64_t> next =
                                                Cells(drawn, cellW, cellH, columns, rows);

                                            if (report != nullptr)
                                            {
                                                SheetCount &one = report->perPiece[idx];
                                                ++one.tried;
                                                if (static_cast<int>(next.size()) < one.bestSeen)
                                                    one.bestSeen = static_cast<int>(next.size());
                                            }

                                            if (next.size() >= was)
                                                continue;

                                            const int64_t dx =
                                                px > origin.px ? px - origin.px : origin.px - px;
                                            const int64_t dy =
                                                py > origin.py ? py - origin.py : origin.py - py;
                                            const int64_t dist = dx + dy;

                                            const bool better = !best.found || next.size() < best.cells
                                                                || (next.size() == best.cells
                                                                    && dist < best.dist);
                                            if (!better)
                                                continue;

                                            // La garde du plan : une feuille de plus n'est pas un gain.
                                            if (UnionSize(next) > total)
                                            {
                                                if (report != nullptr)
                                                    report->perPiece[idx].refusedByTotal = true;
                                                continue;
                                            }

                                            best.found = true;
                                            best.px    = px;
                                            best.py    = py;
                                            best.dist  = dist;
                                            best.cells = next.size();
                                            best.set   = next;
                                        }
                                    }
                                }
                            }
                        }
                    }

                    return best;
                }();

                if (!hit.found)
                {
                    // La pièce retrouve sa place, à la case près comme à l'occupation près.
                    for (const int64_t c : cells[idx])
                        ++occupancy[static_cast<std::size_t>(c)];
                    continue;
                }

                state[idx] = {hit.px, hit.py, angle};
                self       = Draw(local[idx], angle, hit.px, hit.py);
                cells[idx] = hit.set;

                for (const int64_t c : cells[idx])
                    ++occupancy[static_cast<std::size_t>(c)];

                total = 0;
                for (const int count : occupancy)
                {
                    if (count > 0)
                        ++total;
                }

                out[idx].x        = static_cast<double>(hit.px) / kScale;
                out[idx].y        = static_cast<double>(hit.py) / kScale;
                out[idx].rotation = angle;

                if (report != nullptr)
                {
                    SheetCount &one = report->perPiece[idx];
                    one.best  = static_cast<int>(hit.cells);
                    one.turn  = turn;
                    one.moved = true;
                }

                improved = true;
                break;   // orientation gagnante trouvée : les autres n'ont plus d'objet
            }
        }

        if (!improved)
            break;
    }

    if (report != nullptr)
    {
        report->sheetsAfter = static_cast<int>(total);

        for (int i = 0; i < N; ++i)
        {
            if (cells[static_cast<std::size_t>(i)].size() > 1)
                ++report->straddlingAfter;
        }
    }

    return out;
}

//---------------------------------------------------------------------------------------------------------------------
auto Breathe(const Bin &bin,
             const std::vector<Clipper2Lib::Path64> &pieces,
             const std::vector<Placement> &placements,
             const Frame &frame,
             std::vector<Breath> *report) -> std::vector<Placement>
{
    const int64_t cellW  = ToFixed(frame.cellWidth);
    const int64_t cellH  = ToFixed(frame.cellHeight);
    const int64_t gap    = ToFixed(frame.gap);
    const int64_t margin = ToFixed(bin.marginMm());
    const int64_t binW   = ToFixed(bin.widthMm());

    const int64_t columns = frame.columns;
    const int64_t rows    = frame.rows;

    if (report != nullptr)
        report->assign(placements.size(), Breath{});

    if (cellW <= 0 || cellH <= 0 || binW <= 0 || columns < 1 || rows < 1)
        return placements;

    const int N = static_cast<int>(placements.size());
    if (N < 1 || static_cast<int>(pieces.size()) != N)
        return placements;

    const int64_t planH  = rows * cellH;
    const int64_t enough = ToFixed(0.2);    // en dessous, la pièce ne bouge pas
    const int64_t fine   = ToFixed(0.02);   // la finesse de la recherche
    const int64_t probe  = ToFixed(2.0);    // le pas où l'on demande « pourquoi pas »

    // ⚠️ **Un pixel de tolérance, et ce n'est pas du mou.**
    //
    // Le NFP du moteur est calculé sur un contour SIMPLIFIÉ à 0,5 px (`kNfpSimplifyEpsilon`) : un
    // plan peut donc laisser jusqu'à un demi-pixel de moins que le jeu nominal. Un test au bit près
    // sur le jeu exact déclare alors en collision une pièce que le moteur a posée LÉGALEMENT — et
    // comme cette collision existe à la position de départ, elle interdit aussi tout déplacement qui
    // reste dans son voisinage. Mesuré le 07-11-2026 : « bloquée par une pièce » dans les quatre
    // sens, sur une pièce qui avait 22 mm de libre à côté.
    const int64_t tol = ToFixed(1.0);

    const std::vector<Clipper2Lib::Path64> local = NestGeom::normalizeToLocal(pieces);

    std::vector<Clipper2Lib::Path64> forbidden;
    forbidden.reserve(bin.zones().size());
    for (const Bin::Zone &zone : bin.zones())
    {
        Clipper2Lib::Paths64 grown = Clipper2Lib::InflatePaths(
            {makeRect(ToFixed(zone.x), ToFixed(zone.y), ToFixed(zone.x + zone.width),
                      ToFixed(zone.y + zone.height))},
            static_cast<double>(gap), Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon);
        if (!grown.empty())
            forbidden.push_back(grown.front());
    }

    std::vector<int64_t> vLines;
    for (int64_t k = 1; k < columns; ++k)
        vLines.push_back(k * cellW);

    std::vector<int64_t> hLines;
    for (int64_t k = 1; k < rows; ++k)
        hLines.push_back(k * cellH);

    std::vector<Placement> out = placements;

    struct State
    {
        int64_t px;
        int64_t py;
        double  angle;
    };

    std::vector<State>             state(static_cast<std::size_t>(N));
    std::vector<Ink>               ink(static_cast<std::size_t>(N));
    std::vector<std::set<int64_t>> cells(static_cast<std::size_t>(N));

    for (int i = 0; i < N; ++i)
    {
        const std::size_t idx = static_cast<std::size_t>(i);
        state[idx].px    = ToFixed(placements[idx].x);
        state[idx].py    = ToFixed(placements[idx].y);
        state[idx].angle = placements[idx].rotation;

        ink[idx]   = Draw(local[idx], state[idx].angle, state[idx].px, state[idx].py);
        cells[idx] = Cells(ink[idx].abs, cellW, cellH, columns, rows);
    }

    for (int i = 0; i < N; ++i)
    {
        const std::size_t idx = static_cast<std::size_t>(i);

        const Clipper2Lib::Path64 turned = rotatePath(local[idx], state[idx].angle);
        const BBox ext = bboxFromPath(turned);

        // La pièce gonflée du jeu, une fois : c'est elle qu'on déplace et qu'on éprouve.
        const Clipper2Lib::Paths64 grown = Clipper2Lib::InflatePaths(
            {turned}, static_cast<double>(gap - tol), Clipper2Lib::JoinType::Miter,
            Clipper2Lib::EndType::Polygon);
        const Clipper2Lib::Path64 ref = grown.empty() ? turned : grown.front();

        // Pourquoi la pièce ne peut pas s'arrêter là — l'ordre est celui des tests.
        const auto Why = [&](int64_t px, int64_t py) -> int
        {
            // ⚠️ La tolérance est là pour le même défaut : une pièce que le moteur a posée PILE sur
            // la marge ressort « dehors » au bit près, et refuse alors tout déplacement.
            if (px + ext.xMin < margin - tol)
                return WhyLeft;
            if (px + ext.xMax > binW - margin + tol)
                return WhyRight;
            if (py + ext.yMin < margin - tol)
                return WhyTop;
            if (py + ext.yMax > planH - margin + tol)
                return WhyBottom;

            const Clipper2Lib::Path64 drawn = translatePath(turned, px, py);
            const BBox box = bboxFromPath(drawn);

            if (Grazes(drawn, box, vLines, hLines, gap - tol))
                return WhyJunction;

            const std::set<int64_t> next = Cells(drawn, cellW, cellH, columns, rows);
            for (const int64_t c : next)
            {
                if (cells[idx].count(c) == 0)
                    return WhyMorePage;
            }

            const Clipper2Lib::Path64 cand = translatePath(ref, px, py);
            const int blocker = Blocked(cand, bboxFromPath(cand), ink, i, forbidden);
            if (blocker == 1)
                return WhyZone;
            if (blocker == 2)
                return WhyPiece;

            return WhyAllowed;
        };

        // La plus grande distance où l'essai reste vrai, depuis une base, dans un sens.
        const auto Furthest = [&](int64_t base, int64_t room, bool positive, auto ok) -> int64_t
        {
            if (room <= 0)
                return 0;

            if (ok(base + (positive ? room : -room)))
                return room;

            int64_t good = 0;
            int64_t bad  = room;
            while (bad - good > fine)
            {
                const int64_t mid = good + (bad - good) / 2;
                if (ok(base + (positive ? mid : -mid)))
                    good = mid;
                else
                    bad = mid;
            }
            return good;
        };

        const int64_t baseX = state[idx].px;
        const int64_t baseY = state[idx].py;

        const int64_t roomRight = binW - margin - ext.xMax - baseX;
        const int64_t roomLeft  = baseX - margin + ext.xMin;
        const int64_t roomDown  = planH - margin - ext.yMax - baseY;
        const int64_t roomUp    = baseY - margin + ext.yMin;

        const auto allowedX = [&](int64_t x) { return Why(x, baseY) == WhyAllowed; };
        const auto allowedY = [&](int64_t y) { return Why(state[idx].px, y) == WhyAllowed; };
        const auto freeX    = [&](int64_t x)
        {
            const Clipper2Lib::Path64 c = translatePath(ref, x, baseY);
            return Blocked(c, bboxFromPath(c), ink, i, forbidden) == 0;
        };
        const auto freeY = [&](int64_t y)
        {
            const Clipper2Lib::Path64 c = translatePath(ref, state[idx].px, y);
            return Blocked(c, bboxFromPath(c), ink, i, forbidden) == 0;
        };

        const int64_t left  = Furthest(baseX, roomLeft, false, allowedX);
        const int64_t right = Furthest(baseX, roomRight, true, allowedX);
        const int64_t up    = Furthest(baseY, roomUp, false, allowedY);
        const int64_t down  = Furthest(baseY, roomDown, true, allowedY);

        const int64_t leftFree  = Furthest(baseX, roomLeft, false, freeX);
        const int64_t rightFree = Furthest(baseX, roomRight, true, freeX);
        const int64_t upFree    = Furthest(baseY, roomUp, false, freeY);
        const int64_t downFree  = Furthest(baseY, roomDown, true, freeY);

        // « Pourquoi pas 1 mm ? » ne se demande que d'un côté qui ne glisse pas du tout : c'est là
        // que le chiffre est muet et que la raison est tout ce qu'on ait à lire.
        const auto WhyAt = [&](int64_t d, bool positive, bool vertical) -> int
        {
            if (d >= probe)
                return WhyAllowed;

            if (vertical)
                return Why(state[idx].px, baseY + (positive ? probe : -probe));
            return Why(baseX + (positive ? probe : -probe), baseY);
        };

        const int whyLeft   = WhyAt(left, false, false);
        const int whyRight  = WhyAt(right, true, false);
        const int whyUp     = WhyAt(up, false, true);
        const int whyDown   = WhyAt(down, true, true);

        const auto Recentre = [&](int64_t base, int64_t before, int64_t after, bool vertical)
            -> int64_t
        {
            const int64_t shift = (after - before) / 2;
            if (shift == 0)
                return 0;

            const int64_t sign = shift > 0 ? 1 : -1;
            int64_t      d     = sign > 0 ? shift : -shift;

            const auto ok = [&](int64_t move)
            {
                return vertical ? Why(state[idx].px, base + sign * move) == WhyAllowed
                                : Why(base + sign * move, state[idx].py) == WhyAllowed;
            };

            if (!ok(d))
            {
                int64_t good = 0;
                int64_t bad  = d;
                while (bad - good > fine)
                {
                    const int64_t mid = good + (bad - good) / 2;
                    if (ok(base + sign * mid))
                        good = mid;
                    else
                        bad = mid;
                }
                d = good;
            }

            if (d < enough)
                return 0;

            if (vertical)
                state[idx].py = base + sign * d;
            else
                state[idx].px = base + sign * d;

            return sign * d;
        };

        const int64_t shiftX = Recentre(baseX, left, right, false);
        const int64_t shiftY = Recentre(state[idx].py, up, down, true);

        // Le compte rendu se remplit même quand rien ne bouge : « déjà centrée » et « coincée » ne
        // se lisent pas de la même façon, et les deux valent d'être dits.
        if (report != nullptr)
        {
            Breath &one = report->at(idx);

            one.leftSlackPx   = static_cast<double>(left) / kScale;
            one.rightSlackPx  = static_cast<double>(right) / kScale;
            one.topSlackPx    = static_cast<double>(up) / kScale;
            one.bottomSlackPx = static_cast<double>(down) / kScale;

            one.leftFreePx   = static_cast<double>(leftFree) / kScale;
            one.rightFreePx  = static_cast<double>(rightFree) / kScale;
            one.topFreePx    = static_cast<double>(upFree) / kScale;
            one.bottomFreePx = static_cast<double>(downFree) / kScale;

            one.leftRoomPx   = static_cast<double>(roomLeft > 0 ? roomLeft : 0) / kScale;
            one.rightRoomPx  = static_cast<double>(roomRight > 0 ? roomRight : 0) / kScale;
            one.topRoomPx    = static_cast<double>(roomUp > 0 ? roomUp : 0) / kScale;
            one.bottomRoomPx = static_cast<double>(roomDown > 0 ? roomDown : 0) / kScale;

            one.leftWhy   = whyLeft;
            one.rightWhy  = whyRight;
            one.topWhy    = whyUp;
            one.bottomWhy = whyDown;

            one.shiftXPx = static_cast<double>(shiftX) / kScale;
            one.shiftYPx = static_cast<double>(shiftY) / kScale;
            one.moved    = (shiftX != 0 || shiftY != 0);
        }

        if (shiftX == 0 && shiftY == 0)
            continue;

        ink[idx]   = Draw(local[idx], state[idx].angle, state[idx].px, state[idx].py);
        cells[idx] = Cells(ink[idx].abs, cellW, cellH, columns, rows);

        out[idx].x = static_cast<double>(state[idx].px) / kScale;
        out[idx].y = static_cast<double>(state[idx].py) / kScale;
    }

    return out;
}

//---------------------------------------------------------------------------------------------------------------------
auto ClosestBorderToJunction(const std::vector<Clipper2Lib::Path64> &pieces,
                             const std::vector<Placement> &placements,
                             const Frame &frame) -> double
{
    const int64_t cellW = ToFixed(frame.cellWidth);
    const int64_t cellH = ToFixed(frame.cellHeight);

    if (cellW <= 0 || cellH <= 0 || frame.columns < 1 || frame.rows < 1
        || pieces.size() != placements.size() || pieces.empty())
    {
        return -1.0;
    }

    std::vector<int64_t> vLines;
    for (int64_t k = 1; k < frame.columns; ++k)
        vLines.push_back(k * cellW);

    std::vector<int64_t> hLines;
    for (int64_t k = 1; k < frame.rows; ++k)
        hLines.push_back(k * cellH);

    if (vLines.empty() && hLines.empty())
        return -1.0;

    const std::vector<Clipper2Lib::Path64> local = NestGeom::normalizeToLocal(pieces);

    double closest = (std::numeric_limits<double>::max)();

    for (std::size_t i = 0; i < placements.size(); ++i)
    {
        const Clipper2Lib::Path64 drawn = translatePath(
            rotatePath(local[i], placements[i].rotation), ToFixed(placements[i].x),
            ToFixed(placements[i].y));

        for (const int64_t line : vLines)
            closest = std::min(closest, NestGeom::borderToLine(drawn, line, true));

        for (const int64_t line : hLines)
            closest = std::min(closest, NestGeom::borderToLine(drawn, line, false));
    }

    return closest / kScale;
}

} // namespace PageRepack
