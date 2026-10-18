#include "SetupTheme.h"

#include <QApplication>
#include <QFont>
#include <QPalette>

namespace sshsetup
{
void applySetupTheme(QApplication &application)
{
    application.setStyle(QStringLiteral("Fusion"));
    application.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#20262c"));
    palette.setColor(QPalette::WindowText, QColor("#edf1f4"));
    palette.setColor(QPalette::Base, QColor("#181e23"));
    palette.setColor(QPalette::Text, QColor("#edf1f4"));
    palette.setColor(QPalette::Button, QColor("#29323a"));
    palette.setColor(QPalette::ButtonText, QColor("#edf1f4"));
    palette.setColor(QPalette::Highlight, QColor("#237c7a"));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    application.setPalette(palette);
    application.setStyleSheet(QStringLiteral(R"(
        QWidget { color: #edf1f4; }
        QWidget#stepRail { background: #242d35; border-right: 1px solid #36414a; }
        QLabel[role="title"] { font-size: 24px; font-weight: 600; }
        QLabel[role="muted"] { color: #aab8c3; }
        QLabel[role="step"] { padding: 18px 12px; color: #aab8c3; }
        QLabel[role="currentStep"] { padding: 18px 12px; background: #29454a; color: #f5ffff; border-left: 3px solid #38c4bb; }
        QLabel#feedback { padding: 12px; background: #29353d; border: 1px solid #42545c; border-radius: 4px; }
        QLabel#feedback[error="true"] { background: #3a292c; color: #ffb9ad; border-color: #81534e; }
        QLabel[role="section"] { font-size: 15px; font-weight: 600; }
        QLineEdit, QPlainTextEdit, QSpinBox { background: #192127; border: 1px solid #4b5b67; border-radius: 4px; padding: 8px; selection-background-color: #237c7a; }
        QLineEdit:focus, QPlainTextEdit:focus, QSpinBox:focus { border-color: #3fc7be; }
        QPushButton { background: #2c3740; border: 1px solid #566572; border-radius: 4px; padding: 9px 17px; }
        QPushButton:hover { background: #354650; border-color: #7d96a5; }
        QPushButton:focus { border: 2px solid #80e3da; }
        QPushButton[role="primary"] { background: #25b5ad; color: #092b2d; border-color: #25b5ad; font-weight: 600; }
        QPushButton[role="primary"]:hover { background: #42cbc2; }
        QPushButton:disabled { background: #293238; color: #798891; border-color: #39464d; }
        QCheckBox { spacing: 9px; padding: 6px 0px; }
        QCheckBox::indicator { width: 17px; height: 17px; }
        QScrollArea { border: none; background: transparent; }
        QToolTip { background: #29353d; border: 1px solid #647681; color: #ffffff; }
    )"));
}
}
