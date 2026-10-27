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
 **          connaît ni Bin ni Engine — sauf la FORME du cache NFP, qui est un
 **          type et une fabrique de clé, pas un état : deux moteurs qui
 **          cachaient leurs NFP sous deux formes auraient eu deux vérités pour
 **          une memoïsation.
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
#include <unordered_map>
#include <vector>

#include <clipper2/clipper.h>
#include <clipper2/clipper.minkowski.h>

// Le vocabulaire du placement — `Placement`, et `Anchor` qu'une entrée de banque porte. Le noyau
// géométrique ne connaît ni `Bin` ni `Engine`, mais il sait où va une pièce : c'est le type que
// `buildRotBank` recopie sur chaque `RotGeom`, et le déclarer ailleurs en ferait un second nom.
#include "model/Placement.h"

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

/// Tolérance de simplification du contour donné au NFP, en unités mises à l'échelle — 0,5 px.
///
/// **Le NFP n'a pas besoin du contour exact** : c'est un test de collision, et il gonfle déjà les
/// pièces du jeu entier pour les tenir à distance. Une somme de Minkowski coûte quadratiquement en
/// sommets (mesuré : 73 ms à 90 sommets, 2 min 25 sur un run d'impression réel) ; à 0,5 px de
/// fidélité le plan est identique au centième de millimètre et le run trois fois plus court.
///
/// ⚠️ Seul `aref` est simplifié. `path` reste exact : dessin, PDF et payload ne bougent pas.
inline constexpr double kNfpSimplifyEpsilon = 0.5 * kScale;

// ── The shape of the NFP cache ──────────────────────────────────────────────
//
// Moved here from `Engine`, where it was public and reached into by the Tiled
// engine (`Engine::NFPCache`) — a second engine borrowing the first one's cache
// type for a memoïsation both of them make. The cache is the NFP's, and the NFP
// is this file's.
//
// ⚠️ A TYPE and a pure factory, not state: the maps themselves live in each
// engine, for the length of one run.

/// Packed key: pieceA (16b) | rotA (16b) | pieceB (16b) | rotB (16b).
using NFPCacheKey = std::uint64_t;
using NFPCache     = std::unordered_map<NFPCacheKey, Clipper2Lib::Paths64>;

inline NFPCacheKey makeNFPCacheKey(int pieceA, int rotA, int pieceB, int rotB) noexcept
{
    return (std::uint64_t(pieceA) << 48) | (std::uint64_t(rotA) << 32)
         | (std::uint64_t(pieceB) << 16) |  std::uint64_t(rotB);
}

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

