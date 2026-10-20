#pragma once

#include "AuthorizationService.h"
#include "EnvironmentService.h"
#include "IdentityService.h"

#include <QVector>
#include <QWidget>
#include <functional>

class QCheckBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QThread;
class QVBoxLayout;

class SetupWindowTests;

namespace sshsetup
{

// 所有服务调用在工作线程执行；预览模式不访问密钥或系统配置。
class SetupWindow final : public QWidget
{
    Q_OBJECT
public:
    explicit SetupWindow(bool homeMode, const QString &profilePath, const QString &legacyConfigPath = {},
                         bool preview = false, QWidget *parent = nullptr);
    ~SetupWindow() override;
    void setPreviewPage(int page);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    friend class ::SetupWindowTests;
    void buildUi();
    QWidget *makePage(const QString &title, const QString &description, QVBoxLayout *&layout);
    void buildEnvironmentPage();
    void buildIdentityPage();
    void buildAuthorizationPage();
    void buildConnectionPage();
    void buildImportPage();
    void buildApplyPage();
    void buildResultPage();
    void setPage(int page);
    void updateActions();
    void updateSteps();
    void invalidateIdentityResults();
    void invalidateImportResults();
    void showFeedback(const QString &message, bool error = false);
    void runOperation(std::function<void()> work, std::function<void()> finished);
    void inspectEnvironment();
    void prepareIdentity(const QString &existingPath = {});
    void validateImport();
    void applyAuthorization();
    void checkConnection();
    void fillConnectionFields();
    void importConnection();
    void exportPublicKey();
    void exportConnection();
    void next();

    bool m_homeMode = false;
    bool m_preview = false;
    bool m_busy = false;
    bool m_environmentReady = false;
    bool m_authorized = false;
    bool m_connectionVerified = false;
    bool m_authorizationAcknowledged = false;
    bool m_environmentFailed = false;
    bool m_identityFailed = false;
    bool m_connectionFailed = false;
    bool m_importFailed = false;
    bool m_applyFailed = false;
    QString m_profilePath;
    QString m_legacyConfigPath;
    QString m_normalizedKey;
    QString m_importFingerprint;
    QString m_environmentReport;
    QString m_connectionReport;
    Identity m_identity;
    HomeEnvironment m_homeEnvironment;
    ConnectionProfile m_profile;
    QThread *m_worker = nullptr;
    QStackedWidget *m_pages = nullptr;
    QVector<QWidget *> m_stepRows;
    QVector<QLabel *> m_stepBadges;
    QVector<QLabel *> m_stepDescriptions;
    QVector<QWidget *> m_stepLines;
    QLabel *m_feedback = nullptr;
    QLabel *m_environmentText = nullptr;
    QLabel *m_identityText = nullptr;
    QLabel *m_publicSummary = nullptr;
    QLabel *m_importSummary = nullptr;
    QLabel *m_importDetails = nullptr;
    QLabel *m_applySummary = nullptr;
    QLabel *m_resultSummary = nullptr;
    QPushButton *m_back = nullptr;
    QPushButton *m_next = nullptr;
    QPlainTextEdit *m_publicInput = nullptr;
    QPlainTextEdit *m_publicKeyPreview = nullptr;
    QPushButton *m_copyConnectionReport = nullptr;
    QPushButton *m_exportConnection = nullptr;
    QLineEdit *m_host = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_fingerprint = nullptr;
    QSpinBox *m_port = nullptr;
    QCheckBox *m_fingerprintConfirmed = nullptr;
    QCheckBox *m_applyConfirmed = nullptr;
};
}
