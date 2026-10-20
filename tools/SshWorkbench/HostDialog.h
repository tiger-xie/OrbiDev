#pragma once

#include "WorkbenchStore.h"
#include <QDialog>
#include <functional>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace sshworkbench
{
class HostDialogTests;

// 只编辑主机快照；保存处理器负责持久化，不修改已启动的会话。
class HostDialog final : public QDialog
{
    Q_OBJECT
public:
    explicit HostDialog(const HostProfile &host, QWidget *parent = nullptr);
    HostProfile host() const;
    // 在界面线程校验成功后同步调用；失败返回原因并留窗，取消不会调用。
    void setSaveHandler(std::function<bool(const HostProfile &, QString &)> handler);
    void done(int result) override;

private:
    friend class HostDialogTests;
    bool validateFields();
    bool importConnection(const QString &path);
    void checkConnection();
    void showFeedback(const QString &text, bool error = false);
    void invalidateResult();

    HostProfile m_original;
    std::function<bool(const HostProfile &, QString &)> m_saveHandler;
    bool m_busy = false;
    QString m_diagnosticReport;
    QPushButton *m_copyDiagnostic = nullptr;
    QWidget *m_fields = nullptr;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_host = nullptr;
    QLineEdit *m_user = nullptr;
    QSpinBox *m_port = nullptr;
    QLineEdit *m_identity = nullptr;
    QToolButton *m_fingerprintToggle = nullptr;
    QWidget *m_fingerprintFields = nullptr;
    QLineEdit *m_fingerprint = nullptr;
    QCheckBox *m_confirmed = nullptr;
    QLabel *m_feedback = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
}
