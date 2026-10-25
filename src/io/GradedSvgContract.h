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

/**
 * @brief Role marker: what a layer IS, when it is neither a size nor the decor.
 *
 * Until it existed, a layered document only knew two roles — `ostara:layout="yes"` for the
 * decor, and "everything else is a size". The projector needs three more things on the page
 * that are not sizes: a full-page background and two calibration grids. Without a role they
 * would become sizes: listed in the reader's panel as sizes, re-nested by Rabnest as sizes,
 * and painted above the pieces instead of underneath — a magenta sheet would hide the plan.
 *
 * The values are the two roles the writer emits today. A layer that carries neither marker
 * and no `ostara:layout` is a size, exactly as before: the attribute is optional, and an
 * older file keeps its old meaning.
 */
static constexpr char kGradedRoleAttr[] = "ostara:role";
static constexpr char kGradedRoleBackground[] = "background";
static constexpr char kGradedRoleCalibration[] = "calibration";

#endif // GRADEDSVGCONTRACT_H
