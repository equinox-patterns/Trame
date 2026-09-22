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
 **  All rights reserved.
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
    clipper.AddSubject({makeRect(ifpX0, ifpY0, ifpX1, ifpY1)});

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
        // ⚠️ **Le contour tel qu'il est DESSINÉ, et non tel que le moteur le range.** La
        // convention de l'hôte — et celle de `NestedSvgWriter` — est
        // `translate(x, y) · rotate(deg) · translate(-ox, -oy)` : le placement porte le coin min
        // de la pièce **NON tournée**, et l'angle s'applique autour de lui.
        //
        // `rotBank[i][rotIdx].path` est la pièce tournée **puis re-normalisée sur SON propre coin
        // min** : la translater de (px, py) plaçait le coin de la pièce tournée, pas celui de la
        // pièce d'origine — un décalage d'une case, et un juge qui compte faux. On repart donc de
        // la rotation 0, on tourne, on translate.
        const Clipper2Lib::Path64 abs = NestGeom::translatePath(
            NestGeom::rotatePath(rotBank[p.pieceIdx][0].path, p.rotIdx * stepDeg), p.px, p.py);
        const BBox bb = bboxFromPath(abs);
        deepest = std::max(deepest, bb.yMax);

        const int64_t c0 = std::max<int64_t>(0, bb.xMin / sheetW);
        const int64_t c1 = std::min<int64_t>(cols - 1, bb.xMax / sheetW);
        const int64_t r0 = std::max<int64_t>(0, bb.yMin / sheetH);
        const int64_t r1 = std::min<int64_t>(rows - 1, bb.yMax / sheetH);

        for (int64_t r = r0; r <= r1; ++r)
        {
            for (int64_t c = c0; c <= c1; ++c)
            {
                const Clipper2Lib::Path64 cell =
                    makeRect(c * sheetW, r * sheetH, (c + 1) * sheetW, (r + 1) * sheetH);
                if (!Clipper2Lib::Intersect({abs}, {cell}, Clipper2Lib::FillRule::NonZero).empty())
                {
                    touched.insert(r * cols + c);
                }
            }
        }
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

    const int64_t binW   = static_cast<int64_t>(bin.widthMm() * kScale);
    const int64_t margin = static_cast<int64_t>(bin.marginMm() * kScale);
    const int64_t gapR   = static_cast<int64_t>(m_spec.gap * kScale);
    const int64_t sheetH = static_cast<int64_t>(m_spec.sheetHeight * kScale);

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
    // ⚠️ Lue SEULEMENT s'il y a des zones. Sans zone la boucle est laissée
    // exactement telle qu'elle était, et un run sans page de garde ne peut pas être
    // changé par un garde-fou qui ne se lit pas.
    int64_t maxRows = 1;
    if (!zones.empty())
    {
        for (int i = 0; i < N; ++i)
        {
            const BBox &b = rotBank[i][0].bbox;
            maxRows += (b.yMax - b.yMin + sheetH - 1) / sheetH;
        }
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

        // Une pièce plus large que le bin ne se placera jamais, quel que soit
        // le nombre de rangées : ouvrir boucherait sans fin. Même marqueur que
        // le moteur de bande — l'hôte teste `rotation < 0`.
        if (margin - b0.xMin >= binW - margin - b0.xMax)
        {
            out[pieceIdx].pieceIndex = pieceIdx;
            out[pieceIdx].rotation   = -1.0;
            return out;
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
                // Sauf quand une zone interdite a mangé la place : les rangées ne la
                // rendront pas, et en ouvrir indéfiniment serait une boucle sans fin.
                // Même marqueur que la pièce trop large, et pour la même raison —
                // `rotation < 0` est ce que la passe 2 et l'hôte lisent — et le plan
                // entier est rendu tel quel. Ostara le déclarera incomplet par
                // `AllPlaced` et gardera celui du moteur de bande, qui a déjà posé
                // toutes les pièces.
                if (!zones.empty() && rows >= maxRows)
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
    const int64_t sheetW = static_cast<int64_t>(m_spec.sheetWidth * kScale);
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
                                       candidate.pieceIdx, rIdx, binW, rows * sheetH, margin, zones,
                                       nfpCache, obstacleCache);

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

    return out;
}
