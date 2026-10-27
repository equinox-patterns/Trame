/************************************************************************
 **
 **  @file   TiledEngine.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   17/09/2026
 **
 **  @brief  Second moteur de nidification — celui du format TUILLÉ.
 **
 **          Le moteur de bande (Engine) minimise une hauteur : la page
 **          n'existe pas pour lui, et la grille est une grille posée après
 **          coup sur le résultat. Mesuré sur un patron réel : hauteur
 **          1088,050 mm pour une frontière de rangée à 1036 mm — 52 mm de
 **          trop, payés d'une rangée entière, 25 feuilles pour un plancher
 **          de surface de 15.
 **
 **          Ce moteur-ci paie en PAGES. Il pose les grosses pièces d'abord,
 **          puis glisse les petites dans les trous au VRAI CONTOUR — c'est
 **          l'imbrication dans le creux d'une courbe qui rend la place, pas
 **          la boîte englobante. Il n'ouvre une rangée que lorsqu'aucune
 **          pièce restante n'entre dedans.
 **
 **          ⚠️ Les « holders » ne sont PAS posés. Ce sont les boîtes
 **          englobantes : elles donnent l'ORDRE (les grosses d'abord) et le
 **          coût en pages d'une pièce isolée. Instrument de priorité et de
 **          mesure, jamais objet à empiler.
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

#ifndef TILEDENGINE_H
#define TILEDENGINE_H

#include <cstdint>
#include <vector>

#include <clipper2/clipper.h>

#include "model/Placement.h"
#include "nesting/Engine.h"
#include "nesting/NestGeom.h"

class Bin;

/// Nesting engine for tiled formats — pays in sheets, not in height.
class TiledEngine
{
public:
    /// Everything this engine needs, and nothing else.
    ///
    /// Deliberately NOT `Engine::Config`: that one carries a cooling schedule,
    /// an iteration budget and a plateau limit, none of which mean anything to
    /// a greedy placement. Interface segregation — a struct with six fields a
    /// reader has to rule out is a struct that lies about what matters.
    struct Spec
    {
        /// **L'ordre dans lequel les pièces sont posées.** Le moteur est glouton : il pose, et ne
        /// revient jamais. La PREMIÈRE pièce prend la meilleure place, la dernière se contente du
        /// reste — donc l'ordre décide de tout le plan.
        ///
        /// Trois ordres, trois plans complets, à partir des mêmes pièces. C'est l'application qui
        /// les compare et garde celui qui imprime le moins de pages : **le moteur ne sait pas ce
        /// qu'est une feuille**, et n'a pas à le savoir.
        enum class Order
        {
            HolderArea,    ///< la surface du holder, décroissante — l'ordre historique
            HolderHeight,  ///< la hauteur, décroissante — l'ordre du rangeur d'étagères
            HolderWidth    ///< la largeur, décroissante
        };

        Order order{Order::HolderArea};

        /// One sheet, in the caller's unit (pixels here, as everywhere).
        double sheetWidth  = 0.0;
        double sheetHeight = 0.0;

        /// Clear distance held between two pieces, same unit.
        double gap = 0.0;

        /// Half a turn is a real difference for a shape; a quarter is what a
        /// hole needs. The full 2° bank the annealing uses would multiply the
        /// work by forty-five for orientations a garment piece never uses.
        bool allowRotation = true;

        /// Asked between the pieces the greedy pass places: true means the caller wants the run
        /// given up. **Null means never**, as in Engine::Config — see there for what it is for and
        /// what it costs to answer.
        std::function<bool()> abandoned;
    };

    /// The surfaces that decide the target — Sophie's own figures.
    ///
    /// The footprint area is the honest lower bound and it is NOT reachable:
    /// it assumes the ink paves the sheet and ignores every hollow between
    /// shapes. The holder area is the rectangular bound — what the surfaces
    /// look like when each piece is boxed. The truth lives between the two,
    /// and `margin` is the gap between them, stated rather than hidden.
    struct Floors
    {
        double footprintArea = 0.0;  ///< Sum of the true contours, absolutes
        double holderArea    = 0.0;  ///< Sum of the bounding boxes
        double margin        = 0.0;  ///< holderArea − footprintArea
        double sheetArea     = 0.0;  ///< One sheet
        int    footprintSheets = 0;  ///< ceil(footprintArea / sheetArea) — optimiste
        int    holderSheets    = 0;  ///< ceil(holderArea / sheetArea) — réaliste
    };

    /// The two surfaces and the two floors, in the caller's unit squared.
    static Floors ComputeFloors(const std::vector<Clipper2Lib::Path64> &pieces,
                                const Spec &spec);

    explicit TiledEngine(const Spec &spec) noexcept;

    /// Place every piece. Same contract as Engine::place: one Placement per
    /// input piece, `x`/`y` the min-bbox corner of the piece BEFORE rotation,
    /// `rotation` in degrees, and `rotation < 0` marks a piece that could not
    /// be placed at all.
    ///
    /// `Bin::zones()` IS read, like the strip engine reads it: a piece may not be
    /// placed where the run must not print — the block a cover page stands in, and
    /// any page a tiled plan has to keep clear. Both engines subtract them through
    /// the same implementation (`nesting/ZoneGeom.h`), so the two cannot come to
    /// disagree about what keeping a rectangle free means.
    ///
    /// A zone the bin does not have changes NOTHING here: with an empty list the
    /// subtraction loop never runs and the placements are the ones this engine has
    /// always made.
    ///
    /// ⚠️ A zone that leaves no room in any row stops the search rather than opening
    /// rows for ever — the piece is marked unplaced (`rotation < 0`), exactly like a
    /// piece wider than the bin, and the caller keeps whatever the strip engine
    /// produced.
    /// @p abandoned is set to true when the run was given up — see Engine::place, whose contract
    /// this one shares: what comes back from an abandoned run is a half-built plan.
    std::vector<Placement> place(const Bin &bin,
                                 const std::vector<Clipper2Lib::Path64> &pieces,
                                 bool *abandoned = nullptr) const;

private:
    /// Whether the caller has asked for the run to be given up. False when it handed over none.
    bool IsAbandoned() const
    {
        return m_spec.abandoned && m_spec.abandoned();
    }

    Spec m_spec;
};

#endif // TILEDENGINE_H
