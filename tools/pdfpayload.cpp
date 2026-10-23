/************************************************************************
 **
 **  @file   pdfpayload.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   22/10/2026
 **
 **  @brief  Read the payload a PDF carries for Rabnest, and hand it back.
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

#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QTextStream>

#include "pdf/PdfPayload.h"

/// The receiving end of the hybrid PDF, exercisable outside both applications.
///
/// It exists for the same reason svg2ocgpdf does: a path that only one caller can reach is
/// a path nothing proves. Here the round trip is ordinary — svg2ocgpdf writes the payload,
/// this reads it back, and the two files being equal is the whole test.
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QStringList args;
    args.reserve(argc);
    for (int i = 1; i < argc; ++i)
        args.append(QString::fromLocal8Bit(argv[i]));

    QTextStream out(stdout);
    QTextStream err(stderr);

    if (args.isEmpty()) {
        err << "Usage: pdfpayload <file.pdf> [output]\n";
        return 1;
    }

    const QString pdfPath = args.at(0);
    const QString outPath = args.size() >= 2 ? args.at(1) : QString();

    int version = 0;
    QString error;
    const QByteArray payload = PdfPayload::read(pdfPath, &version, &error);

    if (payload.isEmpty()) {
        err << (error.isEmpty() ? QStringLiteral("No payload.") : error) << "\n";
        return 1;
    }

    out << "Version: " << version << "\n";
    out << "Payload: " << payload.size() << " bytes\n";

    if (!outPath.isEmpty()) {
        QFile file(outPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            err << "Cannot write " << outPath << "\n";
            return 1;
        }
        file.write(payload);
        file.close();
        out << "Written: " << outPath << "\n";
    }

    return 0;
}
