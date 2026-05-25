/************************************************************************
 **
 **  @file   Bin.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Strip bin for nesting — fixed width, unbounded height.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef BIN_H
#define BIN_H

/// A strip bin with fixed width and unbounded height (strip packing).
///
/// The nesting engine places pieces freely in a continuous strip.
/// The height is not declared upfront — it is determined by the envelope
/// of placed pieces after nesting completes.
///
/// Artboards (for PDF export) are a post-processing grid overlaid on the
/// result — they are not part of the bin model.
class Bin
{
public:
    /// Construct a strip bin with the given width in millimetres.
    explicit Bin(double widthMm) noexcept;

    /// Strip width in millimetres.
    double widthMm() const noexcept { return m_widthMm; }

private:
    double m_widthMm;
};

#endif // BIN_H
