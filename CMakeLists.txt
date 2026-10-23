cmake_minimum_required(VERSION 3.15)
project(Trame VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Qt 6 ───────────────────────────────────────────────────────────────────
# Le noyau n'a besoin ni de Widgets ni de Concurrent : ils ne servent qu'aux
# outils (déclarés plus bas) et à l'application consommatrice.
find_package(Qt6 REQUIRED COMPONENTS Core Xml Svg Gui PrintSupport)

# ── Clipper2 ───────────────────────────────────────────────────────────────
# add_library() sur un nom de cible déjà pris est une erreur FATALE, pas un
# warning : Trame sera le seul endroit qui ajoute Clipper2.
if(NOT TARGET Clipper2)
    set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
    set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/clipper2/CPP EXCLUDE_FROM_ALL)
endif()

# ── ZLIB ───────────────────────────────────────────────────────────────────
find_package(ZLIB REQUIRED)

# ══════════════════════════════════════════════════════════════════════════
# Trame — socle partagé (bibliothèque statique)
# ══════════════════════════════════════════════════════════════════════════
# Le noyau ne contient aucun Q_OBJECT. AUTOMOC coupé : la variable se propage
# depuis le parent, donc un set(CMAKE_AUTOMOC ON) fait AVANT le
# add_subdirectory(Trame) côté consommateur contaminerait ce répertoire.
set(CMAKE_AUTOMOC OFF)

set(TRAME_SOURCES
    src/decor/MarkPainter.cpp
    src/io/NestedSvgWriter.cpp
    src/io/SvgOutliner.cpp
    src/io/SvgTextFace.cpp
    src/io/SvgReader.cpp
    src/nesting/Bin.cpp
    src/nesting/CuttingRules.cpp
    src/nesting/Engine.cpp
    src/nesting/EngineRefine.cpp
    src/nesting/Footprint.cpp
    src/nesting/TiledEngine.cpp
    src/pdf/PdfPayload.cpp
    src/pdf/SvgToOcgPdf.cpp
)

set(TRAME_HEADERS
    src/decor/MarkPainter.h
    src/model/Piece.h
    src/model/Placement.h
    src/model/GradedSize.h
    src/io/GradedSvgContract.h
    src/io/NestedSvgWriter.h
    src/io/SvgOutliner.h
    src/io/SvgTextFace.h
    src/io/SvgReader.h
    src/nesting/Bin.h
    src/nesting/CuttingRules.h
    src/nesting/Engine.h
    src/nesting/Footprint.h
    src/nesting/NestGeom.h
    src/nesting/TiledEngine.h
    src/nesting/ZoneGeom.h
    src/pdf/PdfBuilder.h
    src/pdf/PdfPayload.h
    src/pdf/SvgToOcgPdf.h
)

add_library(Trame STATIC ${TRAME_SOURCES} ${TRAME_HEADERS})

# PUBLIC : les en-têtes s'incluent par chemin racine ("model/Piece.h",
# "nesting/Engine.h") — depuis Trame comme depuis ses consommateurs.
target_include_directories(Trame PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# La version du projet, rendue au code : le renderer PDF l'écrit dans /Producer, et la
# recopier en dur quelque part mentirait au premier changement de version.
target_compile_definitions(Trame PUBLIC TRAME_VERSION="${PROJECT_VERSION}")

# PUBLIC — types exposés dans les en-têtes publics :
#   Qt6::Core -> QString/QVector (Piece.h), QByteArray/QRectF (SvgToOcgPdf.h)
#   Qt6::Xml  -> QDomElement (SvgToOcgPdf.h)
#   Clipper2  -> Path64/Paths64 (Piece.h, Engine.h, Footprint.h)
# PRIVATE — utilisés uniquement dans les .cpp (remontent quand même sur la
# ligne de link des consommateurs, une statique n'ayant pas de symboles cachés).
target_link_libraries(Trame
    PUBLIC
        Qt6::Core
        Qt6::Xml
        Clipper2
    PRIVATE
        Qt6::Gui
        Qt6::Svg
        Qt6::PrintSupport
        ZLIB::ZLIB
)

# ══════════════════════════════════════════════════════════════════════════
# Outils — ne dépendent que du noyau, donc déclarés ici
# ══════════════════════════════════════════════════════════════════════════
find_package(Qt6 REQUIRED COMPONENTS Widgets SvgWidgets)

# svg2ocgpdf ne recompile PLUS src/pdf/SvgToOcgPdf.cpp en dur (849 lignes
# étaient compilées deux fois) : il prend les objets de libTrame.a.
add_executable(svg2ocgpdf tools/svg2ocgpdf.cpp)
target_link_libraries(svg2ocgpdf PRIVATE Trame Qt6::Widgets)

# pdfpayload : le bout récepteur du PDF hybride, exercable hors des deux applications.
# Il ne sert qu'à prouver l'aller-retour — écrire une charge utile, la relire à l'identique.
add_executable(pdfpayload tools/pdfpayload.cpp)
target_link_libraries(pdfpayload PRIVATE Trame)

add_executable(svg2pdf tests/svg2pdf.cpp)
target_link_libraries(svg2pdf PRIVATE
    Qt6::Widgets Qt6::Svg Qt6::SvgWidgets Qt6::Xml Qt6::PrintSupport)

# Harnais headless de non-régression — rejoue le pipeline complet sans UI.
# C'est le seul moyen de vérifier nesting/ + io/ : l'appli est une GUI sans
# arguments, et il n'existe aucun test automatisé dans ce dépôt.
add_executable(nestbench tests/nestbench.cpp)
target_link_libraries(nestbench PRIVATE Trame)

# Instrument de mesure — il ne vérifie rien, il chiffre : rejoue un nesting et
# mesure le dégagement réellement obtenu entre les contours posés, au lieu de
# le déduire de la lecture du moteur. Voir tests/gapbench.cpp.
add_executable(gapbench tests/gapbench.cpp)
target_link_libraries(gapbench PRIVATE Trame)
