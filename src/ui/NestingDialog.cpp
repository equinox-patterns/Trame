/************************************************************************
 **
 **  @file   NestingDialog.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   26/05/2026
 **
 **  @brief  Temporary modal UI — import SVG, choose bin, run nesting, export SVG.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include "ui/NestingDialog.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QSettings>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

#include "io/NestedSvgWriter.h"
#include "io/SvgReader.h"
#include "nesting/Bin.h"
#include "nesting/Engine.h"
#include "nesting/Footprint.h"

// ── Pipeline result (packed for background thread) ─────────────────────────

struct NestingResult
{
    bool                     success       = false;
    QString                  error;
    Footprint::Result        fp;
    std::vector<Placement>   placements;
    double                   usedHeightMm  = 0;
    double                   elapsed       = 0;
    int                      totalPieces   = 0;
};

static NestingResult runPipeline(const QString &svgPath, double binWidthMm)
{
    NestingResult r;
    constexpr double kMmToPx = 96.0 / 25.4;

    // ── 1. Read SVG ───────────────────────────────────────────────────────
    QVector<SizeLayer> layers;
    SvgReader reader;
    if (!reader.read(svgPath, layers)) {
        r.error = QStringLiteral("Failed to parse SVG.");
        return r;
    }

    for (const auto &layer : layers)
        r.totalPieces += layer.pieces.size();

    // ── 2. Compute footprints ─────────────────────────────────────────────
    r.fp = Footprint::compute(layers);
    if (r.fp.polygons.empty()) {
        r.error = QStringLiteral("No footprints — nothing to nest.");
        return r;
    }

    // ── 3. Nesting ────────────────────────────────────────────────────────
    const double binWidthPx = binWidthMm * kMmToPx;
    const bool isA0 = (binWidthMm <= 841.0);
    const double obsWPx = isA0 ? 210.0 * kMmToPx : 0.0;
    const double obsHPx = isA0 ? 297.0 * kMmToPx : 0.0;
    Bin bin(binWidthPx, 5.0 * kMmToPx, obsWPx, obsHPx);

    Engine::Config cfg;
    cfg.gap = 5.0 * kMmToPx;
    Engine engine(cfg);

    const auto t0 = std::chrono::steady_clock::now();
    r.placements = engine.place(bin, r.fp.polygons);
    const auto t1 = std::chrono::steady_clock::now();
    r.elapsed = std::chrono::duration<double>(t1 - t0).count();

    // ── 4. Compute used height ────────────────────────────────────────────
    for (size_t i = 0; i < r.placements.size() && i < r.fp.polygons.size(); ++i) {
        int64_t xMin = (std::numeric_limits<int64_t>::max)();
        int64_t yMin = (std::numeric_limits<int64_t>::max)();
        for (const auto &pt : r.fp.polygons[i]) {
            if (pt.x < xMin) xMin = pt.x;
            if (pt.y < yMin) yMin = pt.y;
        }

        Clipper2Lib::Path64 local;
        local.reserve(r.fp.polygons[i].size());
        for (const auto &pt : r.fp.polygons[i])
            local.push_back({pt.x - xMin, pt.y - yMin});

        Clipper2Lib::Path64 rotated;
        if (r.placements[i].rotation != 0.0) {
            const double rad = r.placements[i].rotation * M_PI / 180.0;
            const double c   = std::cos(rad);
            const double s   = std::sin(rad);
            rotated.reserve(local.size());
            for (const auto &pt : local) {
                rotated.push_back({
                    static_cast<int64_t>(std::round(pt.x * c - pt.y * s)),
                    static_cast<int64_t>(std::round(pt.x * s + pt.y * c))
                });
            }
        } else {
            rotated = std::move(local);
        }

        int64_t ryMax = (std::numeric_limits<int64_t>::min)();
        for (const auto &pt : rotated) {
            if (pt.y > ryMax) ryMax = pt.y;
        }

        const double bottom = r.placements[i].y + ryMax / 1e6;
        if (bottom > r.usedHeightMm)
            r.usedHeightMm = bottom;
    }
    r.usedHeightMm += bin.marginMm();
    r.usedHeightMm *= 25.4 / 96.0;
    if (r.usedHeightMm < 100) r.usedHeightMm = 100;
    // Ensure height accounts for obstacle at top
    const double obstacleBottomMm = (bin.marginMm() + bin.obstacleHeightMm()) * 25.4 / 96.0;
    if (r.usedHeightMm < obstacleBottomMm) r.usedHeightMm = obstacleBottomMm;

    r.success = true;
    return r;
}

// ── NestingDialog ───────────────────────────────────────────────────────────

NestingDialog::NestingDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Rabnest — Nesting"));
    setMinimumWidth(520);

    QSettings settings;
    auto *mainLayout = new QVBoxLayout(this);

    // ── Input file ──────────────────────────────────────────────────────────
    auto *fileGroup = new QGroupBox(QStringLiteral("Input"), this);
    auto *fileLayout = new QHBoxLayout(fileGroup);
    m_inputFile = new QLineEdit(this);
    m_inputFile->setPlaceholderText(QStringLiteral("Select an SVG file..."));
    m_inputFile->setText(settings.value(QStringLiteral("lastInputFile")).toString());
    auto *browseInBtn = new QPushButton(QStringLiteral("Browse..."), this);
    fileLayout->addWidget(m_inputFile, 1);
    fileLayout->addWidget(browseInBtn);
    mainLayout->addWidget(fileGroup);

    // ── Output directory ────────────────────────────────────────────────────
    auto *outGroup = new QGroupBox(QStringLiteral("Output"), this);
    auto *outLayout = new QHBoxLayout(outGroup);
    m_outputDir = new QLineEdit(this);
    m_outputDir->setPlaceholderText(QStringLiteral("Export directory..."));
    m_outputDir->setText(settings.value(QStringLiteral("lastExportDir")).toString());
    auto *browseOutBtn = new QPushButton(QStringLiteral("Browse..."), this);
    outLayout->addWidget(m_outputDir, 1);
    outLayout->addWidget(browseOutBtn);
    mainLayout->addWidget(outGroup);

    // ── Bin size ────────────────────────────────────────────────────────────
    auto *binGroup = new QGroupBox(QStringLiteral("Bin"), this);
    auto *binLayout = new QFormLayout(binGroup);
    m_binSize = new QComboBox(this);
    m_binSize->addItem(QStringLiteral("841 mm (A0)"), 841.0);
    m_binSize->addItem(QStringLiteral("950 mm"), 950.0);
    binLayout->addRow(QStringLiteral("Width:"), m_binSize);
    mainLayout->addWidget(binGroup);

    // ── Progress bar ────────────────────────────────────────────────────────
    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);
    m_progress->setRange(0, 0); // indeterminate
    mainLayout->addWidget(m_progress);

    // ── Run button ──────────────────────────────────────────────────────────
    m_runButton = new QPushButton(QStringLiteral("Run Nesting"), this);
    mainLayout->addWidget(m_runButton);

    // ── Status ──────────────────────────────────────────────────────────────
    m_status = new QLabel(QStringLiteral("Ready."), this);
    m_status->setWordWrap(true);
    mainLayout->addWidget(m_status);

    mainLayout->addStretch();

    // ── Close ───────────────────────────────────────────────────────────────
    auto *closeBtn = new QPushButton(QStringLiteral("Close"), this);
    mainLayout->addWidget(closeBtn);

    // ── Connections ─────────────────────────────────────────────────────────
    connect(browseInBtn, &QPushButton::clicked, this, &NestingDialog::browseInputFile);
    connect(browseOutBtn, &QPushButton::clicked, this, &NestingDialog::browseOutputDir);
    connect(m_runButton, &QPushButton::clicked, this, &NestingDialog::runNesting);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
}

void NestingDialog::browseInputFile()
{
    QSettings settings;
    const QString lastDir = settings.value(QStringLiteral("lastInputDir")).toString();
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Open SVG file"),
        lastDir,
        QStringLiteral("SVG files (*.svg);;All files (*)"));
    if (path.isEmpty())
        return;
    m_inputFile->setText(path);
    settings.setValue(QStringLiteral("lastInputFile"), path);
    settings.setValue(QStringLiteral("lastInputDir"), QFileInfo(path).absolutePath());

    // Auto-fill output dir from input file's directory if not already set
    if (m_outputDir->text().isEmpty()) {
        const QString dir = QFileInfo(path).absolutePath();
        m_outputDir->setText(dir);
        settings.setValue(QStringLiteral("lastExportDir"), dir);
    }
}

void NestingDialog::browseOutputDir()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select export directory"),
        m_outputDir->text());
    if (!dir.isEmpty())
        updateOutputDir(dir);
}

void NestingDialog::updateOutputDir(const QString &path)
{
    m_outputDir->setText(path);
    QSettings().setValue(QStringLiteral("lastExportDir"), path);
}

void NestingDialog::runNesting()
{
    const QString svgPath = m_inputFile->text();
    if (svgPath.isEmpty()) {
        m_status->setText(QStringLiteral("Please select an SVG file first."));
        return;
    }

    const QFileInfo fi(svgPath);
    if (!fi.exists()) {
        m_status->setText(QStringLiteral("File not found: ") + svgPath);
        return;
    }

    const double binWidthMm = m_binSize->currentData().toDouble();

    m_runButton->setEnabled(false);
    m_progress->setVisible(true);
    m_status->setText(QStringLiteral("Nesting in progress (may take 60+ seconds)..."));

    // ── Run pipeline in background thread ────────────────────────────────────
    auto *watcher = new QFutureWatcher<NestingResult>(this);
    connect(watcher, &QFutureWatcher<NestingResult>::finished, this,
        [this, watcher, svgPath, binWidthMm, fi]()
    {
        NestingResult r = watcher->result();
        watcher->deleteLater();

        m_progress->setVisible(false);

        if (!r.success) {
            m_status->setText(r.error);
            m_runButton->setEnabled(true);
            return;
        }

        // ── Save dialog in persisted or input-file directory ─────────────────
        QString exportDir = m_outputDir->text();
        if (exportDir.isEmpty())
            exportDir = fi.absolutePath();

        // Persist the directory
        QSettings().setValue(QStringLiteral("lastExportDir"), exportDir);

        const QString baseName = fi.completeBaseName();
        const QString suffix = (binWidthMm <= 841.0)
            ? QStringLiteral("_nestingA0") : QStringLiteral("_nesting");
        const QString defaultPath = exportDir + QLatin1Char('/')
            + baseName + suffix + QStringLiteral(".svg");
        const QString outputSvg = QFileDialog::getSaveFileName(
            this,
            QStringLiteral("Save nested SVG"),
            defaultPath,
            QStringLiteral("SVG files (*.svg);;All files (*)"));

        if (outputSvg.isEmpty()) {
            m_status->setText(QStringLiteral("Cancelled."));
            m_runButton->setEnabled(true);
            return;
        }

        // Persist the chosen directory
        updateOutputDir(QFileInfo(outputSvg).absolutePath());

        // ── Write nested SVG ──────────────────────────────────────────────
        m_status->setText(QStringLiteral("Writing nested SVG..."));
        QApplication::processEvents();

        NestedSvgWriter writer;
        if (!writer.write(svgPath, outputSvg, r.placements, r.fp,
                         binWidthMm, r.usedHeightMm)) {
            m_status->setText(QStringLiteral("Failed to write nested SVG."));
            m_runButton->setEnabled(true);
            return;
        }

        m_status->setText(QStringLiteral("Done in %1 s — %2 footprints, %3 pieces, height %4 mm — %5")
                              .arg(r.elapsed, 0, 'f', 1)
                              .arg(r.fp.polygons.size())
                              .arg(r.totalPieces)
                              .arg(r.usedHeightMm, 0, 'f', 0)
                              .arg(outputSvg));
        m_runButton->setEnabled(true);
    });

    QFuture<NestingResult> future = QtConcurrent::run(&runPipeline, svgPath, binWidthMm);
    watcher->setFuture(future);
}
