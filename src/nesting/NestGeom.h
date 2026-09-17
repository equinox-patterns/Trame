/************************************************************************
 **
 **  @file   NestGeom.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   17/09/2026
 **
 **  @brief  Géométrie partagée par les moteurs de nidification.
 **
 **          Extraite de Engine.cpp quand un SECOND moteur est né (le moteur
 **          Tiled, qui paie en pages et non en hauteur). Les deux moteurs ont
 **          besoin du même noyau — rotations, boîtes englobantes, NFP, région
 **          faisable — et le recopier aurait été deux vérités pour une règle.
 **
 **          Tout ici est INLINE et sans état : c'est un ensemble de fonctions
 **          pures, pas un objet. Aucune n'alloue de ressource, aucune ne
 **          connaît ni Bin ni Engine.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef NESTGEOM_H
#define NESTGEOM_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <clipper2/clipper.h>
#include <clipper2/clipper.minkowski.h>

/// Boîte englobante en coordonnées entières Clipper.
///
/// ⚠️ Type GLOBAL, et il l'était déjà dans `Engine.h` avant cette extraction :
/// les consommateurs de Trame l'incluent par `nesting/Engine.h` et le nomment
/// sans préfixe. Le ranger dans le namespace casserait leur compilation pour un
/// gain nul — l'extraction déplace la géométrie, pas l'API.
struct BBox
{
    int64_t xMin, yMin, xMax, yMax;
};

namespace NestGeom {

/// L'échelle du point fixe du moteur : les polygones circulent multipliés par
/// 1e6 en entiers Clipper2, les placements ressortent divisés.
///
/// ⚠️ L'unité réelle est celle de l'APPELANT — des pixels 96 dpi pour tous les
/// appelants de ce dépôt. Les accesseurs de `Bin` s'appellent `…Mm` et mentent ;
/// ce chiffre ne dit rien de l'unité.
inline constexpr double kScale = 1e6;

inline BBox bboxFromPath(const Clipper2Lib::Path64 &path)
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

inline Clipper2Lib::Path64 makeRect(int64_t x0, int64_t y0, int64_t x1, int64_t y1)
{
    Clipper2Lib::Path64 r;
    r.reserve(4);
    r.push_back({x0, y0});
    r.push_back({x1, y0});
    r.push_back({x1, y1});
    r.push_back({x0, y1});
    return r;
}

inline Clipper2Lib::Path64 translatePath(const Clipper2Lib::Path64 &path,
                                         int64_t dx, int64_t dy)
{
    Clipper2Lib::Path64 out;
    out.reserve(path.size());
    for (const auto &p : path)
        out.push_back({p.x + dx, p.y + dy});
    return out;
}

/// Translate every path in a Paths64 collection.
inline Clipper2Lib::Paths64 translatePaths(const Clipper2Lib::Paths64 &paths,
                                           int64_t dx, int64_t dy)
{
    Clipper2Lib::Paths64 out;
    out.reserve(paths.size());
    for (const auto &p : paths)
        out.push_back(translatePath(p, dx, dy));
    return out;
}

/// Rotate about the origin, rounded vertex by vertex.
///
/// ⚠️ L'arrondi SOMMET PAR SOMMET est la convention du moteur, et trois
/// reconstructions la répliquent (gapbench::ToAbsolute, Engine::UsedHeight,
/// NestedSvgWriter) : la changer ferait diverger ce qui est mesuré de ce qui
/// est dessiné.
inline Clipper2Lib::Path64 rotatePath(const Clipper2Lib::Path64 &path,
                                      double degrees)
{
    if (path.empty())
        return {};

    const double kPi = 3.14159265358979323846;
    const double rad = degrees * kPi / 180.0;
    const double c   = std::cos(rad);
    const double s   = std::sin(rad);

    Clipper2Lib::Path64 out;
    out.reserve(path.size());

    for (const auto &p : path) {
        // ⚠️ Les casts sont ÉCRITS, pas implicites. Ce fichier est un en-tête :
        // il est compilé avec les drapeaux de SES CONSOMMATEURS, et Ostara
        // tourne sous `-Werror -Wsign-conversion -Wimplicit-int-float-conversion`
        // — bien plus stricts que ceux de Trame. Du code déplacé d'un .cpp vers
        // un .h change donc de juge, et c'est exactement le piège déjà consigné
        // pour les en-têtes de Clipper2 (Nesting.md §6).
        out.push_back({
            static_cast<int64_t>(std::round(static_cast<double>(p.x) * c
                                            - static_cast<double>(p.y) * s)),
            static_cast<int64_t>(std::round(static_cast<double>(p.x) * s
                                            + static_cast<double>(p.y) * c))
        });
    }

    return out;
}

inline Clipper2Lib::Path64 negatePath(const Clipper2Lib::Path64 &path)
{
    Clipper2Lib::Path64 out;
    out.reserve(path.size());
    for (const auto &p : path)
        out.push_back({-p.x, -p.y});
    return out;
}

/// Clipper2 MinkowskiSum expects CCW for the first path.
inline void ensureCCW(Clipper2Lib::Path64 &p)
{
    if (Clipper2Lib::Area(p) < 0)
        std::reverse(p.begin(), p.end());
}

/// Find the vertex with minimum Y (tie-break by minimum X) in a set of paths.
/// Ignores empty sub-paths. Returns {0, INT64_MAX} if the region is empty.
inline Clipper2Lib::Point64 findBottomLeft(const Clipper2Lib::Paths64 &paths)
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

/// Everything that only depends on (piece, rotation), computed once.
struct RotGeom
{
    Clipper2Lib::Path64 path;  ///< full rotated path, CCW
    Clipper2Lib::Path64 neg;   ///< negatePath(path)
    Clipper2Lib::Path64 aref;  ///< gap-inflated reference (== path when no inflation)
    BBox                 bbox;
};

/// One RotGeom per (piece, rotation), from contours already normalized to their
/// own min corner.
///
/// The gap is the CLEAR DISTANCE held between two pieces, so the reference is
/// inflated by the FULL gap — not by half of it. The NFP is
/// MinkowskiSum(A_inflated, -B): it forbids every position where the inflated A
/// would meet B. Inflating by gap/2 therefore only ever guaranteed gap/2 on the
/// floor, which is what this code did until the clearances were actually
/// measured (Trame/tests/gapbench.cpp) — a nesting asked for 5 mm was
/// delivering 2.5 mm. The two-sided scheme (A and B each grown by gap/2)
/// describes the very same forbidden region, since
/// MinkowskiSum(A⊕g/2, -(B⊕g/2)) equals MinkowskiSum(A⊕g, -B).
inline std::vector<std::vector<RotGeom>>
buildRotBank(const std::vector<Clipper2Lib::Path64> &local,
             double rotationStep, int numRots, int64_t gapRadius)
{
    const std::size_t N = local.size();

    std::vector<std::vector<RotGeom>> rotBank(N);
    for (std::size_t i = 0; i < N; ++i) {
        rotBank[i].reserve(static_cast<std::size_t>(numRots));
        for (int r = 0; r < numRots; ++r) {
            RotGeom g;
            g.path = rotatePath(local[i], r * rotationStep);
            ensureCCW(g.path);
            g.bbox = bboxFromPath(g.path);
            g.neg  = negatePath(g.path);
            if (gapRadius > 0) {
                auto inflated = Clipper2Lib::InflatePaths({g.path},
                    static_cast<double>(gapRadius),
                    Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon);
                g.aref = (!inflated.empty()) ? inflated[0] : g.path;
            } else {
                g.aref = g.path;
            }
            rotBank[i].push_back(std::move(g));
        }
    }
    return rotBank;
}

/// Normalize every contour to its own min corner, in engine coordinates.
inline std::vector<Clipper2Lib::Path64>
normalizeToLocal(const std::vector<Clipper2Lib::Path64> &pieces)
{
    std::vector<Clipper2Lib::Path64> local(pieces.size());
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const BBox bb = bboxFromPath(pieces[i]);
        local[i] = translatePath(pieces[i], -bb.xMin, -bb.yMin);
    }
    return local;
}

} // namespace NestGeom

#endif // NESTGEOM_H