/// Lowest vertex of @p paths lying exactly on the vertical line `x == @p x`.
///
/// **The wall query.** A piece glued to the bin's inner left edge has its origin ON that edge and
/// nothing else to choose, so its position is read among the vertices that sit on the line — never
/// among the ones beside it. Intersecting the region with a thin band instead was tried and is
/// WRONG, not merely imprecise: `findBottomLeft` returns a vertex, and on a band `[x, x+ε]` the
/// lowest vertex is the lowest point of the region anywhere in the band — the wall is often blocked
/// from the floor up while the band is free higher, so the answer comes back at `x + ε`, a piece
/// laid off the wall by ε and reported as laid. The exact answer is always a vertex **on the line**:
/// the region's left edge is the placement window's own left edge, and it survives subtraction
/// only as an edge whose corners and crossings are all vertices.
///
/// Returns `{x, INT64_MAX}` when no vertex lies on the line — the same "nothing here" answer
/// @ref findBottomLeft gives, so the caller's existing empty test catches it unchanged.
inline Clipper2Lib::Point64 findBottomAtX(const Clipper2Lib::Paths64 &paths, int64_t x)
{
    Clipper2Lib::Point64 best = {x, (std::numeric_limits<int64_t>::max)()};
    for (const auto &path : paths) {
        for (const auto &p : path) {
            if (p.x == x && p.y < best.y)
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

    /// The angle this entry was built at, in degrees.
    ///
    /// **Carried rather than recomputed from the index.** A piece whose direction is known is not
    /// swept on a regular grid: its bank holds the two or four angles its grainline allows, and
    /// those are the piece's own angles, not multiples of the step. Every reader that used to say
    /// `index * rotationStep` reads this instead.
    double angle = 0.0;

    /// The wall this entry is glued to, if any.
    ///
    /// Carried on the geometry for the same reason the angle is: it is what the CALLER decided
    /// about this piece, and a second place that recomputed it could answer differently. It rides
    /// the bank rather than the piece list so that `placeSequential` — called hundreds of times by
    /// the annealing, once per order and per rotation — reads it where it already reads everything
    /// else about the entry.
    Anchor anchor = Anchor::Free;
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
/// @param allowedAngles one list per piece, parallel to @p local. **An empty list is a piece
///        nobody constrained**, and it keeps the whole regular sweep; a non-empty one is a piece
///        whose direction is known, tried at those angles and at those only. A shorter vector than
///        @p local leaves the pieces it does not reach unconstrained.
/// @param anchors one value per piece, parallel to @p local, on the same terms: an empty or short
///        vector leaves the pieces it does not reach glued to nothing. It says nothing about the
///        angles — a glued piece is still swept by whatever @p allowedAngles gives it, and a
///        caller that wants one rotation only says so there.
inline std::vector<std::vector<RotGeom>>
buildRotBank(const std::vector<Clipper2Lib::Path64> &local,
             double rotationStep, int numRots, int64_t gapRadius,
             const std::vector<std::vector<double>> &allowedAngles = {},
             const std::vector<Anchor> &anchors = {})
{
    const std::size_t N = local.size();

    std::vector<std::vector<RotGeom>> rotBank(N);
    for (std::size_t i = 0; i < N; ++i) {
        const bool constrained = i < allowedAngles.size() && !allowedAngles[i].empty();
        const int count = constrained ? static_cast<int>(allowedAngles[i].size()) : numRots;
        const Anchor anchor = i < anchors.size() ? anchors[i] : Anchor::Free;

        rotBank[i].reserve(static_cast<std::size_t>(count));
        for (int r = 0; r < count; ++r) {
            RotGeom g;
            g.anchor = anchor;
            g.angle = constrained ? allowedAngles[i][static_cast<std::size_t>(r)]
                                  : r * rotationStep;
            g.path = rotatePath(local[i], g.angle);
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

            // ── Le NFP ne voit pas le contour exact, il en voit une copie allégée ───────────────
            //
            // `aref` n'a qu'un lecteur : la somme de Minkowski du NFP. Or cette somme est une
            // convolution n × m — son coût est **quadratique en sommets**. Mesuré le 26-10-2026 sur
            // un run d'impression réel : 90 sommets par contour gonflé, ~2000 sommes, **73 ms
            // chacune**, 145 s de moteur sur un run de 2 min 32. Rien d'autre ne pèse.
            //
            // ⚠️ **La tolérance est minuscule devant le jeu, et c'est ce qui la rend sûre.** Le jeu
            // de ce run vaut 18,9 px ; on simplifie à 0,5 px, soit trente-huit fois moins. Deux
            // contours que la simplification rapproche de 0,5 px restent donc séparés par plus de
            // 18 px : le plan ne peut pas se recouvrir. Mesuré sur le run : le plan est identique au
            // centième de millimètre (`usedHeight` 1211,09 mm avant comme après), et le moteur passe
            // de 142,7 s à 52,3 s.
            //
            // ⚠️ Et **seul `aref` est simplifié** : `path` reste le contour exact, donc le dessin,
            // le PDF et le payload gardent leur géométrie au bit près. Le NFP est un test, pas un
            // tracé.
            //
            // La tolérance est dérivée du JEU, jamais fixée en dur : un vingtième du jeu, plafonné à
            // `kNfpSimplifyEpsilon`. Un run sans jeu n'est pas simplifié du tout — sans jeu, il n'y
            // a plus de marge pour absorber l'approximation.
            if (gapRadius > 0 && g.aref.size() >= 4) {
                const double tolerance = std::min(kNfpSimplifyEpsilon, static_cast<double>(gapRadius) * 0.05);
                g.aref = Clipper2Lib::SimplifyPath(g.aref, tolerance, true);
            }
            rotBank[i].push_back(std::move(g));
        }
    }
    return rotBank;
}

/// Index of the entry of @p bank built at @p angle, or -1 when the bank holds none.
///
/// The angles are compared modulo a full turn: a bank entry at -90° answers for 270°. **An absent
/// angle is a real answer**: a constrained piece only has the orientations its grainline allows,
/// and there is nothing to round to the nearest — the caller has to skip the move.
inline int IndexOfAngle(const std::vector<RotGeom> &bank, double angle, double tolerance = 1e-6)
{
    for (std::size_t r = 0; r < bank.size(); ++r) {
        double d = std::fmod(bank[r].angle - angle, 360.0);
        if (d < 0.0) {
            d += 360.0;
        }
        if (d < tolerance || 360.0 - d < tolerance) {
            return static_cast<int>(r);
        }
    }
    return -1;
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
