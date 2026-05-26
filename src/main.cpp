/************************************************************************
 **
 **  @file   main.cpp
 **  @author Sophie Denys <contact@sophiedenys.com>
 **  @date   23/05/2026
 **
 **  @brief  Rabnest — SVG nesting tool for sewing patterns.
 **
 **  @copyright
 **  Copyright (C) 2026 Sophie Denys <contact@sophiedenys.com>
 **  All rights reserved.
 **
 *************************************************************************/

#include <QApplication>

#include "ui/NestingDialog.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Rabnest"));
    QApplication::setApplicationVersion(QStringLiteral("0.2.0"));

    NestingDialog dlg;
    dlg.exec();

    return 0;
}
