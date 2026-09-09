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

class Bin;

/// Axis-aligned bounding box in Clipper integer coordinates.
struct BBox
{
    int64_t xMin, yMin, xMax, yMax;
};

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

private:
    // ── Precomputed per-(piece, rotation) geometry ───────────────────────

    /// Everything that only depends on (piece, rotation) is computed once in
    /// place() and reused across every annealing iteration. Previously these
    /// were recomputed identically on each NFP cache miss / each placement.
    struct RotGeom
    {
        Clipper2Lib::Path64 path;  ///< full rotated path, CCW
        Clipper2Lib::Path64 neg;   ///< negatePath(path)
        Clipper2Lib::Path64 aref;  ///< gap-inflated reference (== path when no inflation)
        BBox                 bbox;
    };

    // ── Geometry utilities (Clipper2 integer coordinates) ────────────────

    static constexpr double kScale = 1e6;

    static Clipper2Lib::Path64 rotatePath(const Clipper2Lib::Path64 &path,
                                          double degrees);
    static Clipper2Lib::Path64 negatePath(const Clipper2Lib::Path64 &path);

    // ── Placement ────────────────────────────────────────────────────────

    /// Sequential bottom-left placement for a given order and rotation indices.
    /// Returns the total height used in mm, or a very large value on failure.
    double placeSequential(const Bin &bin,
                           const std::vector<std::vector<RotGeom>> &rotBank,
                           NFPCache &nfpCache,
                           NFPCache &obstacleCache,
                           const std::vector<int> &rotIdx,
                           const std::vector<int> &order,
                           std::vector<Placement> &out,
                           int64_t infY) const;

    // ── Optimisation ─────────────────────────────────────────────────────

    void anneal(const Bin &bin,
                const std::vector<std::vector<RotGeom>> &rotBank,
                NFPCache &nfpCache,
                NFPCache &obstacleCache,
                std::vector<Placement> &best,
                int64_t infY) const;

    Config m_config;
};

#endif // ENGINE_H
