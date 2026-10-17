#pragma once

#include <QString>
class QApplication;
class QSettings;
namespace rad::client
{
/// 应用唯一内置主题并保存标识。未知标识回退 dark，不触碰会话状态。
QString applyTheme(QApplication &application, QSettings &settings);
}
