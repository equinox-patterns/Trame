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

constexpr double kPi = 3.14159265358979323846;

// A large but safe height sentinel — tall enough for any practical strip.
// We use the sum of all piece heights (computed at runtime) to avoid Clipper2
// precision issues with near-infinite coordinates.
static int64_t computeInfY(const std::vector<std::vector<Clipper2Lib::Path64>> &rotBank)
{
    int64_t totalH = 0;
    for (const auto &bank : rotBank) {
        if (bank.empty()) continue;
        // Compute bbox inline to avoid dependency order issues
        int64_t yMin = (std::numeric_limits<int64_t>::max)();
        int64_t yMax = (std::numeric_limits<int64_t>::min)();
        for (const auto &p : bank[0]) {
            if (p.y < yMin) yMin = p.y;
            if (p.y > yMax) yMax = p.y;
        }
        totalH += yMax - yMin;
    }
    // Add 50% margin
    totalH = totalH + totalH / 2;
    constexpr int64_t kMaxSafe = int64_t(1) << 50;
    return totalH > 0 ? totalH : kMaxSafe;
}

// A very large double returned when placement fails for a given arrangement.
constexpr double kBadHeight = 1e12;

// ── Bounding box ─────────────────────────────────────────────────────────

struct BBox {
    int64_t xMin, yMin, xMax, yMax;
};

BBox bboxFromPath(const Clipper2Lib::Path64 &path)
{
    BBox b = {(std::numeric_limits<int64_t>::max)(),
              (std::numeric_limits<int64_t>::max)(),
              (std::numeric_limits<int64_t>::min)(),
              (std::numeric_limits<int64_t>::min)()};
    for (const auto &p : path) {
        if (p.x < b.xMin) b.xMin = p.x;
        if (p.y < b.yMin) b.yMin = p.y;
        if (p.x > b.xMax) b.xMax = p.x;
        if (p.y > b.yMax) b.yMax = p.y;
    }
    return b;
}

// ── Geometry helpers ──────────────────────────────────────────────────────

Clipper2Lib::Path64 makeRect(int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    Clipper2Lib::Path64 r;
    r.reserve(4);
    r.push_back({x0, y0});
    r.push_back({x1, y0});
    r.push_back({x1, y1});
    r.push_back({x0, y1});
    return r;
}

Clipper2Lib::Path64 translatePath(const Clipper2Lib::Path64 &path,
                                  int64_t dx, int64_t dy)
{
    Clipper2Lib::Path64 out;
    out.reserve(path.size());
    for (const auto &p : path)
        out.push_back({p.x + dx, p.y + dy});
    return out;
}

/// Translate every path in a Paths64 collection.
Clipper2Lib::Paths64 translatePaths(const Clipper2Lib::Paths64 &paths,
                                    int64_t dx, int64_t dy)
{
    Clipper2Lib::Paths64 out;
    out.reserve(paths.size());
    for (const auto &p : paths)
        out.push_back(translatePath(p, dx, dy));
    return out;
}

/// Find the vertex with minimum Y (tie-break by minimum X) in a set of paths.
/// Ignores empty sub-paths. Returns {0, INT64_MAX} if the region is empty.
Clipper2Lib::Point64 findBottomLeft(const Clipper2Lib::Paths64 &paths)
{
    Clipper2Lib::Point64 best = {int64_t(0), (std::numeric_limits<int64_t>::max)()};
    for (const auto &path : paths) {
        if (path.empty())
            continue;
        for (const auto &p : path) {
            if (p.y < best.y || (p.y == best.y && p.x < best.x))
                best = p;
        }
    }
    return best;
}

/// Conservative check: can the NFP of placed piece A (at px,py) possibly
/// intersect the IFP rectangle [ifpX0,ifpX1]×[ifpY0,∞)?
inline bool nfpOutsideIFP(const BBox &bboxA, const BBox &bboxB,
                          int64_t px, int64_t py,
                          int64_t ifpX0, int64_t ifpX1, int64_t ifpY0)
{
    const int64_t nfpMaxX = px + bboxA.xMax - bboxB.xMin;
    const int64_t nfpMinX = px + bboxA.xMin - bboxB.xMax;
    const int64_t nfpMaxY = py + bboxA.yMax - bboxB.yMin;

    return nfpMaxY <= ifpY0
        || nfpMaxX <= ifpX0
        || nfpMinX >= ifpX1;
}

