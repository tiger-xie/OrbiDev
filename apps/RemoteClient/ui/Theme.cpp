#include "Theme.h"

#include <QApplication>
#include <QDebug>
#include <QPalette>
#include <QSettings>

namespace rad::client
{
QString applyTheme(QApplication &application, QSettings &settings)
{
    const auto requested =
        settings.value(QStringLiteral("appearance/theme"), QStringLiteral("dark")).toString();
    if (requested != QStringLiteral("dark"))
        qWarning("Unknown client theme; using built-in dark theme.");
    settings.setValue(QStringLiteral("appearance/theme"), QStringLiteral("dark"));
    application.setStyle(QStringLiteral("Fusion"));
    QPalette palette;
    palette.setColor(QPalette::Window, QColor("#24272c"));
    palette.setColor(QPalette::WindowText, QColor("#e6e8eb"));
    palette.setColor(QPalette::Base, QColor("#181b20"));
    palette.setColor(QPalette::AlternateBase, QColor("#2b3036"));
    palette.setColor(QPalette::Text, QColor("#e6e8eb"));
    palette.setColor(QPalette::Button, QColor("#2b3036"));
    palette.setColor(QPalette::ButtonText, QColor("#e6e8eb"));
    palette.setColor(QPalette::Highlight, QColor("#33494b"));
    palette.setColor(QPalette::HighlightedText, QColor("#e6e8eb"));
    palette.setColor(QPalette::ToolTipBase, QColor("#2b3036"));
    palette.setColor(QPalette::ToolTipText, QColor("#e6e8eb"));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor("#7e8790"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#7e8790"));
    application.setPalette(palette);
    application.setStyleSheet(QStringLiteral(R"(
QWidget { font-family: "Microsoft YaHei UI"; font-size: 14px; }
QMenuBar, QMenu, QToolBar, QStatusBar { background: #2b3036; color: #e6e8eb; }
QMenuBar { padding: 2px 12px; border-bottom: 1px solid #41474f; }
QMenuBar::item { padding: 5px 10px; }
QMenuBar::item:selected, QMenu::item:selected { background: #33494b; }
QMenu::item { padding: 7px 28px; }
QToolBar { spacing: 12px; padding: 10px 18px; border-bottom: 1px solid #41474f; }
QPushButton, QComboBox, QLineEdit { border: 1px solid #41474f; border-radius: 3px; padding: 6px 12px; min-height: 20px; background: #2b3036; color: #e6e8eb; }
QPushButton:hover, QComboBox:hover { background: #374047; border-color: #77818c; }
QPushButton:pressed { background: #33494b; }
QPushButton:focus, QComboBox:focus, QLineEdit:focus { border: 1px solid #32b8ad; }
QPushButton:disabled, QComboBox:disabled, QLineEdit:disabled { color: #7e8790; background: #24272c; border-color: #353b43; }
QPushButton[role="primary"] { background: #32b8ad; color: #102725; border-color: #32b8ad; font-weight: 600; }
QPushButton[role="primary"]:hover { background: #50c9be; }
QPushButton[role="primary"]:pressed { background: #26958d; }
QPushButton[role="primary"]:disabled { background: #344b4c; color: #9aa9ab; border-color: #41474f; }
QPushButton[role="danger"] { color: #ee8585; }
QPushButton[role="danger"]:disabled { color: #7e8790; }
QTreeView { background: #24272c; border: none; padding: 6px; show-decoration-selected: 1; }
QTreeView::item { padding: 8px 4px; border-left: 3px solid transparent; }
QTreeView::item:hover { background: #2b3036; }
QTreeView::item:selected { background: #33494b; border-left: 3px solid #32b8ad; color: #e6e8eb; }
QTreeView:focus { border: 1px solid #32b8ad; }
QSplitter::handle { background: #41474f; width: 1px; }
QTabWidget::pane { border: none; background: #181b20; }
QTabBar::tab { background: #24272c; padding: 10px 18px; border-right: 1px solid #41474f; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { background: #2b3036; border-bottom-color: #32b8ad; }
QTabBar::tab:hover { background: #374047; }
QToolButton#sessionTabCloseButton { color: #e6e8eb; background: transparent; border: 1px solid transparent; border-radius: 3px; font-size: 18px; padding: 0px 4px; }
QToolButton#sessionTabCloseButton:hover { background: #653b42; color: #ffffff; }
QToolButton#sessionTabCloseButton:pressed { background: #80454d; }
QToolButton#sessionTabCloseButton:focus { border-color: #32b8ad; }
QLabel[role="secondary"] { color: #adb4bd; }
QLabel[role="heading"] { font-size: 20px; font-weight: 500; }
QLabel[role="error"] { color: #ee8585; padding: 8px 14px; background: #35292d; }
QLabel[role="section"] { color: #adb4bd; padding: 16px 18px 8px 18px; }
QWidget#sessionPage, QWidget#emptyPage { background: #181b20; }
QWidget#contextBar { background: #2b3036; border-bottom: 1px solid #41474f; }
QStatusBar { border-top: 1px solid #41474f; padding: 4px 14px; }
QStatusBar::item { border: none; }
QToolTip { border: 1px solid #77818c; padding: 6px; }
QScrollBar:vertical { background: #24272c; width: 12px; }
QScrollBar::handle:vertical { background: #66717e; min-height: 24px; }
)"));
    return QStringLiteral("dark");
}
}
