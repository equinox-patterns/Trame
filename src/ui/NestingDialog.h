/************************************************************************
 **
 **  @file   NestingDialog.h
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

#ifndef NESTINGDIALOG_H
#define NESTINGDIALOG_H

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>

class NestingDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NestingDialog(QWidget *parent = nullptr);

private slots:
    void browseInputFile();
    void browseOutputDir();
    void runNesting();

private:
    void updateOutputDir(const QString &path);

    QLineEdit   *m_inputFile  = nullptr;
    QLineEdit   *m_outputDir  = nullptr;
    QComboBox   *m_binSize    = nullptr;
    QPushButton *m_runButton  = nullptr;
    QProgressBar *m_progress  = nullptr;
    QLabel      *m_status     = nullptr;
};

#endif // NESTINGDIALOG_H