/// Monotone-chain convex hull (Andrew's algorithm).
/// Returns the convex hull of the input path.
Clipper2Lib::Path64 convexHull(const Clipper2Lib::Path64 &path)
{
    if (path.size() <= 3)
        return path;

    auto pts = path;
    std::sort(pts.begin(), pts.end(),
              [](const Clipper2Lib::Point64 &a, const Clipper2Lib::Point64 &b) {
                  return a.x < b.x || (a.x == b.x && a.y < b.y);
              });

    Clipper2Lib::Path64 hull;
    hull.resize(pts.size() * 2); // upper bound
    int k = 0;

    // Lower hull
    for (const auto &p : pts) {
        while (k >= 2) {
            const auto &a = hull[k - 2];
            const auto &b = hull[k - 1];
            if ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) > 0)
                break;
            --k;
        }
        hull[k++] = p;
    }

    // Upper hull
    const int lowerLen = k;
    for (int i = static_cast<int>(pts.size()) - 2; i >= 0; --i) {
        const auto &p = pts[i];
        while (k > lowerLen) {
            const auto &a = hull[k - 2];
            const auto &b = hull[k - 1];
            if ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x) > 0)
                break;
            --k;
        }
        hull[k++] = p;
    }

    hull.resize(k > 0 ? k - 1 : 0); // remove duplicate last point
    return hull;
}

} // anonymous namespace

// ── Engine: geometry utilities ───────────────────────────────────────────────

Clipper2Lib::Path64 Engine::rotatePath(const Clipper2Lib::Path64 &path,
                                       double degrees)
{
    if (path.empty())
        return {};

    const double rad = degrees * kPi / 180.0;
    const double c   = std::cos(rad);
    const double s   = std::sin(rad);

    Clipper2Lib::Path64 out;
    out.reserve(path.size());

    for (const auto &p : path) {
        out.push_back({
            static_cast<int64_t>(std::round(p.x * c - p.y * s)),
            static_cast<int64_t>(std::round(p.x * s + p.y * c))
        });
    }

    return out;
}

Clipper2Lib::Path64 Engine::negatePath(const Clipper2Lib::Path64 &path)
{
    Clipper2Lib::Path64 out;
    out.reserve(path.size());
    for (const auto &p : path)
        out.push_back({-p.x, -p.y});
    return out;
}

// ── Sequential placement ─────────────────────────────────────────────────────

