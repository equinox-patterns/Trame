/************************************************************************
 **
 **  @file   ZoneGeom.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   22/09/2026
 **
 **  @brief  Les zones interdites du bin, telles que les DEUX moteurs
 **          doivent les lire.
 **
 **          Extraites d'Engine le jour où le moteur Tiled a dû les honorer
 **          lui aussi. Une zone est une règle — « rien ne se pose ici » — et
 **          deux moteurs qui y auraient répondu chacun à leur façon auraient
 **          été deux vérités pour une règle.
 **
 **          ⚠️ Contrairement à NestGeom.h, ce fichier ALLOUE : construire une
 **          zone, c'est l'inflater, et tout l'intérêt de `Build` est que le
 **          résultat soit calculé une fois par run puis réutilisé. C'est
 **          l'autre moitié de la raison pour laquelle il n'est pas dans
 **          NestGeom.h, dont le contrat dit qu'aucune fonction n'y alloue.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef ZONEGEOM_H
#define ZONEGEOM_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "nesting/Bin.h"
#include "nesting/NestGeom.h"

/// Ce qu'une zone interdite devient quand le moteur peut s'en servir.
///
/// Délibérément PAS `Bin::Zone` : l'appelant énonce un rectangle dans SON unité,
/// et ceci est ce rectangle en géométrie moteur — mis à l'échelle, normalisé,
/// gonflé du jeu, et prêt à être mis en cache. Les tenir séparés est ce qui
/// laisse `Bin` une simple description du bin.
namespace NestZone {

/// Une des zones du bin, en géométrie moteur.
///
/// Normalisée sur son propre coin min pour que la NFP par (pièce, rotation) soit
/// mise en cache SUR elle et simplement translatée vers (originX, originY) : la
/// place d'une zone peut alors changer sans toucher au cache.
struct Geom
{
    Clipper2Lib::Path64 local;      ///< La zone, gonflée du jeu, normalisée

    /// Boîte englobante de la zone AVANT gonflage — le chiffre que lit le test
    /// « cette zone peut-elle seulement compter ». Gardée exactement comme
    /// l'obstacle A0 l'avait : le test est un filtre conservateur, et l'élargir
    /// changerait le moment où un clip est sauté.
    BBox bboxLocal;

    int64_t originX = 0;  ///< Où se tient le coin min local dans le bin
    int64_t originY = 0;
};

/// Marque le troisième champ de 16 bits d'une clé de cache d'obstacle comme un
/// index de ZONE plutôt que de pièce.
///
/// La clé a quatre champs et la NFP d'obstacle n'en a jamais utilisé que deux,
/// ce qui est pourquoi un seul emplacement ne pouvait pas distinguer deux zones
/// — la note dans `Subtract` désigne cela comme le premier piège de ce
/// changement.
constexpr int kKeyTag = 0xFFFF;

/// Les zones du bin en géométrie moteur, construites UNE fois par run.
///
/// Chaque zone revient comme son rectangle à sa propre place, normalisé puis
/// gonflé du jeu COMPLET. Reconstruire ceci dans la boucle de placement — qui
/// tourne une fois par itération de recuit, des centaines de fois par run —
/// était gratuit pour un rectangle et ne l'est pas pour K.
inline auto Build(const std::vector<Bin::Zone> &zones, int64_t gapScaled) -> std::vector<Geom>
{
    std::vector<Geom> out;
    out.reserve(zones.size());

    for (const Bin::Zone &z : zones)
    {
        if (z.width <= 0.0 || z.height <= 0.0)
            continue;

        const int64_t x0 = static_cast<int64_t>(z.x * NestGeom::kScale);
        const int64_t y0 = static_cast<int64_t>(z.y * NestGeom::kScale);
        const Clipper2Lib::Path64 raw =
            NestGeom::makeRect(x0, y0,
                               x0 + static_cast<int64_t>(z.width  * NestGeom::kScale),
                               y0 + static_cast<int64_t>(z.height * NestGeom::kScale));

        // Normalize, exactement comme l'obstacle A0 l'a toujours été. La NFP est
        // ensuite mise en cache sur cette géométrie locale et seulement translatée
        // vers l'origine ci-dessous : déplacer une zone ne coûte donc rien, et deux
        // zones ne peuvent jamais être confondues l'une avec l'autre.
        const BBox rawBB = NestGeom::bboxFromPath(raw);

        Geom g;
        g.local     = NestGeom::translatePath(raw, -rawBB.xMin, -rawBB.yMin);
        g.bboxLocal = NestGeom::bboxFromPath(g.local);   // AVANT gonflage — voir plus haut
        g.originX   = rawBB.xMin;
        g.originY   = rawBB.yMin;

        // Gonflée du jeu COMPLET, exactement comme les pièces posées. Le diviser par
        // deux ici laisserait une pièce à seulement jeu/2 d'une page interdite — le
        // même défaut, à une seconde place.
        if (gapScaled > 0)
        {
            // La conversion est EXPLICITE, et pas implicite : `InflatePaths` veut un `double`,
            // le jeu est en point fixe entier, et un en-tête se compile partout — y compris
            // là où `-Werror` fait d'une conversion implicite une erreur de compilation.
            // C'était latent tant que ce code vivait dans un `.cpp` que seul Trame compilait.
            auto inflated = Clipper2Lib::InflatePaths({g.local}, static_cast<double>(gapScaled),
                Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon);
            if (!inflated.empty())
                g.local = inflated[0];
        }

        out.push_back(std::move(g));
    }

    return out;
}

/// Les mêmes zones à leurs places absolues, réunies en UNE région.
///
/// Une seule géométrie, pour une passe qui ne demande jamais que « cette pièce
/// a-t-elle le droit d'être ici ».
inline auto Absolute(const std::vector<Geom> &zones) -> Clipper2Lib::Paths64
{
    Clipper2Lib::Paths64 subjects;
    subjects.reserve(zones.size());

    for (const Geom &z : zones)
        subjects.push_back(NestGeom::translatePath(z.local, z.originX, z.originY));

    if (subjects.empty())
        return {};

    // Une région plutôt qu'une liste que l'appelant devrait interpréter : deux zones
    // qui se recouvrent SONT un seul trou, et ce n'est pas à la règle de remplissage
    // de le dire.
    return Clipper2Lib::Union(subjects, Clipper2Lib::FillRule::NonZero);
}

/// Soustraire la NFP de chaque zone de la région faisable en cours de construction.
///
/// **Le seul endroit où « cette pièce doit éviter toutes les zones » est écrit**,
/// et la raison d'être de ce fichier : les deux moteurs l'appellent, donc aucun ne
/// peut se mettre à dire le contraire de l'autre sur ce que garder un rectangle
/// libre veut dire.
///
/// Chaque zone est traitée comme une pièce pré-placée à sa propre place. Clipper64
/// réunit ses clips, ce qui est exactement ce que « la pièce doit les éviter
/// toutes » signifie — la même propriété sur laquelle les pièces posées comptent
/// déjà.
///
/// Le pré-filtre garde les TROIS clauses du moteur de bande, inchangées. C'est
/// correct pour un run tuilé aussi, et délibérément pas « complété » : la troisième
/// clause est une condition nécessaire à une intersection, donc en omettre une
/// quatrième pour un bord fermé ne peut que sauter MOINS de clips — jamais en
/// soustraire un qui comptait.
///
/// @param clipper la région faisable en construction, portant déjà les NFP des
///                pièces posées.
/// @param bg      la pièce et l'orientation en cours de placement.
/// @param ifpX0,ifpX1,ifpY0 les bornes du rectangle intérieur — celles-là mêmes
///                qu'on vient de demander au filtre des pièces posées.
inline void Subtract(Clipper2Lib::Clipper64 &clipper,
                     const std::vector<Geom> &zones,
                     const NestGeom::RotGeom &bg,
                     int pieceIdx, int rIdx,
                     int64_t ifpX0, int64_t ifpX1, int64_t ifpY0,
                     NestGeom::NFPCache &obstacleCache)
{
    for (std::size_t zi = 0; zi < zones.size(); ++zi)
    {
        const Geom &z = zones[zi];

        const int64_t zoneNfpMaxX = z.originX + z.bboxLocal.xMax - bg.bbox.xMin;
        const int64_t zoneNfpMinX = z.originX + z.bboxLocal.xMin - bg.bbox.xMax;
        const int64_t zoneNfpMaxY = z.originY + z.bboxLocal.yMax - bg.bbox.yMin;
        if (zoneNfpMaxY <= ifpY0 || zoneNfpMaxX <= ifpX0 || zoneNfpMinX >= ifpX1)
            continue;

        // La NFP d'une zone ne dépend que du (pièce, rotation) courant ET de la zone,
        // donc la clé de cache porte l'index de zone. C'est le premier piège de ce
        // changement énoncé en code : une clé qui réservait un seul emplacement pour
        // « l'obstacle » laissait deux zones écraser mutuellement leur NFP, et la
        // seconde à être demandée gagnait.
        const NestGeom::NFPCacheKey zkey =
            NestGeom::makeNFPCacheKey(pieceIdx, rIdx, kKeyTag, static_cast<int>(zi));

        const Clipper2Lib::Paths64 *nfpZonePtr = nullptr;
        auto zit = obstacleCache.find(zkey);
        if (zit != obstacleCache.end()) {
            nfpZonePtr = &zit->second;
        } else {
            Clipper2Lib::Paths64 nfpZone =
                Clipper2Lib::MinkowskiSum(z.local, bg.neg, true);
            // Les chemins de NFP en CCW (Clipper2 Difference exige des clips CCW).
            for (auto &np : nfpZone) {
                if (Clipper2Lib::Area(np) < 0)
                    std::reverse(np.begin(), np.end());
            }
            auto [ins, _] = obstacleCache.emplace(zkey, std::move(nfpZone));
            nfpZonePtr = &ins->second;
        }

        clipper.AddClip(NestGeom::translatePaths(*nfpZonePtr, z.originX, z.originY));
    }
}

} // namespace NestZone

#endif // ZONEGEOM_H
