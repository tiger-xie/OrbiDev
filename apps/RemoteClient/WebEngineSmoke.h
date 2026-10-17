#pragma once
class QApplication;
class QString;
// 保留独立 WebEngine 资源烟测，不将技术页当作产品主界面。
int runWebEngineSmoke(QApplication &application, const QString &screenshot);
