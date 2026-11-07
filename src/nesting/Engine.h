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

#ifndef ENGINE_H
#define ENGINE_H

#include <cstdint>
#include <functional>
#include <vector>

#include <clipper2/clipper.h>
#include <clipper2/clipper.minkowski.h>

#include "model/Placement.h"
#include "nesting/NestGeom.h"
#include "nesting/ZoneGeom.h"

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

        /// Budget de la recherche, en millisecondes. **Zéro veut dire « pas de budget »**, le
        /// comportement d'avant, gardé pour les appelants qui n'ont personne à faire attendre.
        ///
        /// ⚠️ **Sans budget, la durée d'un run ne dépend d'aucune des autres bornes.** Le recuit
        /// enchaîne jusqu'à 456 passages de placement — le plancher de température (T ≤ 0,01 avec
        /// 100 et 0,98) tombe avant `maxIterations`, et le frein de plateau ne pouvait pas se
        /// déclencher avant que T passe sous 0,1, soit l'itération 343. Chaque passage replace
        /// toutes les pièces, et chaque couple (pièce, rotation) jamais essayé coûte une somme de
        /// Minkowski. Un plan correct existe dès le premier passage : le budget ne coupe pas la
        /// réponse, il coupe la recherche de la réponse parfaite.
        int    maxMilliseconds = 0;

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

        /// Asked between annealing iterations and between adjustment passes: true means the
        /// caller wants the run given up, and it is given up there and then.
        ///
        /// **Null means never**, which is what a caller with nobody in front of it wants: a batch
        /// run has no one to ask and nothing to give up for. It is the caller with a window — and
        /// someone watching it — that hands one over.
        ///
        /// ⚠️ **Called on the worker thread.** Whatever it reads must be readable from there, and
        /// it must be cheap: it runs once per annealing iteration, of which a run has hundreds.
        std::function<bool()> abandoned;
    };

    // ── NFP cache ──────────────────────────────────────────────────────────

    /// The cache's shape lives in `nesting/NestGeom.h`, with the NFP it memoïses:
    /// both engines make that cache, so it could not stay one engine's private
    /// arrangement. The two names are kept here because this class's own signatures
    /// are written with them.
    ///
    /// `makeNFPCacheKey` is NOT re-exported: a using-declaration at class scope may
    /// only name a base class's member, and a one-line forwarder would have been a
    /// second name for one function. Callers name it `NestGeom::makeNFPCacheKey`.
    using NFPCacheKey = NestGeom::NFPCacheKey;
    using NFPCache    = NestGeom::NFPCache;

    explicit Engine(const Config &cfg) noexcept;

    /// Place all pieces in the bin. Returns a placement for each input piece.
    ///
    /// **A given-up run returns what it had when it was stopped**, which may be fewer placements
    /// than there are pieces, or none at all. @p abandoned says which happened — a caller that
    /// passed no callback can never see true. A run that was abandoned has no answer to give: its
    /// result is a half-built plan, and reading it as a finished one would place pieces that were
    /// never placed.
    /// @param allowedAngles one list of angles per piece, parallel to @p pieces. **Empty — the
    ///        default — leaves every piece free**, which is what the print run asks for and what
    ///        every caller before this parameter existed got. A non-empty list pins that piece to
    ///        the angles it holds, and the piece is never seen at any other: a piece laid on fabric
    ///        follows the grain, and the grain is not negotiable.
    /// @param anchors which wall of the bin a piece is glued to, parallel to @p pieces on the same
    ///        terms — **empty, the default, glues nothing**. A glued piece keeps its rotations and
    ///        its slide along the wall; what it loses is its position across the bin, which the
    ///        caller has already decided. It is how a piece cut on the fold is laid: its fold line
    ///        has to sit exactly on the fabric's fold, and a hair away is a piece cut to the wrong
    ///        width. @see Anchor
    std::vector<Placement> place(const Bin &bin,
                                 const std::vector<Clipper2Lib::Path64> &pieces,
                                 const std::vector<std::vector<double>> &allowedAngles = {},
                                 const std::vector<Anchor> &anchors = {},
                                 bool *abandoned = nullptr) const;

    /// Envelope height of a placement result, in the unit of the input polygons
    /// (the bin's BOTTOM margin included; the top one is already inside the
    /// placements, since the first piece starts a margin below the edge).
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
    /// used height WITH the bin's bottom margin already in it — which is what UsedHeight returns —
    /// and both lengths are in the unit of the input polygons. artboardHeight is in that same unit,
    /// as Config says.
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

    /// The geometry of the bin's zones lives in `nesting/ZoneGeom.h`, with the ONE
    /// implementation of "a piece must avoid every zone" — the Tiled engine asks the
    /// same question, so neither engine may hold the answer alone.
    ///
    /// The alias is kept because this class's own signatures are written with it, and
    /// because the type is what `Build` returns: naming it twice would be one truth
    /// wearing two names.
    using ZoneGeom = NestZone::Geom;

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

    /// ── Les sommes de Minkowski d'un passage, calculées de front ────────────────────────────────
    ///
    /// Un passage du recuit replace les pièces une à une : pour chaque pièce, le moteur soustrait du
    /// bac les polygones de non-recouvrement (`NFP`) entre cette pièce et CHACUNE des pièces déjà
    /// posées, à son orientation du moment. Ces sommes sont indépendantes les unes des autres, et
    /// c'est elles qui coûtent : mesuré, 1,01 s par passage pour 14 pièces, soit ~72 ms par pièce.
    ///
    /// **On les calcule donc avant, sur plusieurs cœurs, et on remplit le cache.** Le placement qui
    /// suit ne fait plus que les lire. Les valeurs sont rigoureusement les mêmes — chaque somme est
    /// une fonction pure de ses deux orientations — donc le plan rendu est identique au bit près ;
    /// seule l'horloge change. C'est ce qui rend cette optimisation vérifiable : la longueur ne doit
    /// pas bouger d'un millimètre.
    ///
    /// ⚠️ **Rien n'est écrit dans le cache pendant que les cœurs tournent.** Chaque somme rend son
    /// résultat dans son propre emplacement, et l'insertion se fait en série après la jonction : un
    /// `unordered_map` partagé entre huit fils serait, lui, une course.
    void PrewarmNfp(const std::vector<std::vector<RotGeom>> &rotBank,
                    const std::vector<int> &rotIdx,
                    const std::vector<int> &order,
                    NFPCache &nfpCache) const;

    // ── Optimisation ─────────────────────────────────────────────────────

    /// @return true when the run was given up before the search ran its course.
    bool anneal(const Bin &bin,
                const std::vector<std::vector<RotGeom>> &rotBank,
                NFPCache &nfpCache,
                NFPCache &obstacleCache,
                const std::vector<ZoneGeom> &zones,
                std::vector<Placement> &best,
                int64_t infY) const;

    /// Whether the caller has asked for the run to be given up. False when it handed over none.
    bool IsAbandoned() const
    {
        return m_config.abandoned && m_config.abandoned();
    }

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
    /// @return true when the run was given up before the pass ran its course.
    bool refine(const Bin &bin,
                const std::vector<std::vector<RotGeom>> &rotBank,
                std::vector<Placement> &best,
                const std::vector<Clipper2Lib::Path64> &pieces) const;

    /// ── La SECONDE PASSE d'un plan de bande : le blanc laissé par le recuit ────────────────────
    ///
    /// Un plan de bande (une laize, un rouleau, une page) n'a pas de grille : `refine()` ci-dessus
    /// aligne des blocs sur des lignes de pages qui n'existent pas ici. Le principe reste le même —
    /// une seconde passe, APRÈS le placement, qui ne rend jamais un plan plus long — mais le levier
    /// n'est pas le même : dans une bande, la longueur vient de l'ORDRE dans lequel les pièces sont
    /// posées, et le recuit part d'un ordre par surface qu'il n'a aucune raison de garder.
    ///
    /// Le blanc d'un plan se lit dans le plan lui-même : on relit les étages déjà posés, on en tire
    /// plusieurs ordres (du bas, du haut, par hauteur, par largeur) et on repose les mêmes pièces aux
    /// MÊMES orientations. Chaque repose est un `placeSequential`, donc les mêmes sommes de Minkowski
    /// déjà en cache : le coût est de l'ordre du placement d'un plan, pas d'une recherche.
    ///
    /// **Le contrat est celui de `refine()` : on vérifie à la fin, et on ne commite que si le plan
    /// est strictement plus court.** Une seconde passe qui peut allonger n'est pas une seconde passe.
    bool refineStrip(const Bin &bin,
                     const std::vector<std::vector<RotGeom>> &rotBank,
                     NFPCache &nfpCache,
                     NFPCache &obstacleCache,
                     const std::vector<ZoneGeom> &zones,
                     std::vector<Placement> &best,
                     const std::vector<Clipper2Lib::Path64> &pieces,
                     int64_t infY) const;

    Config m_config;
};

#endif // ENGINE_H
