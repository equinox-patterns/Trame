cmake_minimum_required(VERSION 3.15)
project(Rabnest VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)

# ── Qt 6 ───────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Widgets Xml Svg SvgWidgets PrintSupport)

# ── Clipper2 ───────────────────────────────────────────────────────────────
set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/clipper2/CPP EXCLUDE_FROM_ALL)

# ── ZLIB ───────────────────────────────────────────────────────────────────
find_package(ZLIB REQUIRED)

# ── Rabnest CLI ───────────────────────────────────────────────────────────
set(RABNEST_SOURCES
    src/main.cpp
    src/io/NestedSvgWriter.cpp
    src/io/SvgReader.cpp
    src/nesting/Bin.cpp
    src/nesting/Engine.cpp
    src/nesting/Footprint.cpp
    src/pdf/SvgToOcgPdf.cpp
    src/ui/NestingDialog.cpp
)

set(RABNEST_HEADERS
    src/model/Piece.h
    src/model/GradedSize.h
    src/model/Placement.h
    src/io/NestedSvgWriter.h
    src/io/SvgReader.h
    src/nesting/Bin.h
    src/nesting/Engine.h
    src/nesting/Footprint.h
    src/pdf/SvgToOcgPdf.h
    src/pdf/PdfBuilder.h
    src/ui/NestingDialog.h
)

add_executable(Rabnest ${RABNEST_SOURCES} ${RABNEST_HEADERS})

target_include_directories(Rabnest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(Rabnest PRIVATE
    Qt6::Widgets
    Qt6::Xml
    Qt6::Svg
    Qt6::PrintSupport
    Clipper2
    ZLIB::ZLIB
)

# ── Tools ─────────────────────────────────────────────────────────────────

add_executable(svg2ocgpdf
    tools/svg2ocgpdf.cpp
    src/pdf/SvgToOcgPdf.cpp
)
target_include_directories(svg2ocgpdf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(svg2ocgpdf PRIVATE
    Qt6::Widgets
    Qt6::Svg
    Qt6::Xml
    Qt6::PrintSupport
    ZLIB::ZLIB
)

# ── Test utilities ─────────────────────────────────────────────────────────

add_executable(svg2pdf tests/svg2pdf.cpp)
target_link_libraries(svg2pdf PRIVATE Qt6::Widgets Qt6::Svg Qt6::SvgWidgets Qt6::Xml Qt6::PrintSupport)
