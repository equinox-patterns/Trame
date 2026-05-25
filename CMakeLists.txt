cmake_minimum_required(VERSION 3.15)
project(Rabnest VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Qt 6 ───────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Widgets Xml Svg SvgWidgets PrintSupport)

# ── Clipper2 ───────────────────────────────────────────────────────────────
set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/clipper2/CPP EXCLUDE_FROM_ALL)

# ── Rabnest sources ─────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/io/RabnestReader.cpp
    src/io/SvgReader.cpp
    src/nesting/Bin.cpp
    src/nesting/Engine.cpp
    src/nesting/Footprint.cpp
    src/nesting/SizeRestorer.cpp
    src/pdf/PdfExporter.cpp
)

set(HEADERS
    src/model/Piece.h
    src/model/GradedSize.h
    src/model/Placement.h
    src/model/TransformedPiece.h
    src/io/RabnestReader.h
    src/io/SvgReader.h
    src/nesting/Bin.h
    src/nesting/Engine.h
    src/nesting/Footprint.h
    src/nesting/SizeRestorer.h
    src/pdf/PdfExporter.h
)

add_executable(Rabnest ${SOURCES} ${HEADERS})

target_include_directories(Rabnest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(Rabnest PRIVATE
    Qt6::Widgets
    Qt6::Xml
    Qt6::Svg
    Clipper2
)

# ── Test utilities ───────────────────────────────────────────────────────
find_package(ZLIB REQUIRED)

add_executable(svg2pdf tests/svg2pdf.cpp)
target_link_libraries(svg2pdf PRIVATE Qt6::Widgets Qt6::Svg Qt6::SvgWidgets Qt6::Xml Qt6::PrintSupport)

add_executable(svg2ocgpdf
    tests/svg2ocgpdf.cpp
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
