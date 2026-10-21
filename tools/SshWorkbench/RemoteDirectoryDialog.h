#pragma once

#include "RemoteDirectoryService.h"
#include "WorkbenchStore.h"
#include <QCache>
#include <QDialog>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QShowEvent;

namespace sshworkbench
{
class RemoteDirectoryDialogTests;

// 只读浏览远程目录；确认返回当前目录，不改变远程文件或项目配置。
class RemoteDirectoryDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit RemoteDirectoryDialog(const HostProfile &host, const QString &initialPath,
                                   QWidget *parent = nullptr);
    QString selectedPath() const;

protected:
    void showEvent(QShowEvent *event) override;

private:
    friend class RemoteDirectoryDialogTests;
    void requestDirectory(const QString &path, bool refresh = false);
    void applyResult(const RemoteDirectoryResult &result);
    void setBusy(bool busy);

    HostProfile m_host;
    QString m_initialPath;
    RemoteDirectoryResult m_result;
    QCache<QString, RemoteDirectoryResult> m_cache{8192};
    QString m_homePath;
    bool m_started = false;
    RemoteDirectoryService *m_service = nullptr;
    QLineEdit *m_location = nullptr;
    QListWidget *m_drives = nullptr;
    QListWidget *m_directories = nullptr;
    QLabel *m_feedback = nullptr;
    QPushButton *m_up = nullptr;
    QPushButton *m_home = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_select = nullptr;
};
}