double Engine::placeSequential(const Bin &bin,
                               const std::vector<std::vector<Clipper2Lib::Path64>> &rotBank,
                               const std::vector<std::vector<Clipper2Lib::Path64>> &convexRotBank,
                               NFPCache &nfpCache,
                               const std::vector<int> &rotIdx,
                               const std::vector<int> &order,
                               std::vector<Placement> &out,
                               int64_t infY) const
{
    const int64_t binW    = static_cast<int64_t>(bin.widthMm() * kScale);
    const int64_t margin  = static_cast<int64_t>(bin.marginMm() * kScale);
    const int64_t halfGap = static_cast<int64_t>(m_config.gap * kScale / 2.0);

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

    // Obstacle (e.g. A4 zone at top-left) — treated as a pre-placed piece
    // whose NFP is subtracted from the IFP of every new piece.
    const bool hasObstacle = bin.obstacleWidthMm() > 0.0 && bin.obstacleHeightMm() > 0.0;
    Clipper2Lib::Path64 obsPath;       // inflated obstacle, local coords
    Clipper2Lib::Path64 obsNegOrig;    // negated (uninflated) for NFP
    BBox obsBBoxLocal;
    int64_t obsPx = 0, obsPy = 0;
    if (hasObstacle) {
        const int64_t obsW = static_cast<int64_t>(bin.obstacleWidthMm() * kScale);
        const int64_t obsH = static_cast<int64_t>(bin.obstacleHeightMm() * kScale);
        Clipper2Lib::Path64 raw = makeRect(margin, margin, margin + obsW, margin + obsH);
        // Normalize
        const BBox rawBB = bboxFromPath(raw);
        Clipper2Lib::Path64 local = translatePath(raw, -rawBB.xMin, -rawBB.yMin);
        obsBBoxLocal = bboxFromPath(local);
        obsPx = margin;
        obsPy = margin;
        // Inflate for gap (same as placed pieces)
        if (halfGap > 0) {
            auto inflated = Clipper2Lib::InflatePaths({local}, halfGap,
                Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon);
            obsPath = (!inflated.empty()) ? inflated[0] : local;
        } else {
            obsPath = local;
        }
        obsNegOrig = negatePath(local); // NFP = MinkowskiSum(obsPath, negB) but use uninflated origin
    }

    int64_t maxYScaled = hasObstacle
        ? (margin + static_cast<int64_t>(bin.obstacleHeightMm() * kScale))
        : 0;

    for (int orderIdx = 0; orderIdx < static_cast<int>(order.size()); ++orderIdx) {
        const int pieceIdx = order[orderIdx];
        const int rIdx     = rotIdx[pieceIdx];
        const Clipper2Lib::Path64 &B    = rotBank[pieceIdx][rIdx];
        const BBox bboxB = bboxFromPath(B);

        int64_t ifpX0 = margin - bboxB.xMin;
        int64_t ifpX1 = binW - margin - bboxB.xMax;
        int64_t ifpY0 = margin - bboxB.yMin;

        if (ifpX1 <= ifpX0) {
            out[pieceIdx].pieceIndex = pieceIdx;
            out[pieceIdx].rotation   = rIdx * m_config.rotationStep;
            return kBadHeight;
        }

        // Build feasible region = IFP rectangle minus NFPs of placed pieces +
        // obstacle.
        Clipper2Lib::Paths64 feasible;
        {
            Clipper2Lib::Clipper64 clipper;
            clipper.AddSubject({makeRect(ifpX0, ifpY0, ifpX1, infY)});

            for (const auto &p : placed) {
                const BBox &bboxA = bboxFromPath(rotBank[p.pieceIdx][p.rotIdx]);

                if (nfpOutsideIFP(bboxA, bboxB, p.px, p.py, ifpX0, ifpX1, ifpY0))
                    continue;

                const NFPCacheKey key = makeNFPCacheKey(p.pieceIdx, p.rotIdx,
                                                        pieceIdx, rIdx);
                const Clipper2Lib::Paths64 *nfp = nullptr;
                auto it = nfpCache.find(key);
                if (it != nfpCache.end()) {
                    nfp = &it->second;
                } else {
                    // Use full polygon for accurate NFP (concavities can be exploited).
                    const Clipper2Lib::Path64 &Areal = rotBank[p.pieceIdx][p.rotIdx];
                    Clipper2Lib::Paths64 Ainflated;
                    if (halfGap > 0) {
                        Ainflated = Clipper2Lib::InflatePaths({Areal}, halfGap,
                            Clipper2Lib::JoinType::Miter,
                            Clipper2Lib::EndType::Polygon);
                    }
                    const Clipper2Lib::Path64 &Aref = halfGap > 0 && !Ainflated.empty()
                        ? Ainflated[0] : Areal;
                    Clipper2Lib::Path64 negBreal = negatePath(
                        rotBank[pieceIdx][rIdx]);
                    Clipper2Lib::Paths64 nfpResult =
                        Clipper2Lib::MinkowskiSum(Aref, negBreal, true);

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

            // Subtract obstacle NFP (like a pre-placed piece at margin,margin).
            if (hasObstacle) {
                const int64_t obsNfpMaxX = obsPx + obsBBoxLocal.xMax - bboxB.xMin;
                const int64_t obsNfpMinX = obsPx + obsBBoxLocal.xMin - bboxB.xMax;
                const int64_t obsNfpMaxY = obsPy + obsBBoxLocal.yMax - bboxB.yMin;
                if (!(obsNfpMaxY <= ifpY0 || obsNfpMaxX <= ifpX0 || obsNfpMinX >= ifpX1)) {
                    Clipper2Lib::Path64 negB = negatePath(B);
                    Clipper2Lib::Paths64 nfpObs =
                        Clipper2Lib::MinkowskiSum(obsPath, negB, true);
                    for (auto &np : nfpObs) {
                        if (Clipper2Lib::Area(np) < 0)
                            std::reverse(np.begin(), np.end());
                    }
                    clipper.AddClip(translatePaths(nfpObs, obsPx, obsPy));
                }
            }

            clipper.Execute(Clipper2Lib::ClipType::Difference,
                           Clipper2Lib::FillRule::NonZero, feasible);
        }

        const Clipper2Lib::Point64 pos = findBottomLeft(feasible);
        if (pos.y >= infY) {
            out[pieceIdx].pieceIndex = pieceIdx;
            out[pieceIdx].rotation   = rIdx * m_config.rotationStep;
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

    return maxYScaled / kScale;
}

// ── Simulated annealing ──────────────────────────────────────────────────────

void Engine::anneal(const Bin &bin,
                    const std::vector<std::vector<Clipper2Lib::Path64>> &rotBank,
                    const std::vector<std::vector<Clipper2Lib::Path64>> &convexRotBank,
                    NFPCache &nfpCache,
                    std::vector<Placement> &best,
                    int64_t infY) const
{
    const int N = static_cast<int>(rotBank.size());
    if (N == 0)
        return;

    std::mt19937 rng(42);

    // Initial state: order by area descending, rotation 0.
    std::vector<int> order(N);
    std::vector<int> rotIdx(N, 0);
    {
        std::vector<std::pair<double, int>> areaIdx;
        areaIdx.reserve(N);
        for (int i = 0; i < N; ++i)
            areaIdx.emplace_back(Clipper2Lib::Area(rotBank[i][0]), i);
        std::sort(areaIdx.begin(), areaIdx.end(),
                  [](const auto &a, const auto &b) { return a.first > b.first; });
        for (int i = 0; i < N; ++i)
            order[i] = areaIdx[i].second;
    }

    std::vector<Placement> current;
    double bestHeight = placeSequential(bin, rotBank, convexRotBank, nfpCache,
                                        rotIdx, order, current, infY);
    best = current;

    double T = m_config.initialTemp;

    std::uniform_real_distribution<double> u01(0.0, 1.0);
    std::uniform_int_distribution<int> randPiece(0, N - 1);
    std::uniform_int_distribution<int> randSwapPos(0, N - 1);

    int plateauCount = 0;

    for (int iter = 0; iter < m_config.maxIterations && T > 0.01; ++iter) {
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
        double newHeight = placeSequential(bin, rotBank, convexRotBank, nfpCache,
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
}

// ── Public API ────────────────────────────────────────────────────────────────

Engine::Engine(const Config &cfg) noexcept
    : m_config(cfg)
{
}

std::vector<Placement> Engine::place(const Bin &bin,
                                     const std::vector<Clipper2Lib::Path64> &pieces) const
{
    const int N = static_cast<int>(pieces.size());
    if (N == 0)
        return {};

    assert(m_config.gap >= 0.0 && "gap must be non-negative");
    assert(m_config.rotationStep > 0.0 && "rotationStep must be positive");
    assert(m_config.coolingRate > 0.0 && m_config.coolingRate < 1.0
           && "coolingRate must be in (0, 1)");

    // Normalize all contours to local coordinates.
    std::vector<Clipper2Lib::Path64> local(N);
    for (int i = 0; i < N; ++i) {
        const BBox bb = bboxFromPath(pieces[i]);
        local[i] = translatePath(pieces[i], -bb.xMin, -bb.yMin);
    }

    const int numRots = m_config.freeRotation
        ? static_cast<int>(360.0 / m_config.rotationStep)
        : 1;

    // Helper: ensure a path is CCW (positive signed area) for Clipper2 conventions.
    // Clipper2 MinkowskiSum expects CCW for the first path.
    auto ensureCCW = [](Clipper2Lib::Path64 &p) {
        if (Clipper2Lib::Area(p) < 0)
            std::reverse(p.begin(), p.end());
    };

    // Rotation banks:
    //   rotBank — full-precision paths for placement
    //   convexRotBank — convex hull of each rotated path, used for MinkowskiSum.
    //     The convex-hull NFP is a conservative superset of the true NFP,
    //     so pieces never overlap, but MinkowskiSum is O(N+M) instead of O(NM).
    std::vector<std::vector<Clipper2Lib::Path64>> rotBank(N);
    std::vector<std::vector<Clipper2Lib::Path64>> convexRotBank(N);
    for (int i = 0; i < N; ++i) {
        rotBank[i].reserve(numRots);
        convexRotBank[i].reserve(numRots);
        for (int r = 0; r < numRots; ++r) {
            Clipper2Lib::Path64 rotated = rotatePath(local[i], r * m_config.rotationStep);
            ensureCCW(rotated);
            auto ch = convexHull(rotated);
            ensureCCW(ch);
            convexRotBank[i].push_back(std::move(ch));
            rotBank[i].push_back(std::move(rotated));
        }
    }

    NFPCache nfpCache;
    nfpCache.reserve(std::size_t(N) * numRots * 4);

    const int64_t infY = computeInfY(rotBank);

    std::vector<Placement> best;
    anneal(bin, rotBank, convexRotBank, nfpCache, best, infY);
    return best;
}
