/************************************************************************
 **
 **  @file   Engine.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Nesting engine — NFP + simulated annealing via Clipper2.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef ENGINE_H
#define ENGINE_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <clipper2/clipper.h>
#include <clipper2/clipper.minkowski.h>

#include "model/Placement.h"
#include "nesting/NestGeom.h"

class Bin;

/// Nesting engine — places pieces in a strip bin via NFP + simulated annealing.
///
/// Receives a Bin (fixed width, unbounded height) and a list of polygons.
/// The engine does not know about pattern pieces, sizes, or footprints — it
/// only sees polygons.
class Engine
{
public:
    struct Config
    {
        double gap           = 5.0;    ///< Gap between pieces, mm
        double rotationStep  = 2.0;    ///< Rotation precision, degrees
        bool   freeRotation  = true;   ///< If false, pieces keep their input rotation
        int    maxIterations = 1000;   ///< Simulated annealing iterations
        int    plateauLimit  = 200;    ///< Stop if no improvement for this many iterations
        double initialTemp   = 100.0;  ///< Initial temperature
        double coolingRate   = 0.98;   ///< Temperature multiplier per iteration

        /// Artboard grid — the printing frames a tiled plan is cut into. Both
        /// zero means "not a tiled run": no grid, no refinement, and place()
        /// behaves exactly as it did before tiling existed.
        ///
        /// ⚠️ artboardHeight is in the SAME unit as gap — the caller's polygon
        /// unit, which every caller in this repository builds in pixels. The
        /// sibling fields above say "mm" and are passed pixels; these two do not
        /// repeat the lie.
        int    artboardCount  = 0;     ///< Frames side by side across the bin width
        double artboardHeight = 0.0;   ///< Frame height
    };

    // ── NFP cache ──────────────────────────────────────────────────────────

    /// Packed key: pieceA (16b) | rotA (16b) | pieceB (16b) | rotB (16b).
    using NFPCacheKey = std::uint64_t;
    using NFPCache     = std::unordered_map<NFPCacheKey, Clipper2Lib::Paths64>;

    static NFPCacheKey makeNFPCacheKey(int pieceA, int rotA, int pieceB, int rotB) noexcept
    {
        return (std::uint64_t(pieceA) << 48) | (std::uint64_t(rotA) << 32)
             | (std::uint64_t(pieceB) << 16) |  std::uint64_t(rotB);
    }

    explicit Engine(const Config &cfg) noexcept;

    /// Place all pieces in the bin. Returns a placement for each input piece.
    std::vector<Placement> place(const Bin &bin,
                                 const std::vector<Clipper2Lib::Path64> &pieces) const;

    /// Envelope height of a placement result, in the unit of the input polygons
    /// (the bin margin included).
    ///
    /// place() computes this internally to drive the annealing but does not
    /// return it. Every caller that needs the final height — writers, tests,
    /// the host application — shares this one implementation instead of
    /// recomputing the envelope from the placements.
    static double UsedHeight(const std::vector<Placement> &placements,
                             const std::vector<Clipper2Lib::Path64> &pieces,
                             const Bin &bin);

    /// How many printed sheet rows a used height costs, on a frame @p artboardHeight tall.
    ///
    /// The number the host application turns a used height into, written once so that the two
    /// places needing it cannot answer differently: the adjustment pass, which may not cross a row
    /// boundary either way, and the host's own reading of the same formula. Both measure from the
    /// used height WITH the bin margin already in it — which is what UsedHeight returns — and both
    /// lengths are in the unit of the input polygons. artboardHeight is in that same unit, as
    /// Config says.
    ///
    /// Note what this is NOT: it is not the cost the annealing minimises. That is the height, and
    /// it has to be — see the note in placeSequential. This counts the rows the host will print,
    /// which is a different question asked of the same height.
    ///
    /// The tolerance is not decoration. A height landing exactly on a multiple of the frame sits
    /// one rounding away from counting a whole extra sheet, and a printed sheet nobody asked for
    /// is worse than a hair of unused paper. Same figure as the host application's own tolerance.
    static auto SheetRows(double usedHeight, double artboardHeight) -> int;

private:
    // ── Precomputed per-(piece, rotation) geometry ───────────────────────

    /// `RotGeom` et le noyau géométrique (rotations, boîtes, NFP) vivent
    /// désormais dans `nesting/NestGeom.h` : un SECOND moteur est né, le moteur
    /// Tiled, et il a besoin du même noyau. Le recopier aurait été deux vérités
    /// pour une règle.
    using RotGeom = NestGeom::RotGeom;

    // ── Forbidden zones ──────────────────────────────────────────────────

    /// One of the bin's zones, as engine geometry.
    ///
    /// Normalized to its own min corner so that the per-(piece, rotation) NFP
    /// can be cached on it and merely translated to (originX, originY): a zone's
    /// place is then free to change without touching the cache.
    struct ZoneGeom
    {
        Clipper2Lib::Path64 local;      ///< The zone, gap-inflated, normalized

        /// Bounding box of the zone BEFORE inflation — the figure the
        /// "can this zone possibly matter" test reads. Kept exactly as the A0
        /// obstacle had it: the test is a conservative filter, and widening it
        /// would change when a clip is skipped.
        BBox bboxLocal;

        int64_t originX = 0;  ///< Where the local min corner sits in the bin
        int64_t originY = 0;
    };

    /// Marks the third 16-bit field of an obstacle-cache key as a ZONE index
    /// rather than a piece index.
    ///
    /// The key has four fields and the obstacle NFP only ever used two of them,
    /// which is why a single slot could not tell two zones apart — the note in
    /// placeSequential calls that out as the first trap of this change.
    static constexpr int kZoneKeyTag = 0xFFFF;

    /// The bin's forbidden zones as engine geometry, built ONCE per run.
    ///
    /// Each zone comes back as its rectangle at its own place, normalized and
    /// then inflated by the FULL gap. Rebuilding this inside placeSequential —
    /// which runs once per annealing iteration, hundreds of times per run — was
    /// free for one rectangle and is not for K.
    static std::vector<ZoneGeom> buildZoneGeom(const Bin &bin, int64_t gapScaled);

    /// The same zones at their absolute places, joined into one region.
    ///
    /// One geometry, for a pass that only ever asks "may this piece be here".
    static Clipper2Lib::Paths64 absoluteZonePaths(const std::vector<ZoneGeom> &zones);

    // ── Placement ────────────────────────────────────────────────────────

    /// Sequential bottom-left placement for a given order and rotation indices.
    ///
    /// Returns the height used, or a very large value when a piece cannot be placed at all. The
    /// height is what a ROLL is paid by, and a roll is what this engine nests. Counting the
    /// printed SHEETS instead was tried on 17-09-2026 and could not be acted on from here — the
    /// note in the .cpp says why, and is worth reading before trying it again.
    double placeSequential(const Bin &bin,
                           const std::vector<std::vector<RotGeom>> &rotBank,
                           NFPCache &nfpCache,
                           NFPCache &obstacleCache,
                           const std::vector<ZoneGeom> &zones,
                           const std::vector<int> &rotIdx,
                           const std::vector<int> &order,
                           std::vector<Placement> &out,
                           int64_t infY) const;

    // ── Optimisation ─────────────────────────────────────────────────────

    void anneal(const Bin &bin,
                const std::vector<std::vector<RotGeom>> &rotBank,
                NFPCache &nfpCache,
                NFPCache &obstacleCache,
                const std::vector<ZoneGeom> &zones,
                std::vector<Placement> &best,
                int64_t infY) const;

    // ── Adjustment pass ──────────────────────────────────────────────────
    //
    // Defined in EngineRefine.cpp — a private method may be implemented in any
    // translation unit, which keeps this file and Engine.cpp at their size
    // instead of growing one of them by three hundred lines.

    /// Second pass over an already decided placement, inside a frozen envelope.
    ///
    /// ONE aim: keep every footprint border off the artboard cut lines. A border
    /// running along a line is a cut that nicks the piece, so the pass moves a
    /// block until its borders sit a full gap clear of every interior line.
    ///
    /// **It used to carry a second aim — spending whatever room was left pulling
    /// the pieces apart — and that aim is gone (17-09-2026).** Widening the
    /// tightest gap is the opposite of what a tiled plan wants: a plan is paid for
    /// in rows of frames, and pieces spread to the edges of the band fill cells
    /// that would otherwise be dropped, at a printed sheet each. Measured on a
    /// seven-piece pattern, A3 in three columns: the aim took the used height from
    /// 927 mm to 1260 mm on a 1260 mm envelope, and the tightest gap from 5 mm to
    /// 47 mm. The hand that wants room has the canvas to make it in; the machine's
    /// job is to hand it the least paper to work on.
    ///
    /// It runs AFTER the annealing — never inside it — so the search above stays
    /// the search it is, and Rabnest, which has no printing formats, keeps nesting
    /// as it always did.
    ///
    /// Transactional by construction: it works on a copy and commits only on an
    /// improvement, so it can never return something worse than the first pass.
    void refine(const Bin &bin,
                const std::vector<std::vector<RotGeom>> &rotBank,
                std::vector<Placement> &best,
                const std::vector<Clipper2Lib::Path64> &pieces) const;

    Config m_config;
};

#endif // ENGINE_H
