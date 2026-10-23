/************************************************************************
 **
 **  @file   PdfPayload.cpp
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

#include "pdf/PdfPayload.h"

#include <QDebug>
#include <QFile>

#include <zlib.h>

#include "pdf/PdfBuilder.h"

namespace
{
/// The decimal number that follows @p key in @p hay, or -1 when there is none.
///
/// Deliberately tiny: the dictionaries this reads are the ones this module writes, and a
/// number where a number is expected is not a parser's job to recover from.
int numberAfter(const QByteArray &hay, const char *key)
{
    const int at = hay.indexOf(key);
    if (at < 0)
        return -1;

    int i = at + int(qstrlen(key));
    while (i < hay.size() && (hay.at(i) == ' ' || hay.at(i) == '\t'))
        ++i;

    const int start = i;
    while (i < hay.size() && hay.at(i) >= '0' && hay.at(i) <= '9')
        ++i;

    if (i == start)
        return -1;

    return hay.mid(start, i - start).toInt();
}

/// The offset of the object named @p num, or -1.
///
/// The leading delimiter is checked because `42 0 obj` is a substring of `142 0 obj`, and
/// jumping into the wrong object would hand a reader the middle of someone else's stream.
int objectAt(const QByteArray &pdf, int num)
{
    const QByteArray needle = QByteArray::number(num) + " 0 obj";
    int from = 0;
    while (true) {
        const int at = pdf.indexOf(needle, from);
        if (at < 0)
            return -1;
        if (at == 0 || pdf.at(at - 1) == '\n' || pdf.at(at - 1) == '\r')
            return at;
        from = at + 1;
    }
}
}  // namespace

int PdfPayload::write(PdfBuilder &pdf, const QByteArray &payload)
{
    if (payload.isEmpty())
        return 0;

    // qCompress writes a four-byte big-endian size in front of the zlib stream; a PDF's
    // /FlateDecode wants the stream alone. Dropping the prefix is the whole conversion —
    // what is left is a zlib stream, header and checksum included, which is exactly what
    // the filter says it is.
    QByteArray compressed = qCompress(payload, 9);
    if (compressed.size() <= 4)
        return 0;
    compressed.remove(0, 4);

    const int num = pdf.alloc();
    pdf.add(num, QStringLiteral("<< /Length %1 /Filter /FlateDecode >>\nstream\n")
                     .arg(compressed.size())
                     .toUtf8()
                 + compressed
                 + "\nendstream");

    return num;
}

QByteArray PdfPayload::pageEntry(int payloadObj)
{
    if (payloadObj <= 0)
        return {};

    return QStringLiteral("   /PieceInfo << /%1 << /Version %2 /Payload %3 0 R >> >>\n")
        .arg(QString::fromLatin1(kPrivateKey))
        .arg(kVersion)
        .arg(payloadObj)
        .toUtf8();
}

QByteArray PdfPayload::inflateStream(const QByteArray &compressed)
{
    if (compressed.isEmpty())
        return {};

    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        qWarning() << "PdfPayload: inflateInit failed";
        return {};
    }

    zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.constData()));
    zs.avail_in = static_cast<uInt>(compressed.size());

    QByteArray out;
    char buffer[64 * 1024];
    int ret = Z_OK;

    while (ret != Z_STREAM_END) {
        zs.next_out = reinterpret_cast<Bytef *>(buffer);
        zs.avail_out = sizeof(buffer);

        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            qWarning() << "PdfPayload: inflate failed with" << ret;
            inflateEnd(&zs);
            return {};
        }

        out.append(buffer, int(sizeof(buffer) - zs.avail_out));

        // No input left and no end marker: the stream was cut short. Handing back what was
        // decoded so far would be a payload that looks complete and is not.
        if (ret == Z_OK && zs.avail_in == 0 && zs.avail_out != 0) {
            qWarning() << "PdfPayload: truncated stream";
            inflateEnd(&zs);
            return {};
        }
    }

    inflateEnd(&zs);
    return out;
}

QByteArray PdfPayload::readFrom(const QByteArray &pdfBytes, int *versionOut, QString *errorOut)
{
    const auto fail = [errorOut](const QString &why) {
        if (errorOut != nullptr)
            *errorOut = why;
        return QByteArray();
    };

    if (versionOut != nullptr)
        *versionOut = 0;

    // A page's /PieceInfo is a small dictionary and the private key is inside it. The window
    // keeps a stray "/PieceInfo" from another producer — Illustrator writes one too — from
    // sending us to a key we do not own.
    int pieceAt = -1;
    int from = 0;
    while (true) {
        const int at = pdfBytes.indexOf("/PieceInfo", from);
        if (at < 0)
            break;
        const int windowEnd = qMin(at + 512, pdfBytes.size());
        const int keyAt = pdfBytes.indexOf('/' + QByteArray(kPrivateKey), at);
        if (keyAt >= 0 && keyAt < windowEnd) {
            pieceAt = at;
            break;
        }
        from = at + 10;
    }

    if (pieceAt < 0) {
        return fail(QStringLiteral("This PDF carries no %1 payload: it was not prepared "
                                   "for Rabnest.")
                        .arg(QString::fromLatin1(kPrivateKey)));
    }

    const QByteArray head = pdfBytes.mid(pieceAt, 512);

    const int version = numberAfter(head, "/Version");
    if (version <= 0)
        return fail(QStringLiteral("The payload declares no version."));
    if (versionOut != nullptr)
        *versionOut = version;
    if (version != kVersion) {
        return fail(QStringLiteral("The payload declares version %1; this reader understands "
                                   "version %2.")
                        .arg(version)
                        .arg(kVersion));
    }

    const int payloadObj = numberAfter(head, "/Payload");
    if (payloadObj <= 0)
        return fail(QStringLiteral("The payload dictionary names no stream object."));

    const int objAt = objectAt(pdfBytes, payloadObj);
    if (objAt < 0)
        return fail(QStringLiteral("The payload names object %1, which is not in the file.")
                        .arg(payloadObj));

    const int streamAt = pdfBytes.indexOf("stream", objAt);
    if (streamAt < 0)
        return fail(QStringLiteral("Object %1 is not a stream.").arg(payloadObj));

    const QByteArray dict = pdfBytes.mid(objAt, streamAt - objAt);

    int dataAt = streamAt + int(qstrlen("stream"));
    if (dataAt < pdfBytes.size() && pdfBytes.at(dataAt) == '\r')
        ++dataAt;
    if (dataAt < pdfBytes.size() && pdfBytes.at(dataAt) == '\n')
        ++dataAt;

    // /Length is read rather than searched for: the bytes of a compressed stream are binary,
    // and the word "endstream" is a shape they can take.
    int length = numberAfter(dict, "/Length");
    if (length < 0 || dataAt + length > pdfBytes.size()) {
        const int endAt = pdfBytes.indexOf("endstream", dataAt);
        if (endAt < 0)
            return fail(QStringLiteral("The payload stream has no end."));
        length = endAt - dataAt;
    }

    const QByteArray raw = pdfBytes.mid(dataAt, length);

    if (!dict.contains("/FlateDecode"))
        return raw;

    const QByteArray inflated = inflateStream(raw);
    if (inflated.isEmpty())
        return fail(QStringLiteral("The payload stream could not be decompressed."));

    return inflated;
}

QByteArray PdfPayload::read(const QString &pdfPath, int *versionOut, QString *errorOut)
{
    QFile file(pdfPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut != nullptr)
            *errorOut = QStringLiteral("Cannot open %1.").arg(pdfPath);
        return {};
    }

    const QByteArray bytes = file.readAll();
    file.close();

    return readFrom(bytes, versionOut, errorOut);
}
