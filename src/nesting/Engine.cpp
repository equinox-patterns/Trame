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

#include "Engine.h"
#include "Bin.h"

#include <algorithm>
#include <chrono>
#include <cassert>

#include <QDebug>
#include <cmath>
#include <limits>
#include <random>
#include <atomic>
#include <thread>

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
using NestGeom::findBottomAtX;
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

// ── Les sommes d'un passage, de front ────────────────────────────────────────

void Engine::PrewarmNfp(const std::vector<std::vector<RotGeom>> &rotBank,
                        const std::vector<int> &rotIdx,
                        const std::vector<int> &order,
                        NFPCache &nfpCache) const
{
    struct Job
    {
        NestGeom::NFPCacheKey key;
        int aPiece, aRot, bPiece, bRot;
    };

    std::vector<Job> jobs;
    const int N = static_cast<int>(order.size());
    jobs.reserve(static_cast<std::size_t>(N) * static_cast<std::size_t>(N) / 2 + 1);

    for (int i = 1; i < N; ++i)
    {
        const int bPiece = order[static_cast<std::size_t>(i)];
        const int bRot = rotIdx[static_cast<std::size_t>(bPiece)];
        for (int j = 0; j < i; ++j)
        {
            const int aPiece = order[static_cast<std::size_t>(j)];
            const int aRot = rotIdx[static_cast<std::size_t>(aPiece)];
            const NestGeom::NFPCacheKey key = NestGeom::makeNFPCacheKey(aPiece, aRot, bPiece, bRot);
            if (nfpCache.find(key) != nfpCache.end())
                continue;
            jobs.push_back({key, aPiece, aRot, bPiece, bRot});
        }
    }

    if (jobs.empty())
        return;

    const unsigned hardware = std::thread::hardware_concurrency();
    const unsigned threads = (std::min)(hardware == 0 ? 1u : hardware,
                                       static_cast<unsigned>(jobs.size()));

    std::vector<Clipper2Lib::Paths64> results(jobs.size());
    std::atomic<std::size_t> next{0};

    const auto worker = [&]()
    {
        for (;;)
        {
            const std::size_t index = next.fetch_add(1);
            if (index >= jobs.size())
                return;
            const Job &job = jobs[index];
            const RotGeom &placed = rotBank[static_cast<std::size_t>(job.aPiece)]
                                                [static_cast<std::size_t>(job.aRot)];
            const RotGeom &candidate = rotBank[static_cast<std::size_t>(job.bPiece)]
                                                   [static_cast<std::size_t>(job.bRot)];
            Clipper2Lib::Paths64 nfp =
                Clipper2Lib::MinkowskiSum(placed.aref, candidate.neg, true);
            for (auto &np : nfp)
            {
                if (Clipper2Lib::Area(np) < 0)
                    std::reverse(np.begin(), np.end());
            }
            results[index] = std::move(nfp);
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back(worker);
    for (std::thread &thread : pool)
        thread.join();

    // L'insertion, en série : c'est le seul instant où la table est écrite.
    for (std::size_t i = 0; i < jobs.size(); ++i)
        nfpCache.emplace(jobs[i].key, std::move(results[i]));
}

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
    const int64_t marginL = static_cast<int64_t>(bin.margins().left * kScale);
    const int64_t marginR = static_cast<int64_t>(bin.margins().right * kScale);
    const int64_t marginT = static_cast<int64_t>(bin.margins().top * kScale);
    // The bottom margin is not read here and has nothing to read: the strip is open downwards, so
    // nothing forbids a piece from sitting at the end of the cloth. It is what UsedHeight adds
    // afterwards — see the note there.

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

        // ⚠️ **La marge du côté du pli est PAR PIÈCE, et elle ne rétrécit jamais le bac.**
        //
        // Sur tissu plié, le pli court sur un grand bord et l'appelant demande une marge de ZÉRO de
        // ce côté — une pièce coupée au pli doit avoir son pli exactement SUR le pli du tissu. Mais une
        // marge de bac vaut pour toutes les pièces : ce zéro ouvrait le bord plié à TOUT LE MONDE, et
        // l'écart saisi dans le champ était ignoré de ce côté. C'est le défaut.
        //
        // Le zéro appartient donc aux pièces ANCRÉES à cette paroi, et à elles seules. Les autres
        // respectent l'écart — mais **en portant la contrainte sur leur propre position**, jamais en
        // rétrécissant la zone : rétrécir le bac retirait un centimètre à toutes les pièces alors que
        // la laize pliée n'en fait déjà que la moitié, et le plan entier devenait impossible.
        //
        // Et l'écart est TENU, sans condition : la hauteur du bac est libre, donc manquer de place
        // n'est jamais une raison de céder sur la largeur — le plan descend, et voilà tout. Céder sur
        // l'écart serait décider à la place de l'utilisatrice d'une chose qu'elle a tapée.
        //
        // Le côté plié est celui où les marges diffèrent : l'appelant y écrit 0 et l'écart de l'autre.
        // Des marges égales — tissu déplié, ou écart nul partout — n'exemptent rien.
        const int64_t edgePx = std::max(marginL, marginR);
        const bool foldsOnLeft = marginL < marginR;
        const bool foldsOnRight = marginR < marginL;
        const int64_t wantedL =
            (foldsOnLeft && bg.anchor != Anchor::LeftWall) ? edgePx : marginL;
        const int64_t wantedR =
            (foldsOnRight && bg.anchor != Anchor::RightWall) ? edgePx : marginR;

        int64_t ifpX0 = wantedL - bboxB.xMin;
        int64_t ifpX1 = binW - wantedR - bboxB.xMax;
        int64_t ifpY0 = marginT - bboxB.yMin;

        // **A glued piece has no position across the bin to choose.** `ifpX0` is the origin that
        // puts the rotated path flush against the inner left edge and `ifpX1` the one that puts it
        // flush against the inner right edge — the two walls, each read off the window the piece is
        // placed in. For a glued piece that is the only x there is: the wall decides it, and what is
        // left to the search is how far the piece slides along the wall. It is what a piece cut on
        // the fold needs — its fold line has to sit ON the fold, and a hair beside it is a piece cut
        // to the wrong width. Both walls matter: fabric folded so its two selvedges meet inward has
        // a fold on each long edge, so one half of the stack pins to one wall and the other to the
        // other.
        //
        // The window the region is built from is left alone: the three tests below still ask
        // whether a piece fits between the margins, and `ifpX0`/`ifpX1` are still the margins it
        // must clear. Only the POSITION gets read differently — off the wall line, among the
        // vertices that sit on it. See NestGeom::findBottomAtX for why a band is not the answer.
        //
        // ⚠️ **What the wall is WORTH, and who answers for it.** `ifpX0` sits `margins.left` from
        // the bin's edge, and `ifpX1` `margins.right` from it the other way, so a glued piece's fold
        // line ends up that far from the fold. For a piece cut on the fold there is exactly ONE
        // right value there, and it is **zero** — the engine knows nothing of folds and cannot
        // decide this. The caller does, and writes it in one place: `SeatMargins`, in
        // oexportnesting.cpp.
        const Anchor anchor = bg.anchor;
        const bool gluedLeft = anchor == Anchor::LeftWall;
        const bool gluedRight = anchor == Anchor::RightWall;

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

        Clipper2Lib::Point64 pos = {int64_t(0), int64_t(0)};
        if (gluedLeft) {
            pos = findBottomAtX(feasible, ifpX0);
        } else if (gluedRight) {
            pos = findBottomAtX(feasible, ifpX1);
        } else {
            pos = findBottomLeft(feasible);
        }
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
        // The angle the bank was built at, never the index times the step: a constrained piece
        // has its own angles, and they are not multiples of anything.
        pl.rotation   = rotBank[pieceIdx][static_cast<std::size_t>(rIdx)].angle;

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

    PrewarmNfp(rotBank, rotIdx, order, nfpCache);

    std::vector<Placement> current;
    double bestHeight = placeSequential(bin, rotBank, nfpCache, obstacleCache, zones,
                                        rotIdx, order, current, infY);
    best = current;

    double T = m_config.initialTemp;

    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::uniform_int_distribution<int> randPiece(0, N - 1);
    std::uniform_int_distribution<int> randSwapPos(0, N - 1);

    int plateauCount = 0;

    // L'horloge du budget : lue seulement si un budget est demandé, mais posée ici pour que la
    // boucle n'ait pas deux formes selon le réglage.
    const auto searchStart = std::chrono::steady_clock::now();

    for (int iter = 0; iter < m_config.maxIterations && T > 0.01; ++iter) {

        // **Le budget, s'il y en a un.** Lu une fois par itération : c'est le seul endroit où le
        // coût d'un passage est connu sans instrumenter le placement, et un passage dure de l'ordre
        // de la seconde sur un patron réel.
        // ⚠️ **Et jamais tant qu'aucun plan n'est trouvé.** Le budget coupe la recherche de la
        // réponse parfaite, pas la recherche d'une réponse : tant que `bestHeight` vaut
        // `kBadHeight`, aucune pièce n'est posée, et s'arrêter là rend un plan vide que l'hôte
        // annonce comme « document trop étroit » — un mensonge, et celui qui coûte le plus cher.
        if (m_config.maxMilliseconds > 0 && bestHeight < kBadHeight * 0.5)
        {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - searchStart).count();
            if (spent > m_config.maxMilliseconds)
            {
                break;
            }
        }

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

        // Une somme de Minkowski pèse des dizaines de kilooctets : un cache qui ne se vide jamais
        // finirait par coûter plus cher que le calcul qu'il évite. Mesuré : le préchauffage propose
        // jusqu'à 91 sommes par passage, pour 456 passages. On borne donc la table, et on repart à
        // vide — les passages ne réutilisent presque jamais une orientation déjà essayée.
        constexpr std::size_t kMaxCachedNfp = 12000;
        if (nfpCache.size() > kMaxCachedNfp)
            nfpCache.clear();

        // Les sommes du passage, de front, avant de le poser : voir PrewarmNfp.
        PrewarmNfp(rotBank, newRot, newOrder, nfpCache);

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

        // ⚠️ **Un palier est un palier, quelle que soit la température.** La condition `T < 0.1`
        // rendait ce frein inatteignable avant l'itération 343 : `plateauLimit` itérations sans
        // améliorer le meilleur plan disent que la recherche ne trouve plus rien, et attendre que
        // le métal refroidisse ne lui fera pas trouver davantage — ça ne fait que replacer toutes
        // les pièces trois cents fois de plus.
        // ⚠️ Même règle : un palier de HAUTEUR ne veut rien dire sur un plan qui n'en a pas.
        if (bestHeight < kBadHeight * 0.5 && plateauCount >= m_config.plateauLimit)
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
                                     const std::vector<std::vector<double>> &allowedAngles,
                                     const std::vector<Anchor> &anchors,
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
    // ── L'empreinte de nidification, allégée d'un dixième de millimètre ─────────────────────────
    //
    // Le dessin garde sa géométrie exacte ; c'est l'empreinte POSÉE qui est simplifiée. Gonflée du
    // double de la tolérance avant d'être simplifiée, pour qu'elle ne puisse pas rapetisser : voir
    // `NestGeom::simplifyForNesting`.
    //
    // ⚠️ **Un dixième de millimètre, et c'est une règle mesurée, pas un choix de confort.**
    // Le plan de ce patron est emboîté au millimètre : élargir l'empreinte de chaque pièce casse les
    // creux qui accueillent les autres, et la longueur ne bouge pas proportionnellement — elle saute.
    // Mesuré à l'écran, taille 58 : 0,1 mm → 324,7 cm en 1 min 25 ; 0,3 mm → 339,9 cm en 20 s ;
    // 0,5 mm → 339,9 cm en 10 s. Le saut est entre 0,1 et 0,3, et les 15 cm ne valent pas la minute
    // gagnée. **On garde 0,1 mm.** Le gain de temps viendra d'ailleurs : le gonflement protecteur
    // ci-dessous est la piste suivante (voir `NestGeom::simplifyForNesting`).
    constexpr int64_t kTidyFixed = 378000; // 0,1 mm, en unités fixes (1 px = 1e6)
    const std::vector<Clipper2Lib::Path64> tidied =
        NestGeom::simplifyForNesting(pieces, kTidyFixed);

    const std::vector<Clipper2Lib::Path64> local = NestGeom::normalizeToLocal(tidied);
    std::vector<std::vector<RotGeom>> rotBank =
        NestGeom::buildRotBank(local, m_config.rotationStep, numRots,
                               static_cast<int64_t>(m_config.gap * kScale), allowedAngles, anchors);

    // ── Le contrôle d'AVANT: une orientation plus large que le bac ne s'essaie jamais ──────────
    //
    // ⚠️ **Un passage abandonne TOUT dès qu'une pièce ne rentre pas en largeur** (`placeSequential`
    // pose, puis rend `kBadHeight`). Or l'orientation de chaque pièce est tirée au sort par le
    // recuit : sur 14 pièces dont quelques-unes frôlent la largeur utile, la chance qu'un passage
    // les ait TOUTES dans une orientation qui rentre est faible — le recuit jetait donc presque
    // tous ses passages et rendait un plan incomplet, en accusant le document d'être trop étroit.
    //
    // Mesuré sur le patron client, rouleau A0 : `Top V2 Devant` fait 87,9 cm couché à 0° pour 83,1
    // cm utiles — elle ne rentre qu'à partir d'une rotation, et le moteur ne lui en laissait pas
    // l'occasion. On retire donc de la banque, PIÈCE PAR PIÈCE, les orientations qui ne passent pas
    // en largeur. Le moteur ne peut plus tirer qu'une orientation posable.
    //
    // Et le cas qui reste vraiment impossible — aucune orientation ne rentre — se voit ici, avant
    // toute recherche : le run s'arrête tout de suite au lieu d'attendre des minutes pour rien.
    // L'hôte le nomme avec sa largeur réelle (`TooNarrowFactsOf`), parce que la pièce revient non
    // placée comme les autres.
    {
        const int64_t usableW = static_cast<int64_t>(std::llround(
            (bin.widthMm() - bin.margins().left - bin.margins().right) * kScale));
        bool impossible = false;
        for (int i = 0; i < N && !impossible; ++i) {
            std::vector<RotGeom> &bank = rotBank[i];
            const auto tooWide = [usableW](const RotGeom &g) {
                return (g.bbox.xMax - g.bbox.xMin) > usableW;
            };
            bank.erase(std::remove_if(bank.begin(), bank.end(), tooWide), bank.end());
            if (bank.empty()) {
                impossible = true;
            }
        }

        if (impossible) {
            // Ce que l'hôte reçoit : une pièce non placée par entrée, marquée comme telle. Il en
            // conclut « ça ne tient pas » et NOMME la pièce dont aucune orientation ne rentre.
            std::vector<Placement> none(static_cast<std::size_t>(N));
            for (int i = 0; i < N; ++i) {
                none[static_cast<std::size_t>(i)].pieceIndex = i;
                none[static_cast<std::size_t>(i)].rotation = -1.0;
            }
            return none;
        }
    }

    // The bin's forbidden zones, as engine geometry, built ONCE for the whole
    // run: the annealing below calls placeSequential hundreds of times, and
    // rebuilding K inflations on each of those calls is what this avoids.
    const std::vector<ZoneGeom> zones =
        NestZone::Build(bin.zones(), static_cast<int64_t>(m_config.gap * kScale));

    NFPCache nfpCache;
    nfpCache.reserve(std::size_t(N) * numRots * 4);
    NFPCache obstacleCache; // keyed (piece, rot, zone); a zone NFP is independent of context
    obstacleCache.reserve(std::size_t(N) * numRots * std::max<std::size_t>(zones.size(), 1));

    // ── Le plafond de hauteur : **la plus GRANDE hauteur de chaque pièce, pas celle à 0°** ──────
    //
    // Un plan peut toujours tenir en une seule colonne : une pièce par rangée. Sa hauteur vaut donc
    // au plus la somme des hauteurs des pièces, chacune dans l'orientation où le moteur la pose.
    // Ce plafond-ci est le seul qui ne puisse pas refuser un plan qui existe.
    //
    // ⚠️ **Il se calculait sur la hauteur à 0°**, plus 50 %. Or une pièce presque aussi large que le
    // bac ne rentre en largeur qu'en se couchant — et couchée elle est bien plus HAUTE qu'à 0°. Sur
    // les grandes tailles, la somme des hauteurs réellement posées dépassait alors le plafond, et le
    // placement rendait « aucune place » sur un plan qui tenait : l'hôte annonçait « document trop
    // étroit » pour des pièces qui rentraient. C'est le plafond qui était faux, pas la place.
    //
    // Les marges s'ajoutent : le plafond est comparé à une ordonnée absolue dans le bac, pas à une
    // hauteur. Sans elles, un bac qui réserve de l'espace en haut et en bas refusait la dernière
    // rangée d'un plan qui tenait exactement.
    int64_t totalH = 0;
    for (int i = 0; i < N; ++i) {
        if (rotBank[i].empty())
            continue;
        int64_t tallest = 0;
        for (const RotGeom &rot : rotBank[i]) {
            const int64_t h = rot.bbox.yMax - rot.bbox.yMin;
            if (h > tallest)
                tallest = h;
        }
        totalH += tallest;
    }
    totalH += static_cast<int64_t>(bin.margins().top * kScale);
    totalH += static_cast<int64_t>(bin.margins().bottom * kScale);
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
    //
    // ⚠️ And never on a plan that has a piece glued to a wall. That pass translates BLOCKS of
    // pieces to bring their borders off the frame lines: it moves a piece it did not choose, and a
    // glued piece that is moved is a piece off the fold — the one thing the caller pinned it for.
    // Nothing here checks the glued pieces one by one, because there is no version of that pass
    // that may touch one: a plan glued to a wall is not a plan it may rearrange.
    const bool anyGlued = std::any_of(anchors.cbegin(), anchors.cend(),
                                      [](Anchor a) { return a != Anchor::Free; });
    if (!anyGlued && m_config.artboardCount > 0 && m_config.artboardHeight > 0.0) {
        if (refine(bin, rotBank, best, pieces)) {
            if (abandoned != nullptr)
                *abandoned = true;
        }
    } else if (!anyGlued) {
        // ── Le plan de BANDE a sa seconde passe aussi ─────────────────────────────────────────
        //
        // Sans grille, il n'y a pas de lignes de pages à aligner — mais il reste le blanc entre les
        // pièces, et la longueur qu'il coûte. `refineStrip` relit les étages du plan, en tire
        // plusieurs ordres et repose les mêmes pièces aux mêmes orientations ; elle ne commite que
        // si le plan est strictement plus court. Voir EngineRefine.cpp.
        //
        // ⚠️ Jamais sur un plan qui a une pièce collée à un bord : cette passe repose les pièces
        // dans un autre ordre, et une pièce collée au pli qui se déplace est une pièce hors du pli.
        if (refineStrip(bin, rotBank, nfpCache, obstacleCache, zones, best, pieces, infY)) {
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

    // The TOP margin is already part of the placement area — `ifpY0 = margins.top - bbox.yMin`, so
    // the first piece starts that far from the edge — and what is added here is the BOTTOM one: the
    // strip runs from the start of the yardage to its end, and the last rectangle keeps its space
    // there.
    //
    // ⚠️ **On a bin with a single margin the bottom equals the top, so this reports exactly what it
    // reported before there were four.** This line, and not another, is where the non-regression of
    // every strip run is read.
    return usedHeight + bin.margins().bottom;
}
