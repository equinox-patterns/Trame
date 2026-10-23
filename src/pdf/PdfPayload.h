/************************************************************************
 **
 **  @file   PdfPayload.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   22/10/2026
 **
 **  @brief  The payload a PDF carries for Rabnest — written under /PieceInfo,
 **          and read back by the same contract.
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

#ifndef PDFPAYLOAD_H
#define PDFPAYLOAD_H

#include <QByteArray>
#include <QString>

class PdfBuilder;

/// @brief What a "hybrid" PDF carries besides its pages, and how it is read back.
///
/// The PDF stays a PDF: a reader shows it, a printer prints it, and nothing in it says
/// "open me with something else". The payload is attached where PDF 32000-1 §14.5 says
/// service data belongs — /PieceInfo on a page — and NOT as an embedded file, which would
/// put a paperclip in the reader's panel. Adobe made the same choice for Illustrator.
///
/// The payload is opaque to this module: it writes the bytes it is given and hands them
/// back unchanged. What those bytes are — a graded SVG, a model, both — is the contract's
/// business, not the envelope's.
///
/// The envelope, as written:
///
///     /PieceInfo << /Trame << /Version 1 /Payload 42 0 R >> >>
///
/// `/Trame` names the private application key, `/Version` is the contract's own version —
/// a payload a reader does not understand must be refused, never drawn — and `/Payload`
/// is a Flate-compressed stream.
///
/// The reader below is deliberately narrow: it reads the files this module writes, and
/// refuses anything else with a message. It is not a PDF parser. It looks for the private
/// key in the file's text, jumps to the object the dictionary names, and inflates it. That
/// is enough for a document written by us, and it is not a pretence of handling a document
/// written by anyone else.
class PdfPayload
{
public:
    /// The version of the envelope, and the only one this reader claims to understand.
    ///
    /// A file that declares another version is not read: a payload whose shape changed
    /// under a reader that kept going would arrive as plausible nonsense.
    static constexpr int kVersion = 1;

    /// The private application key. It is a NAME the PDF addresses the payload by, so it
    /// carries no version — the version is a field beside it.
    static constexpr const char *kPrivateKey = "Trame";

    /// @brief Write @p payload as a compressed stream object in @p pdf.
    ///
    /// @return the object number to name in a page's /PieceInfo, or 0 when @p payload is
    /// empty — a PDF with no payload is a legal PDF, and it is the "closed" export.
    ///
    /// Allocates and adds in one step, like every other object in the PDF builder: the xref
    /// is positional, so a number taken and never written shifts everything after it.
    static int write(PdfBuilder &pdf, const QByteArray &payload);

    /// @brief The dictionary a page carries to point at @p payloadObj.
    ///
    /// Empty when @p payloadObj is 0, so a caller can append it unconditionally.
    static QByteArray pageEntry(int payloadObj);

    /// @brief Read the payload back out of a PDF held in memory.
    ///
    /// @param versionOut  receives the declared version, or 0 when none was declared.
    /// @param errorOut    receives a sentence a human can read, when the payload is refused.
    /// @return the payload, empty on failure — an empty payload is not a valid one.
    static QByteArray readFrom(const QByteArray &pdfBytes, int *versionOut = nullptr,
                               QString *errorOut = nullptr);

    /// @brief Same, from a file.
    static QByteArray read(const QString &pdfPath, int *versionOut = nullptr,
                           QString *errorOut = nullptr);

private:
    /// The zlib stream inside a PDF's /FlateDecode — no header to skip, no size to trust.
    static QByteArray inflateStream(const QByteArray &compressed);
};

#endif // PDFPAYLOAD_H
