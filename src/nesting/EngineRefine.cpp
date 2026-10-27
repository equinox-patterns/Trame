/************************************************************************
 **
 **  @file   EngineRefine.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   15/09/2026
 **
 **  @brief  Second pass over a decided placement — the tiled adjustment.
 **
 **          The nesting engine minimises height, and nothing else. On a tiled
 **          plan that leaves two things on the table: the dead band the last
 **          artboard row keeps for free, and the footprint borders that happen
 **          to run along a cut line.
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

#include "nesting/Engine.h"

#include "nesting/Bin.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{

/// How many rounds over every piece before giving up. The loop also stops as
/// soon as a whole round changes nothing, which is the usual ending.
constexpr int kMaxRounds = 8;

/// Ceiling on the non-fit polygons this pass may compute. Reusing what the
/// annealing already paid for is free; computing a missing one is not, and an
/// unbounded search would answer a printing annoyance with a frozen window.
constexpr int kMaxNfpComputations = 4000;

using Clipper2Lib::Path64;
using Clipper2Lib::Paths64;
using Clipper2Lib::Point64;

/// L'échelle du point fixe, désormais partagée par les deux moteurs — elle
/// vivait sur `Engine` avant que le noyau géométrique parte dans NestGeom.h.
using NestGeom::kScale;

BBox BBoxOf(const Path64 &path)
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

/// Point64's two-argument constructor is a template: both arguments must have
/// the SAME type or deduction fails and the call does not compile. A bare `0`
/// beside an int64_t is enough to break it, which is why this exists.
Point64 Delta(int64_t dx, int64_t dy)
{
    return Point64(dx, dy);
}

/// How close a footprint border runs to a cut line — the number that matters.
///
/// Crossing a line is NOT the defect: a piece larger than an artboard is cut,
/// that is what tiling is for. The defect is a BORDER that runs along the line,
/// either by stopping just short of it or by poking only a hair past it. So a
/// crossing piece is measured by its **thinnest protrusion**: a piece cut
/// through the middle has two large ones and is left alone, a piece that pokes
/// one millimetre past counts.
double BorderToLine(const Path64 &c, int64_t coord, bool vertical)
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

    const bool crosses = (maxLeft > 0.0 && maxRight > 0.0);
    return crosses ? std::min(maxLeft, maxRight) : minAbs;
}

} // namespace

//---------------------------------------------------------------------------------------------------------------------
bool Engine::refine(const Bin &bin,
                    const std::vector<std::vector<RotGeom>> &rotBank,
                    std::vector<Placement> &best,
                    const std::vector<Clipper2Lib::Path64> &pieces) const
{
    const std::size_t n = best.size();

    // Nothing to adjust with a single piece, and nothing trustworthy to adjust
    // if the first pass did not place them all. UsedHeight would happily fold a
    // phantom at (0, 0) into the envelope, and the pass would then optimise a
    // plan that does not exist.
    if (n < 2 || pieces.size() != n)
    {
        return false;
    }
    for (const Placement &p : best)
    {
        if (p.rotation < 0.0)
        {
            return false;
        }
    }

    const int64_t binW    = static_cast<int64_t>(bin.widthMm() * kScale);
    const int64_t marginL = static_cast<int64_t>(bin.margins().left * kScale);
    const int64_t marginR = static_cast<int64_t>(bin.margins().right * kScale);
    const int64_t marginT = static_cast<int64_t>(bin.margins().top * kScale);
    const int64_t marginB = static_cast<int64_t>(bin.margins().bottom * kScale);
    const int64_t gapR    = static_cast<int64_t>(m_config.gap * kScale);
    const int64_t cellW   = binW / m_config.artboardCount;
    const int64_t cellH   = static_cast<int64_t>(m_config.artboardHeight * kScale);

    if (cellW <= 0 || cellH <= 0)
    {
        return false;
    }

    // What the first pass kept clear, this pass must keep clear too — every
    // forbidden zone, at its absolute place, joined into one region. The same
    // builder the first pass uses, so the two passes cannot disagree about
    // where a forbidden page is (see NestZone::Build).
    //
    // This used to be impossible: the zone geometry lived inside
    // placeSequential and was not reachable here, so the pass refused to run at
    // all as soon as the bin carried an obstacle. The refusal is gone with the
    // reachability — and it was the obstacle-plus-grid combination, the one a
    // tiled plan with forbidden pages needs, that it made unreachable.
    const Clipper2Lib::Paths64 forbidden = NestZone::Absolute(NestZone::Build(bin.zones(), gapR));

    // The envelope comes from the very number the host application stores and
    // turns into a page count — never from a local recomputation, which would
    // drift. The rows are counted by the one function that counts them, which the
    // annealing now aims at as well.
    const int     rows     = SheetRows(UsedHeight(best, pieces, bin), m_config.artboardHeight);
    const int64_t envelope = static_cast<int64_t>(rows) * cellH;

    // One engine unit past the tolerance, so that "clear" stays clear once the
    // rounded integer coordinates are converted back. Aiming exactly at the gap
    // leaves the border a fraction of a nanometre inside it, which the measure
    // then reports as still grazing — true, and useless.
    const int64_t clear = gapR + 1;

    // The upper end of the envelope is knife-edge: Ostara recomputes the row
    // count as ceil(height / cellHeight - 1e-9), so a height landing exactly on
    // rows*cellHeight sits one rounding away from counting a whole extra row —
    // a printed sheet nobody asked for. Every candidate aims at the envelope
    // MINUS this guard, never at the brim, so the pass can still use the dead
    // band without ever touching the edge.
    const int64_t guard = cellH / 1000;

    std::vector<int64_t> vLines;
    std::vector<int64_t> hLines;
    for (int k = 1; k < m_config.artboardCount; ++k) { vLines.push_back(cellW * k); }
    for (int j = 1; j < rows; ++j)                   { hLines.push_back(cellH * j); }

    if (vLines.empty() && hLines.empty())
    {
        return false;
    }

    std::vector<int>     rIdx(n, 0);
    std::vector<Point64> pos(n);

    for (std::size_t i = 0; i < n; ++i)
    {
        if (IsAbandoned())
            return true;

        // The angle came out of a bank, so it is IN a bank — found by angle rather than divided by
        // the step, a constrained piece holding angles that are not multiples of anything.
        const int r = NestGeom::IndexOfAngle(rotBank[i], best[i].rotation);
        if (r < 0)
        {
            return false;
        }

        rIdx[i] = r;
        pos[i]  = {static_cast<int64_t>(std::llround(best[i].x * kScale)),
                   static_cast<int64_t>(std::llround(best[i].y * kScale))};
    }

    // ── Geometry at a hypothetical set of positions ───────────────────────

    auto contourAt = [&](std::size_t i, const Point64 &p) -> Path64
    {
        return Clipper2Lib::TranslatePath(rotBank[i][rIdx[i]].path, p.x, p.y);
    };

    // ── The one test that decides whether a position is allowed ───────────
    //
    // It is NOT a distance. Two outlines can be far apart while one of them
    // sits entirely INSIDE the other — the distance between outlines is then
    // perfectly healthy, and the pieces are on top of each other. That is how a
    // cuff was first dropped inside its own sleeve: everything the earlier
    // version measured was fine, and the result was two pieces in one place.
    //
    // So the question asked is the right one: does the reference piece, grown by
    // the full gap, meet the other piece at all. `aref` is exactly that grown
    // outline — the engine computed it for the first pass, so this costs no
    // inflation here. Containment is caught, since a piece inside another still
    // intersects it.
    auto tooClose = [&](std::size_t a, const Point64 &pa,
                        std::size_t b, const Point64 &pb) -> bool
    {
        const Paths64 grown = Clipper2Lib::TranslatePaths({rotBank[a][rIdx[a]].aref}, pa.x, pa.y);
        const Paths64 other = Clipper2Lib::TranslatePaths({rotBank[b][rIdx[b]].path}, pb.x, pb.y);

        return !Clipper2Lib::Intersect(grown, other, Clipper2Lib::FillRule::NonZero).empty();
    };

    // Borders running along a cut line, summed over every piece. Cheap: a walk
    // over the vertices, once per line.
    auto grazingTotal = [&](const std::vector<Point64> &p) -> int
    {
        int g = 0;

        for (std::size_t i = 0; i < n; ++i)
        {
            const Path64 c = contourAt(i, p[i]);

            for (const int64_t line : vLines) { g += (BorderToLine(c, line, true)  < static_cast<double>(gapR)) ? 1 : 0; }
            for (const int64_t line : hLines) { g += (BorderToLine(c, line, false) < static_cast<double>(gapR)) ? 1 : 0; }
        }

        return g;
    };

    // ── A move is a BLOCK, not a piece ────────────────────────────────────
    //
    // Moving one piece alone almost never helps: the pieces sit at exactly the
    // gap, so pushing one into its neighbour is a collision, not a move. What a
    // hand does — and what Sophie described — is push the neighbour along. A
    // block translated as a whole keeps every gap inside it, so only the leading
    // edge has anything new to answer for.
    //
    // Returns the resulting positions, and an empty vector when the move is
    // refused. Nothing is ever written before it is known to hold.

    auto pushBlock = [&](std::size_t i, const Point64 &delta) -> std::vector<Point64>
    {
        std::vector<Point64> out = pos;
        std::vector<bool>    taken(n, false);

        std::vector<std::size_t> queue{i};
        std::vector<std::size_t> block{i};
        taken[i] = true;

        while (!queue.empty())
        {
            const std::size_t k = queue.back();
            queue.pop_back();

            out[k] = {pos[k].x + delta.x, pos[k].y + delta.y};

            for (std::size_t j = 0; j < n; ++j)
            {
                if (taken[j])
                {
                    continue;
                }

                if (tooClose(k, out[k], j, pos[j]))
                {
                    taken[j] = true;
                    block.push_back(j);
                    queue.push_back(j);
                }
            }
        }

        // Everything the block touched must stay in the bin, and must not have
        // been driven into a piece left behind.
        for (const std::size_t k : block)
        {
            const BBox &g = rotBank[k][rIdx[k]].bbox;

            if (out[k].x + g.xMin < marginL || out[k].x + g.xMax > binW - marginR)
            {
                return {};
            }
            if (out[k].y + g.yMin < marginT)
            {
                return {};
            }
        }

        // …nor into a page this run must not print on.
        //
        // The question is asked of the CONTOUR, not of the bounding box. A
        // rotated piece's box is far larger than the piece, and refusing on the
        // box would deny a place the first pass itself allowed — the mistake
        // already paid for once, on this canvas, when the holder clamped on its
        // rectangle and pushed back a piece that was perfectly inside the plan.
        //
        // This is the only place the test is needed: rIdx[i] is set BEFORE the
        // deltas are built, and this lambda reads rotBank[k][rIdx[k]] — so a
        // rotation candidate goes through here exactly like a translation.
        if (!forbidden.empty())
        {
            for (const std::size_t k : block)
            {
                if (!Clipper2Lib::Intersect({contourAt(k, out[k])}, forbidden,
                                            Clipper2Lib::FillRule::NonZero).empty())
                {
                    return {};
                }
            }
        }

        for (const std::size_t k : block)
        {
            for (std::size_t j = 0; j < n; ++j)
            {
                if (taken[j])
                {
                    continue;
                }

                if (tooClose(k, out[k], j, pos[j]))
                {
                    return {};
                }
            }
        }

        // Height. Ostara recomputes the row count from whatever height this pass
        // leaves behind, so crossing a row boundary — either way — adds or drops
        // a printed sheet. The envelope is an interval, not a ceiling.
        int64_t newUsed = 0;
        for (std::size_t j = 0; j < n; ++j)
        {
            newUsed = std::max(newUsed, out[j].y + rotBank[j][rIdx[j]].bbox.yMax);
        }
        // The BOTTOM margin, the same one UsedHeight adds: the host recomputes the row count from
        // the height this pass leaves behind, and a height measured with a different margin than
        // the one it will be reported with is a row counted twice or lost.
        newUsed += marginB;

        if (newUsed > (envelope - guard) || newUsed <= (envelope - cellH))
        {
            return {};
        }

        return out;
    };

    // ── Rounds ────────────────────────────────────────────────────────────

    // The arrangement the first pass handed over, kept whole. The commits below
    // write straight into `best`, so the promise this pass makes — never returning
    // anything worse than what it was given — can only be kept by being able to
    // hand this back. See the check at the end.
    const std::vector<Placement> start = best;

    for (int round = 0; round < kMaxRounds; ++round)
    {
        // The pass is a second search of its own, over the same pieces. Asked once a round, which
        // is where its time goes.
        if (IsAbandoned())
            return true;

        bool moved = false;

        for (std::size_t i = 0; i < n; ++i)
        {
            const int g0 = grazingTotal(pos);

            const int savedR  = rIdx[i];
            const double baseAngle = rotBank[i][static_cast<std::size_t>(savedR)].angle;

            int                  bestG   = g0;
            std::vector<Point64> bestPos = pos;
            int                  bestR   = savedR;
            bool                 found   = false;

            // ── Les orientations essayées ──────────────────────────────────
            //
            // La banque porte DÉJÀ toutes les rotations de chaque pièce — le
            // recuit les a fait construire — donc essayer un quart de tour ne
            // coûte aucune géométrie, seulement les tests. Les pas intermédiaires
            // n'apporteraient rien ici : ce que la rotation change, c'est
            // l'appartenance à un cadre, donc l'axe. Une pièce posée de travers
            // dans une grille droite ne gagne rien à pencher un peu plus.
            for (const double offset : {0.0, 90.0, -90.0, 180.0})
            {
                // Le quart de tour est demandé PAR L'ANGLE, jamais par l'index : une pièce dont la
                // direction est connue ne tient que les angles que son droit-fil autorise, et un
                // angle qu'elle n'a pas est SAUTÉ — l'arrondir au plus proche la coucherait en
                // travers du droit-fil, ce que rien à l'écran ne dirait.
                const int r = NestGeom::IndexOfAngle(rotBank[i], baseAngle + offset);
                if (r < 0)
                {
                    continue;
                }

                rIdx[i] = r;
                const BBox &g = rotBank[i][r].bbox;

                // Quatre façons de passer une ligne de coupe, par ligne : dégagé
                // d'un côté, dégagé de l'autre, ou à cheval avec le bord le plus
                // proche exactement au jeu. Les deux dernières sont ce qui répare
                // vraiment une pièce qui dépasse d'un cheveu — les interdire
                // était le défaut entier de la première version.
                std::vector<Point64> deltas;
                deltas.reserve(4 * (vLines.size() + hLines.size()) + 2);

                for (const int64_t line : vLines)
                {
                    deltas.push_back(Delta((line - clear - g.xMax) - pos[i].x, 0));
                    deltas.push_back(Delta((line + clear - g.xMin) - pos[i].x, 0));
                    deltas.push_back(Delta((line + clear - g.xMax) - pos[i].x, 0));
                    deltas.push_back(Delta((line - clear - g.xMin) - pos[i].x, 0));
                }
                for (const int64_t line : hLines)
                {
                    deltas.push_back(Delta(0, (line - clear - g.yMax) - pos[i].y));
                    deltas.push_back(Delta(0, (line + clear - g.yMin) - pos[i].y));
                    deltas.push_back(Delta(0, (line + clear - g.yMax) - pos[i].y));
                    deltas.push_back(Delta(0, (line - clear - g.yMin) - pos[i].y));
                }

                // ── And two small nudges, for when the room is tight ───────
                //
                // There used to be four more here — as far right, as far left, as
                // high and as low as the bin and the envelope allowed. They served
                // the second aim this pass no longer has: widening the tightest
                // gap. A plan is paid for in rows of frames, and a block carried
                // to the edge of the band fills cells that would otherwise be
                // dropped, at a printed sheet each. Gone, and the small moves
                // below are all that is left — they suffice when the room is
                // tight, which is the only case this pass now has to answer.
                deltas.push_back(Delta(0, gapR));
                deltas.push_back(Delta(gapR, 0));

                for (const Point64 &delta : deltas)
                {
                    if (delta.x == 0 && delta.y == 0)
                    {
                        continue;
                    }

                    const std::vector<Point64> candidate = pushBlock(i, delta);
                    if (candidate.empty())
                    {
                        continue;
                    }

                    const int g1 = grazingTotal(candidate);

                    // A cut line freed is the ONLY reason to move anything. A
                    // candidate that leaves the same borders grazing is passed
                    // over, however far it would have carried the block: pulling
                    // the pieces apart used to win this tie, and that is exactly
                    // what filled the plan's empty cells and cost printed sheets.
                    if (g1 < bestG)
                    {
                        bestG   = g1;
                        bestPos = candidate;
                        bestR   = r;
                        found   = true;
                    }
                }
            }

            if (!found)
            {
                rIdx[i] = savedR; // rien retenu : la pièce garde son orientation
                continue;
            }

            rIdx[i] = bestR;
            best[i].rotation = rotBank[i][static_cast<std::size_t>(bestR)].angle;

            for (std::size_t j = 0; j < n; ++j)
            {
                pos[j]    = bestPos[j];
                best[j].x = static_cast<double>(pos[j].x) / kScale;
                best[j].y = static_cast<double>(pos[j].y) / kScale;
            }
            moved = true;
        }

        if (!moved)
        {
            break;
        }
    }

    // ── The promise, checked ──────────────────────────────────────────────
    //
    // "Transactional by construction: it works on a copy and commits only on an
    // improvement, so it can never return something worse than the first pass."
    // That was written and never verified — and on 17-09-2026 a plan came out of
    // here with two pairs of pieces lying on one another. A plan whose pieces
    // overlap is not a hard-to-print plan, it is not a plan: the drawing puts one
    // piece on top of another and the paper shows one of them.
    //
    // So it is verified, once, on the arrangement about to be returned. Two
    // contours that INTERSECT are two pieces in one place. The gap is deliberately
    // not asked about: a pair sitting exactly at the gap is what a healthy plan
    // looks like, and testing for it here would refuse perfectly good layouts.
    //
    // When the check fails the pass hands back what it was given, untouched — the
    // promise, kept by construction rather than by hope. It says nothing about WHY
    // it failed: the cause is still open, and a guard that concealed it would be
    // worse than none.
    for (std::size_t a = 0; a < n; ++a)
    {
        if (IsAbandoned())
            return true;

        for (std::size_t b = a + 1; b < n; ++b)
        {
            const Paths64 hit = Clipper2Lib::Intersect({contourAt(a, pos[a])},
                                                       {contourAt(b, pos[b])},
                                                       Clipper2Lib::FillRule::NonZero);
            if (!hit.empty())
            {
                best = start;
                return false;
            }
        }
    }

    // The same promise, asked about the forbidden zones — a piece sitting on a
    // page this run must not print on is the same class of wrong as two pieces
    // in one place, and the pass is the last place it can be caught.
    if (!forbidden.empty())
    {
        for (std::size_t a = 0; a < n; ++a)
        {
            if (!Clipper2Lib::Intersect({contourAt(a, pos[a])}, forbidden,
                                        Clipper2Lib::FillRule::NonZero).empty())
            {
                best = start;
                return false;
            }
        }
    }

    // The pass ran its course and committed. `false`, because this return value says ONE thing
    // and it is not "did it work": Engine::place reads it as "the run was given up" and throws
    // the whole plan away without a word. A `true` here declared every SUCCESSFUL adjustment an
    // abandonment — which is every tiled plan the pass improved, and only those: a rolled-back
    // pass (the two returns above) kept its plan. Measured on the framed formats, whose geometry
    // is the one the pass succeeds on: 95 seconds of annealing, then nothing on screen.
    return false;
}
