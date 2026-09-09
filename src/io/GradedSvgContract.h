/************************************************************************
 **
 **  @file   GradedSvgContract.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   09/09/2026
 **
 **  @brief  Machine contract shared with the Ostara graded-SVG writer.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef GRADEDSVGCONTRACT_H
#define GRADEDSVGCONTRACT_H

/**
 * @brief Backend marker identifying the decor layer of a graded SVG.
 *
 * The decor layer's <g> carries ostara:layout="yes" — an attribute authored by
 * Ostara (namespace "ostara", declared on the <svg> root of the export). Rabnest
 * identifies the decor layer by this marker. It is only meaningful to the
 * Ostara → Rabnest pipeline; vector editors never display it, so the visible
 * layer name is independent and may be localized or user-edited (kept in the
 * id, inkscape:label and <title>). Keep these constants in sync with the writer
 * in Ostara (ogradedsvgwriter.cpp).
 */
static constexpr char kGradedLayoutLayerAttr[] = "ostara:layout";
static constexpr char kGradedLayoutLayerMarker[] = "yes";

#endif // GRADEDSVGCONTRACT_H
