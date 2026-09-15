/************************************************************************
 **
 **  @file   PdfBuilder.h
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   25/05/2026
 **
 **  @brief  Minimal PDF-1.7 builder — writes objects, xref table, trailer.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#ifndef PDFBUILDER_H
#define PDFBUILDER_H

#include <QByteArray>
#include <QString>
#include <QVector>

#include <algorithm>

struct PdfObj
{
    int         num;
    QByteArray  body;
};

class PdfBuilder
{
public:
    QByteArray     header;
    QVector<PdfObj> objects;

    int nextNum = 1;

    /// The /Info object, when the caller wrote one. Zero leaves the trailer as it was —
    /// a PDF without an information dictionary is legal, it simply has nothing to say.
    int infoObj = 0;

    int alloc() { return nextNum++; }

    void add(int num, const QByteArray &body)
    {
        objects.append({num, body});
    }

    QByteArray build(int &xrefOffset)
    {
        std::sort(objects.begin(), objects.end(),
                  [](const PdfObj &a, const PdfObj &b) { return a.num < b.num; });

        QByteArray out;
        out.reserve(objects.size() * 1024);
        out.append(header);

        QVector<int> offsets;
        offsets.reserve(objects.size());

        for (const auto &obj : objects) {
            offsets.append(out.size());
            out.append(QStringLiteral("%1 0 obj\n").arg(obj.num).toUtf8());
            out.append(obj.body);
            if (!obj.body.endsWith('\n'))
                out.append('\n');
            out.append("endobj\n");
        }

        xrefOffset = out.size();

        out.append("xref\n");
        out.append(QStringLiteral("0 %1\n").arg(objects.size() + 1).toUtf8());
        out.append("0000000000 65535 f \n");
        for (const auto &off : offsets)
            out.append(QStringLiteral("%1 00000 n \n").arg(off, 10, 10, QLatin1Char('0')).toUtf8());

        const int catalogNum = objects.first().num;
        out.append("trailer\n");
        QByteArray trailer = QStringLiteral("<< /Size %1 /Root %2 0 R")
                                 .arg(objects.size() + 1).arg(catalogNum).toUtf8();
        if (infoObj > 0)
            trailer += QStringLiteral(" /Info %1 0 R").arg(infoObj).toUtf8();
        trailer += " >>\n";
        out.append(trailer);
        out.append("startxref\n");
        out.append(QStringLiteral("%1\n").arg(xrefOffset).toUtf8());
        out.append("%%EOF\n");

        return out;
    }
};

#endif // PDFBUILDER_H
