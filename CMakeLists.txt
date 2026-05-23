cmake_minimum_required(VERSION 3.15)
project(NestingApp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Qt 6 ───────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Widgets Xml)

# ── Clipper2 ───────────────────────────────────────────────────────────────
set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/clipper2/CPP EXCLUDE_FROM_ALL)

# ── NestingApp sources ─────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/io/SvgReader.cpp
)

set(HEADERS
    src/model/Piece.h
    src/model/GradedSize.h
    src/io/SvgReader.h
)

add_executable(NestingApp ${SOURCES} ${HEADERS})

target_include_directories(NestingApp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(NestingApp PRIVATE
    Qt6::Widgets
    Qt6::Xml
    Clipper2
)
