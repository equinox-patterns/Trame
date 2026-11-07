/************************************************************************
 **
 **  @file   PageRepack.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   07/11/2026
 **
 **  @brief  Reposage d'un plan TUILÉ déjà calculé : moins de feuilles par pièce.
 **
 **          Un plan tuilé n'est pas jugé sur sa hauteur mais sur son papier, et le papier se compte
 **          deux fois : en FEUILLES, ce que fait la passe des pages du moteur tuilé — et en PIÈCES À
 **          CHEVAL, ce que personne ne comptait. Une pièce sur six feuilles se recoupe et se recolle
 **          six fois à l'atelier ; la même pièce posée trois centimètres plus bas tient sur trois.
 **
 **          Ce module est une SECONDE PASSE, et elle ne nudifie rien : elle relit un plan, déplace
 **          des pièces dans le vide qu'elles ont déjà autour d'elles, et rend le même plan en mieux
 **          ou tel qu'il était. Elle ne connaît aucun moteur — c'est le plan qui s'imprime qui est
 **          reposé, quel que soit celui des deux qui l'a produit.
 **
 **          ⚠️ **Elle n'agrandit jamais le plan.** Une position qui ajouterait une rangée est
 **          refusée, et le nombre de feuilles imprimées ne peut pas monter : le seul gain possible
 **          est un gain de pièces à cheval, jamais une rangée de plus.
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

#ifndef PAGEREPACK_H
#define PAGEREPACK_H

#include <vector>

#include <clipper2/clipper.h>

#include "model/Placement.h"

class Bin;

/// @brief Remettre chaque pièce là où elle tient sur le moins de feuilles.
///
/// Le plan n'est pas recalculé : les pièces glissent dans le vide existant, une par une, et une
/// pièce ne bouge que si elle touche **strictement** moins de feuilles ensuite. La rotation n'est
/// essayée qu'en secours, quand aucune translation ne fait gagner (décision de Sophie, 07-11-2026).
namespace PageRepack
{
/// La grille de tuliage, telle que l'hôte la coupe — lue chez lui, jamais recalculée ici.
struct Frame
{
    double cellWidth  = 0.0;  ///< Une feuille en largeur, dans l'unité de l'appelant
    double cellHeight = 0.0;  ///< Une feuille en hauteur
    int    columns    = 0;    ///< Feuilles côte à côte
    int    rows       = 0;    ///< Rangées de feuilles que le plan occupe
    double gap        = 0.0;  ///< L'écart clair tenu entre deux pièces
};

/// Ce que la passe a trouvé pour UNE pièce — la mesure, pas le récit.
///
/// Les trois nombres de feuilles sont là pour dire **où ça bloque** : `tried` à zéro veut dire
/// qu'aucune position légale n'a été trouvée ; `bestSeen` au-dessus de `before` veut dire que le vide
/// existe mais ne fait rien gagner ; et `bestSeen` en dessous avec `best` égal à `before` veut dire
/// qu'un gain a été trouvé puis REFUSÉ — c'est la seule des trois qui accuse la garde du total.
struct SheetCount
{
    int  before       = 0;      ///< Feuilles qu'elle touchait au départ
    int  best         = 0;      ///< Feuilles qu'elle touche à la fin
    int  bestSeen     = 0;      ///< Le moins de feuilles qu'une position LÉGALE ait atteint
    int  tried        = 0;      ///< Positions légales réellement évaluées
    int  generated    = 0;      ///< Positions candidates essayées, légales ou non
    int  zoneRefused  = 0;      ///< Parmi elles, celles qu'une zone interdite a écartées
    int  turn         = 0;      ///< 0 : translation ; 1 à 3 : quart de tour
    bool moved        = false;
    bool refusedByTotal = false;  ///< Un gain existait, mais le plan aurait coûté plus cher
};

/// Le compte rendu d'un passage — rempli pour être LU (trace, diagnostic), jamais pour décider.
struct Report
{
    bool ran      = false;  ///< Faux quand la grille interdit tout reposage (une seule feuille)
    int  pieces   = 0;
    int  columns  = 0;
    int  rows     = 0;
    int  sheetsBefore = 0;  ///< Feuilles imprimées avant la passe
    int  sheetsAfter  = 0;  ///< Feuilles imprimées après
    int  straddlingBefore = 0;  ///< Pièces à cheval sur plus d'une feuille, avant
    int  straddlingAfter  = 0;  ///< Les mêmes, après
    std::vector<SheetCount> perPiece;
};

/// Ce que la passe « donner de l'air » a fait pour UNE pièce.
///
/// Les glissements sont ce que les règles laissent encore à la pièce de chaque côté : c'est le
/// chiffre qui explique le déplacement, et il se lit comme Sophie parle du plan — « il y a 30 mm à
/// gauche et 5 mm à droite, donc on la décale de 12,5 mm vers la gauche ».
///
/// ⚠️ **Deux glissements par côté, et la différence est le diagnostic.** `leftSlackMm` est ce que
/// TOUTES les règles laissent ; `leftFreeMm` ce que le seul CONTACT laisse. Un côté qui donne 0 au
/// premier et 20 au second n'est pas coincé : c'est une règle qui le retient, et `leftWhy` dit
/// laquelle. Sans cette distinction, « ça ne bouge pas » ne dit pas si la place manque ou si on la
/// refuse.
struct Breath
{
    /// Pourquoi un côté ne peut pas glisser du tout — voir `PageRepack::Why`.
    int  leftWhy   = 0;
    int  rightWhy  = 0;
    int  topWhy    = 0;
    int  bottomWhy = 0;

    /// ⚠️ **Toutes ces distances sont dans l'unité de l'appelant — des PIXELS**, comme tout ce que
    /// ce noyau reçoit. L'application les convertit en millimètres pour les lire ; un nom qui
    /// promettrait des millimètres ici mentirait d'un facteur 3,78.
    double leftSlackPx   = 0.0;  ///< Toutes règles comprises
    double rightSlackPx  = 0.0;
    double topSlackPx    = 0.0;
    double bottomSlackPx = 0.0;

    double leftFreePx   = 0.0;   ///< Le contact seul, règles ignorées
    double rightFreePx  = 0.0;
    double topFreePx    = 0.0;
    double bottomFreePx = 0.0;

    /// Ce que les BORNES du plan laissent, avant toute autre règle : un plafond, pas une mesure.
    double leftRoomPx   = 0.0;
    double rightRoomPx  = 0.0;
    double topRoomPx    = 0.0;
    double bottomRoomPx = 0.0;

    double shiftXPx = 0.0;
    double shiftYPx = 0.0;
    bool   moved    = false;
};

/// Pourquoi une position est refusée — l'ordre est celui des tests, du moins cher au plus cher.
///
/// **Les quatre bords du plan ont chacun leur code** : « hors du plan » ne dit pas lequel, et c'est
/// exactement ce qu'on a besoin de lire quand une pièce posée à la marge ne bouge dans aucun sens.
enum Why : int
{
    WhyAllowed  = 0,  ///< Rien ne s'y oppose
    WhyLeft     = 1,  ///< Elle sortirait par le bord gauche
    WhyRight    = 2,  ///< … par le bord droit
    WhyTop      = 3,  ///< … par le bord haut
    WhyBottom   = 4,  ///< … par le bord bas
    WhyJunction = 5,  ///< Un bord raserait une jonction
    WhyMorePage = 6,  ///< La pièce toucherait une feuille de plus
    WhyZone     = 7,  ///< Une zone interdite la prend
    WhyPiece    = 8   ///< Une autre pièce la prend
};

/// @brief Reposer @p placements sur le moins de feuilles possible.
///
/// @p pieces est la liste des contours du plan, dans les mêmes coordonnées que les placements
/// (celles du bin) ; @p placements leur correspond un pour un. @p bin porte la marge et les zones
/// interdites — la page de garde, et toute feuille que le run ne doit pas imprimer.
///
/// Rend un vecteur de même taille et de même ordre : les pièces qui n'ont pas gagné sont rendues
/// exactement telles quelles, et un plan qu'aucune pièce n'améliore revient identique.
///
/// @p report est facultatif et ne change RIEN au plan : il ne sert qu'à dire ce qui a été trouvé.
auto Minimize(const Bin &bin,
              const std::vector<Clipper2Lib::Path64> &pieces,
              const std::vector<Placement> &placements,
              const Frame &frame,
              Report *report = nullptr) -> std::vector<Placement>;

/// @brief Centrer chaque pièce dans le couloir que les règles lui laissent — « donner de l'air ».
///
/// Le nesting est fini et il n'y a plus rien à gagner en feuilles : ce qui reste, c'est l'espace mal
/// réparti. Une pièce serrée contre sa voisine d'un côté et loin de tout de l'autre n'est pas
/// fausse, elle est inconfortable — à couper, à lire, à poser. Cette passe mesure **jusqu'où la
/// pièce peut encore glisser de chaque côté**, puis la décale de la moitié de la différence : deux
/// écarts inégaux deviennent deux écarts égaux.
///
/// Ce que les règles laissent, et rien de plus — le jeu entre deux pièces, la marge du plan, et deux
/// gardes reprises du reposage : **aucune feuille de plus** (les feuilles touchées restent les mêmes)
/// et **aucun bord sur une jonction**. Une pièce qui ne peut plus boucler de 0,2 mm ne bouge pas.
///
/// L'axe horizontal est traité d'abord, puis le vertical sur la position obtenue : deux couloirs
/// indépendants, et l'ordre est là pour que le résultat soit reproductible.
auto Breathe(const Bin &bin,
             const std::vector<Clipper2Lib::Path64> &pieces,
             const std::vector<Placement> &placements,
             const Frame &frame,
             std::vector<Breath> *report = nullptr) -> std::vector<Placement>;

/// @brief L'écart le plus faible entre un bord de pièce et une jonction de feuilles, en pixels.
///
/// Une MESURE, pas une décision : elle sert à vérifier un plan, et à le dire. C'est la même fonction
/// qui juge (`NestGeom::borderToLine`), donc ce chiffre ne peut pas contredire la règle — une pièce
/// traverse une jonction compte par sa plus fine excroissance, une pièce qui s'arrête à côté par sa
/// distance au bord.
///
/// Rend -1 quand le plan n'a aucune jonction à mesurer (une seule feuille).
auto ClosestBorderToJunction(const std::vector<Clipper2Lib::Path64> &pieces,
                             const std::vector<Placement> &placements,
                             const Frame &frame) -> double;
} // namespace PageRepack

#endif // PAGEREPACK_H
