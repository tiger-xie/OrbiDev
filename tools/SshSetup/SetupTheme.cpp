#include "SetupTheme.h"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QPalette>

// 静态库显式初始化根资源，防止链接器裁剪未引用的资源对象。
static void initializeOrbiDevResources()
{
    Q_INIT_RESOURCE(OrbiDev);
}

namespace sshsetup
{
void applySetupTheme(QApplication &application)
{
    initializeOrbiDevResources();
    application.setWindowIcon(QIcon(QStringLiteral(":/resources/icon/common/app.svg")));
    application.setStyle(QStringLiteral("Fusion"));
    application.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#141b21"));
    palette.setColor(QPalette::WindowText, QColor("#edf2f6"));
    palette.setColor(QPalette::Base, QColor("#151e25"));
    palette.setColor(QPalette::Text, QColor("#edf2f6"));
    palette.setColor(QPalette::PlaceholderText, QColor("#91a3b0"));
    palette.setColor(QPalette::Button, QColor("#26323c"));
    palette.setColor(QPalette::ButtonText, QColor("#edf2f6"));
    palette.setColor(QPalette::Highlight, QColor("#176f73"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    application.setPalette(palette);
    QString style;
    for (const auto *name : {"base", "window", "setup", "workbench"})
    {
        QFile file(QStringLiteral(":/resources/style/%1.qss").arg(QString::fromLatin1(name)));
        if (!file.open(QIODevice::ReadOnly))
            qFatal("OrbiDev 样式资源缺失，无法初始化界面。");
        style += QString::fromUtf8(file.readAll()) + QLatin1Char('\n');
    }
    application.setStyleSheet(style);
}
}
