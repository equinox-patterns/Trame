/************************************************************************
 **
 **  @file   Engine.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Nesting engine implementation — NFP + simulated annealing.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include "Engine.h"
#include "Bin.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <random>

namespace {

// A very large double returned when placement fails for a given arrangement.
constexpr double kBadHeight = 1e12;

} // anonymous namespace

// ── Shared geometry ──────────────────────────────────────────────────────────
//
// Le noyau géométrique — boîtes, rectangles, translations, rotations, NFP,
// région faisable — vit dans `nesting/NestGeom.h` depuis qu'un SECOND moteur
// est né : le moteur Tiled, qui paie en pages et non en hauteur, et qui a
// besoin du même noyau. Le recopier aurait été deux vérités pour une règle.
//
// Ces déclarations ramènent sous les noms nus que ce fichier employait avant
// l'extraction. **Aucun site d'appel ne bouge** : le diff se relit comme un
// déplacement pur, pas comme une réécriture.
using NestGeom::kScale;
using NestGeom::bboxFromPath;
using NestGeom::makeRect;
using NestGeom::translatePath;
using NestGeom::translatePaths;
using NestGeom::rotatePath;
using NestGeom::negatePath;
using NestGeom::findBottomLeft;
using NestGeom::nfpOutsideIFP;
using NestGeom::ensureCCW;
// Named, and not re-exported by Engine.h: a using-declaration at class scope may only
// name a base class's member, so the factory is reached through the kernel that owns it.
using NestGeom::makeNFPCacheKey;

// ── Forbidden zones ──────────────────────────────────────────────────────────
//
// The geometry moved to `nesting/ZoneGeom.h` when the Tiled engine had to honour
// the zones too: `NestZone::Build` and `NestZone::Absolute` are what used to stand
// here, and `NestZone::Subtract` is the loop that used to sit in placeSequential.
// Both engines call them now, so neither can answer "keep this rectangle free" its
// own way.
//
// Nothing is left behind as a forwarder on purpose: a `Engine::buildZoneGeom` that
// called `NestZone::Build` would have been a second name for one function.

// ── Sequential placement ─────────────────────────────────────────────────────

double Engine::placeSequential(const Bin &bin,
                               const std::vector<std::vector<RotGeom>> &rotBank,
                               NFPCache &nfpCache,
                               NFPCache &obstacleCache,
                               const std::vector<ZoneGeom> &zones,
                               const std::vector<int> &rotIdx,
                               const std::vector<int> &order,
                               std::vector<Placement> &out,
                               int64_t infY) const
{
    const int64_t binW    = static_cast<int64_t>(bin.widthMm() * kScale);
    const int64_t margin  = static_cast<int64_t>(bin.marginMm() * kScale);

    out.clear();
    out.resize(rotBank.size());
    for (auto &pl : out)
        pl.rotation = -1.0; // sentinel: not placed yet

    struct Placed {
        int     pieceIdx;
        int     rotIdx;
        int64_t px;
        int64_t py;
    };
    std::vector<Placed> placed;
    placed.reserve(order.size());

    // Forbidden zones — the A0 sheet emulation at the top-left corner, and the
    // pages a tiled plan must not print on. Each is treated as a pre-placed
    // piece whose NFP is subtracted from the IFP of every new piece. The
    // geometry arrives already built: this function runs once per annealing
    // iteration, so building it here would pay for K inflations hundreds of
    // times over (see NestZone::Build).

    // The deepest zone floors the reported height: the A0 sheet sits at the
    // corner, and a plan cannot be shorter than the sheet it was given.
    //
    // ⚠️ What this number IS, said plainly, because it is easy to over-read: it
    // is only ever the annealing's cost, compared candidate against candidate —
    // and the zone set is the same for the whole run, so it is a CONSTANT. It
    // changes no result. Its one job is to let this function read the zones
    // from a single place.
    int64_t maxYScaled = 0;
    for (const Bin::Zone &z : bin.zones())
    {
        const int64_t bottom = static_cast<int64_t>((z.y + z.height) * kScale);
        if (bottom > maxYScaled)
            maxYScaled = bottom;
    }

    for (int orderIdx = 0; orderIdx < static_cast<int>(order.size()); ++orderIdx) {
        const int pieceIdx = order[orderIdx];
        const int rIdx     = rotIdx[pieceIdx];
        const RotGeom &bg  = rotBank[pieceIdx][rIdx];
        const BBox &bboxB  = bg.bbox;

        int64_t ifpX0 = margin - bboxB.xMin;
        int64_t ifpX1 = binW - margin - bboxB.xMax;
        int64_t ifpY0 = margin - bboxB.yMin;

        if (ifpX1 <= ifpX0) {
            // A failure, and it has to LOOK like one. Writing the trial rotation here left
            // the piece indistinguishable from a placed one — x and y keep their default
            // (0, 0) — so a plan nothing could lay out came back as a plan, with a piece
            // sitting in the corner on the other pieces. Neither guard could see it: the
            // adjustment pass tests `rotation < 0`, and so does the host application. The
            // sentinel the unplaced pieces already carry is the one marker both read.
            out[pieceIdx].pieceIndex = pieceIdx;
            out[pieceIdx].rotation   = -1.0;
            return kBadHeight;
        }

        // Build feasible region = IFP rectangle minus NFPs of placed pieces +
        // obstacle.
        Clipper2Lib::Paths64 feasible;
        {
            Clipper2Lib::Clipper64 clipper;
            clipper.AddSubject({makeRect(ifpX0, ifpY0, ifpX1, infY)});

            for (const auto &p : placed) {
                const RotGeom &ag = rotBank[p.pieceIdx][p.rotIdx];

                if (nfpOutsideIFP(ag.bbox, bboxB, p.px, p.py, ifpX0, ifpX1, ifpY0))
                    continue;

                const NFPCacheKey key = makeNFPCacheKey(p.pieceIdx, p.rotIdx,
                                                        pieceIdx, rIdx);
                const Clipper2Lib::Paths64 *nfp = nullptr;
                auto it = nfpCache.find(key);
                if (it != nfpCache.end()) {
                    nfp = &it->second;
                } else {
                    // Full polygon is kept so concavities stay exploitable.
                    // Aref (inflated A) and negB (negated B) are precomputed.
                    Clipper2Lib::Paths64 nfpResult =
                        Clipper2Lib::MinkowskiSum(ag.aref, bg.neg, true);

                    // Ensure NFP paths are CCW (Clipper2 Difference requires CCW clips).
                    for (auto &np : nfpResult) {
                        if (Clipper2Lib::Area(np) < 0)
                            std::reverse(np.begin(), np.end());
                    }

                    auto [ins, _] = nfpCache.emplace(key, std::move(nfpResult));
                    nfp = &ins->second;
                }

                if (!nfp->empty()) {
                    const auto translated = translatePaths(*nfp, p.px, p.py);
                    clipper.AddClip(translated);
                }
            }

            // Subtract every forbidden zone's NFP — one call, because the Tiled
            // engine asks the same question and one rule may not have two answers.
            // What stands behind it is in `nesting/ZoneGeom.h`, with the note that
            // says why the cache key carries the zone index.
            NestZone::Subtract(clipper, zones, bg, pieceIdx, rIdx, ifpX0, ifpX1, ifpY0,
                               obstacleCache);

            clipper.Execute(Clipper2Lib::ClipType::Difference,
                           Clipper2Lib::FillRule::NonZero, feasible);
        }

        const Clipper2Lib::Point64 pos = findBottomLeft(feasible);
        if (pos.y >= infY) {
            // The same failure, the same marker — see the note on the other one.
            out[pieceIdx].pieceIndex = pieceIdx;
            out[pieceIdx].rotation   = -1.0;
            return kBadHeight;
        }

        const int64_t px = pos.x;
        const int64_t py = pos.y;

        Placement &pl = out[pieceIdx];
        pl.pieceIndex = pieceIdx;
        pl.x          = px / kScale;
        pl.y          = py / kScale;
        pl.rotation   = rIdx * m_config.rotationStep;

        placed.push_back({pieceIdx, rIdx, px, py});

        const int64_t pieceTop = py + bboxB.yMax;
        if (pieceTop > maxYScaled)
            maxYScaled = pieceTop;
    }

    // The cost is the height used — and the reason it is NOT the number of sheets is worth writing
    // down, because counting sheets is the obvious idea and it does not work.
    //
    // A tiled plan is paid for in sheets: the columns and the rows its ink reaches into. A cost
    // that counted them was tried, here, on 17-09-2026, and changed nothing at all — the ink
    // reached the right-hand edge of the bin in every run, before and after, for the plain reason
    // that the placement below is bottom-left and bottom-left fills the width before it stacks.
    // The search has no arrangement to offer in which the last column is empty, so no cost can
    // prefer one; a cost that cannot be acted on only distorts the search. What narrows a tiled
    // plan is the host's sheet rule — a sheet with nothing on it is not printed — and a hand on
    // the canvas: measured, emptying one column of a five-column plan took it from ten sheets to
    // eight.
    //
    // The height is what a ROLL is paid by, and a roll is what this engine has always nested.
    return maxYScaled / kScale;
}

// ── Simulated annealing ──────────────────────────────────────────────────────

bool Engine::anneal(const Bin &bin,
                    const std::vector<std::vector<RotGeom>> &rotBank,
                    NFPCache &nfpCache,
                    NFPCache &obstacleCache,
                    const std::vector<ZoneGeom> &zones,
                    std::vector<Placement> &best,
                    int64_t infY) const
{
    const int N = static_cast<int>(rotBank.size());
    if (N == 0)
        return false;

    std::mt19937 rng(42);

    // Initial state: order by area descending, rotation 0.
    std::vector<int> order(N);
    std::vector<int> rotIdx(N, 0);
    {
        std::vector<std::pair<double, int>> areaIdx;
        areaIdx.reserve(N);
        for (int i = 0; i < N; ++i)
            areaIdx.emplace_back(Clipper2Lib::Area(rotBank[i][0].path), i);
        std::sort(areaIdx.begin(), areaIdx.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        for (int i = 0; i < N; ++i)
            order[i] = areaIdx[i].second;
    }

    std::vector<Placement> current;
    double bestHeight = placeSequential(bin, rotBank, nfpCache, obstacleCache, zones,
                                        rotIdx, order, current, infY);
    best = current;

    double T = m_config.initialTemp;

    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::uniform_int_distribution<int> randPiece(0, N - 1);
    std::uniform_int_distribution<int> randSwapPos(0, N - 1);

    int plateauCount = 0;

    for (int iter = 0; iter < m_config.maxIterations && T > 0.01; ++iter) {
        // **Asked once an iteration, which is where a run spends its time.** The annealing walks
        // the pieces hundreds of times; a request to give up that waited for the whole search
        // would be a request answered after the person asking had stopped waiting.
        if (IsAbandoned())
            return true;

        std::vector<int> newOrder = order;
        std::vector<int> newRot   = rotIdx;

        if (u01(rng) < 0.5 && N >= 2) {
            int a = randSwapPos(rng);
            int b = randSwapPos(rng);
            while (b == a)
                b = randSwapPos(rng);
            std::swap(newOrder[a], newOrder[b]);
        } else {
            int p = randPiece(rng);
            int nRots = static_cast<int>(rotBank[p].size());
            if (nRots > 1) {
                std::uniform_int_distribution<int> randRot(0, nRots - 1);
                newRot[p] = randRot(rng);
            }
        }

        std::vector<Placement> neighbor;
        double newHeight = placeSequential(bin, rotBank, nfpCache, obstacleCache, zones,
                                           newRot, newOrder, neighbor, infY);

        const double delta = newHeight - bestHeight;
        if (delta < 0.0 || u01(rng) < std::exp(-delta / T)) {
            order = std::move(newOrder);
            rotIdx = std::move(newRot);
            if (newHeight < bestHeight) {
                bestHeight = newHeight;
                best = std::move(neighbor);
                plateauCount = 0;
            } else {
                ++plateauCount;
            }
        } else {
            ++plateauCount;
        }

        if (plateauCount >= m_config.plateauLimit && T < 0.1)
            break;

        T *= m_config.coolingRate;
    }

    return false;
}

// ── Public API ────────────────────────────────────────────────────────────────

Engine::Engine(const Config &cfg) noexcept
    : m_config(cfg)
{
}

std::vector<Placement> Engine::place(const Bin &bin,
                                     const std::vector<Clipper2Lib::Path64> &pieces,
                                     bool *abandoned) const
{
    if (abandoned != nullptr)
        *abandoned = false;

    const int N = static_cast<int>(pieces.size());
    if (N == 0)
        return {};

    assert(m_config.gap >= 0.0 && "gap must be non-negative");
    assert(m_config.rotationStep > 0.0 && "rotationStep must be positive");
    assert(m_config.coolingRate > 0.0 && m_config.coolingRate < 1.0
           && "coolingRate must be in (0, 1)");

    const int numRots = m_config.freeRotation
        ? static_cast<int>(360.0 / m_config.rotationStep)
        : 1;

    // Contours ramenés sur leur propre coin min, puis banque de rotations.
    //
    // Les deux règles qui vivaient ici — le gonflement au jeu ENTIER (jamais la
    // moitié, qui ne rendait que gap/2 sur le plancher avant que les dégagements
    // soient mesurés le 15-09) et le fait que les NFP gardent le polygone
    // complet pour que les concavités restent exploitables — sont écrites à
    // côté du code qui les applique, dans `nesting/NestGeom.h`. Elles valent
    // désormais pour les DEUX moteurs.
    const std::vector<Clipper2Lib::Path64> local = NestGeom::normalizeToLocal(pieces);
    const std::vector<std::vector<RotGeom>> rotBank =
        NestGeom::buildRotBank(local, m_config.rotationStep, numRots,
                               static_cast<int64_t>(m_config.gap * kScale));

    // The bin's forbidden zones, as engine geometry, built ONCE for the whole
    // run: the annealing below calls placeSequential hundreds of times, and
    // rebuilding K inflations on each of those calls is what this avoids.
    const std::vector<ZoneGeom> zones =
        NestZone::Build(bin.zones(), static_cast<int64_t>(m_config.gap * kScale));

    NFPCache nfpCache;
    nfpCache.reserve(std::size_t(N) * numRots * 4);
    NFPCache obstacleCache; // keyed (piece, rot, zone); a zone NFP is independent of context
    obstacleCache.reserve(std::size_t(N) * numRots * std::max<std::size_t>(zones.size(), 1));

    // Safe height sentinel: sum of piece heights (rotation 0) + 50% margin.
    int64_t totalH = 0;
    for (int i = 0; i < N; ++i) {
        if (rotBank[i].empty())
            continue;
        totalH += rotBank[i][0].bbox.yMax - rotBank[i][0].bbox.yMin;
    }
    totalH = totalH + totalH / 2;
    constexpr int64_t kMaxSafe = int64_t(1) << 50;
    const int64_t infY = totalH > 0 ? totalH : kMaxSafe;

    // Asked before the search starts as well: a run given up while its pieces were being read has
    // no reason to search for a plan nobody is waiting for any more.
    if (IsAbandoned()) {
        if (abandoned != nullptr)
            *abandoned = true;
        return {};
    }

    std::vector<Placement> best;
    if (anneal(bin, rotBank, nfpCache, obstacleCache, zones, best, infY)) {
        // **What came back is a half-built plan, and is handed back as one.** The caller is told
        // the run was given up; a caller that reads the placements without asking has taken a
        // decision that was never made. See the note on place() in the header.
        if (abandoned != nullptr)
            *abandoned = true;
        return best;
    }

    // The adjustment pass — strictly after the search, never inside it. The
    // grid is read here and nowhere else: the annealing above runs exactly as
    // it did before tiling existed, and without a grid this line does nothing.
    if (m_config.artboardCount > 0 && m_config.artboardHeight > 0.0) {
        if (refine(bin, rotBank, best, pieces)) {
            if (abandoned != nullptr)
                *abandoned = true;
        }
    }

    return best;
}

//---------------------------------------------------------------------------------------------------------------------
auto Engine::SheetRows(double usedHeight, double artboardHeight) -> int
{
    if (artboardHeight <= 0.0) {
        return 1;
    }

    // The tolerance, subtracted rather than added: a height that lands on the boundary counts as
    // the row below it. See the note in the header — the two callers and the host application all
    // mean the same thing by it, and the figure is the host's own.
    const double rows = std::ceil(usedHeight / artboardHeight - 1e-9);
    return static_cast<int>(std::max(rows, 1.0));
}

//---------------------------------------------------------------------------------------------------------------------
double Engine::UsedHeight(const std::vector<Placement> &placements,
                          const std::vector<Clipper2Lib::Path64> &pieces,
                          const Bin &bin)
{
    double usedHeight = 0.0;

    for (size_t i = 0; i < placements.size() && i < pieces.size(); ++i)
    {
        int64_t xMin = (std::numeric_limits<int64_t>::max)();
        int64_t yMin = (std::numeric_limits<int64_t>::max)();
        for (const auto &pt : pieces[i])
        {
            if (pt.x < xMin) { xMin = pt.x; }
            if (pt.y < yMin) { yMin = pt.y; }
        }

        // The engine places the piece by the min-bbox corner of its *unrotated*
        // polygon, so the envelope is measured from that same origin.
        Clipper2Lib::Path64 local;
        local.reserve(pieces[i].size());
        for (const auto &pt : pieces[i])
        {
            local.push_back({pt.x - xMin, pt.y - yMin});
        }

        int64_t ryMax = (std::numeric_limits<int64_t>::min)();
        if (placements[i].rotation != 0.0)
        {
            const double rad = placements[i].rotation * M_PI / 180.0;
            const double c   = std::cos(rad);
            const double s   = std::sin(rad);
            for (const auto &pt : local)
            {
                const auto y = static_cast<int64_t>(std::round(pt.x * s + pt.y * c));
                if (y > ryMax) { ryMax = y; }
            }
        }
        else
        {
            for (const auto &pt : local)
            {
                if (pt.y > ryMax) { ryMax = pt.y; }
            }
        }

        const double bottom = placements[i].y + static_cast<double>(ryMax) / kScale;
        if (bottom > usedHeight) { usedHeight = bottom; }
    }

    // The top margin is already part of the placement area: the IFP starts at y = margin.
    return usedHeight + bin.marginMm();
}
