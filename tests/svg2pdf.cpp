#include <QApplication>
#include <QPdfWriter>
#include <QPainter>
#include <QSvgRenderer>
#include <QPageSize>
#include <QMarginsF>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    if (argc < 2) {
        qWarning() << "Usage: svg2pdf <file.svg>";
        return 1;
    }

    const QString svgPath = QString::fromLocal8Bit(argv[1]);
    const QFileInfo fi(svgPath);
    const QString pdfPath = fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(".pdf");

    QDomDocument svgDoc;
    QFile svgFile(svgPath);
    svgFile.open(QIODevice::ReadOnly);
    svgDoc.setContent(&svgFile);
    svgFile.close();

    const QDomElement svgEl = svgDoc.documentElement();
    auto mmFromAttr = [](const QString &s) -> qreal {
        if (s.endsWith(QStringLiteral("mm"))) return s.chopped(2).toDouble();
        return s.toDouble() * 25.4 / 96.0;
    };
    const qreal pageWmm = mmFromAttr(svgEl.attribute(QStringLiteral("width")));
    const qreal pageHmm = mmFromAttr(svgEl.attribute(QStringLiteral("height")));
    const QStringList vb = svgEl.attribute(QStringLiteral("viewBox")).split(QLatin1Char(' '));
    const QRectF viewBox(vb.value(0).toDouble(), vb.value(1).toDouble(),
                         vb.value(2).toDouble(), vb.value(3).toDouble());

    QSvgRenderer renderer(svgPath);

    QPdfWriter writer(pdfPath);
    writer.setResolution(96);
    writer.setPageSize(QPageSize(QSizeF(pageWmm, pageHmm), QPageSize::Millimeter));
    writer.setPageMargins(QMarginsF(0, 0, 0, 0));

    QPainter painter(&writer);
    renderer.render(&painter, viewBox);
    painter.end();

    qWarning() << "PDF exported:" << pdfPath;
    return 0;
}
