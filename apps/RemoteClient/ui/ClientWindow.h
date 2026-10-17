#pragma once

#include <QMainWindow>

class QAction;
class QLabel;
class QPushButton;
class QTreeView;
class QTabWidget;
class QStandardItemModel;
class QSplitter;
class QStackedWidget;
namespace rad::client
{
class ClientController;
/// 只显示协调器状态；不直接操作 Agent、终端或系统进程。
class ClientWindow final : public QMainWindow
{
    Q_OBJECT
public:
    explicit ClientWindow(ClientController *controller, QWidget *parent = nullptr);
    QString currentSessionId() const;
    int sessionTabCount() const;
    void openSession(const QString &id);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupUi();
    void refreshUi();
    void updateContext();
    void showSession(const QString &id);
    void removeSession(const QString &id);
    void createSession();
    void closeSession(int index);
    void confirmTerminate(const QString &id);
    ClientController *m_controller;
    QTreeView *m_tree = nullptr;
    QStandardItemModel *m_model = nullptr;
    QTabWidget *m_tabs = nullptr;
    QStackedWidget *m_workspace = nullptr;
    QSplitter *m_splitter = nullptr;
    QLabel *m_connection = nullptr;
    QLabel *m_context = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_error = nullptr;
    QPushButton *m_connect = nullptr;
    QPushButton *m_create = nullptr;
    QPushButton *m_detach = nullptr;
    QPushButton *m_terminate = nullptr;
    QAction *m_createAction = nullptr;
    QAction *m_detachAction = nullptr;
    QAction *m_terminateAction = nullptr;
};
}
