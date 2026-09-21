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

#include <algorithm>
#include <cmath>

using NestGeom::kScale;
using NestGeom::bboxFromPath;
using NestGeom::makeRect;
using NestGeom::translatePaths;
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
                                    Engine::NFPCache &nfpCache)
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
            Engine::makeNFPCacheKey(p.pieceIdx, p.rotIdx, pieceIdx, rIdx);

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

    clipper.Execute(Clipper2Lib::ClipType::Difference,
                    Clipper2Lib::FillRule::NonZero, out);
    return out;
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
            byHolder.emplace_back(w * h, i);
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

    Engine::NFPCache nfpCache;
    nfpCache.reserve(std::size_t(N) * numRots * 4);

    // ── L'enveloppe ───────────────────────────────────────────────────────
    //
    // La largeur est celle du bin — c'est-à-dire le MAXIMUM de colonnes que
    // l'utilisatrice a saisi. Une rangée peut en utiliser moins : les cases
    // qu'elle ne touche pas ne s'impriment pas, et c'est là que le plan cesse
    // d'être un rectangle. Seul le nombre de RANGÉES s'ouvre.
    int64_t rows = 1;

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

            for (int rIdx = 0; rIdx < numRots; ++rIdx)
            {
                const Clipper2Lib::Paths64 region =
                    feasibleRegion(placed, rotBank, rotBank[pieceIdx][rIdx],
                                   pieceIdx, rIdx, binW, envH, margin, nfpCache);

                const Clipper2Lib::Point64 pos = findBottomLeft(region);

                // `findBottomLeft` rend y = INT64_MAX sur une région vide, donc
                // ce test couvre les deux cas : rien à placer, ou rien qui
                // tienne dans l'enveloppe courante.
                if (pos.y >= envH)
                    continue;

                if (bestRot < 0 || pos.y < bestY || (pos.y == bestY && pos.x < bestX))
                {
                    bestRot = rIdx;
                    bestX   = pos.x;
                    bestY   = pos.y;
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
                ++rows;
            }
        }
    }

    return out;
}
