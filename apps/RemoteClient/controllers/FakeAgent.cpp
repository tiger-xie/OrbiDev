#include "FakeAgent.h"

#include <QTimer>
#include <QUuid>
#include <utility>

namespace rad::client
{
FakeAgent::FakeAgent(QObject *parent) : QObject(parent)
{
    m_sessions = {{QStringLiteral("demo-terminal"), tr("终端验证"), QStringLiteral("OrbiDev")},
                  {QStringLiteral("demo-network"), tr("网络调试"), QStringLiteral("OrbiDev")},
                  {QStringLiteral("demo-docs"), tr("文档整理"), QStringLiteral("SampleApp")}};
    m_sessions[1].writerAvailable = false;
    m_sessions[2].running = false;
}
bool FakeAgent::isConnected() const
{
    return m_connected;
}
QVector<ClientSession> FakeAgent::sessions() const
{
    return m_sessions;
}
QStringList FakeAgent::workspaces() const
{
    return {QStringLiteral("OrbiDev"), QStringLiteral("SampleApp")};
}
int FakeAgent::terminateCount() const
{
    return m_terminateCount;
}
void FakeAgent::failNext(Operation operation, const QString &message)
{
    m_failures.insert(static_cast<int>(operation), message);
}
ClientSession *FakeAgent::findSession(const QString &id)
{
    for (auto &session : m_sessions)
        if (session.id == id)
            return &session;
    return nullptr;
}
void FakeAgent::request(Operation operation, const QString &id, Completion completion,
                        std::function<QString()> mutation)
{
    const auto generation = m_generation;
    QTimer::singleShot(
        35, this,
        [this, operation, id, generation, completion = std::move(completion), mutation = std::move(mutation)]
        {
            if (generation != m_generation)
            {
                completion(tr("连接已断开，请重新连接后重试。"), id);
                return;
            }
            const auto failure = m_failures.take(static_cast<int>(operation));
            if (!failure.isEmpty())
            {
                completion(failure, id);
                return;
            }
            if (operation != Operation::Connect && !m_connected)
            {
                completion(tr("尚未连接 FakeAgent。"), id);
                return;
            }
            completion(mutation(), id);
        });
}
void FakeAgent::connectAgent(Completion completion)
{
    request(Operation::Connect, {}, std::move(completion),
            [this]
            {
                m_connected = true;
                return QString();
            });
}
void FakeAgent::disconnectAgent()
{
    ++m_generation;
    m_connected = false;
    for (auto &session : m_sessions)
    {
        session.attached = false;
        session.readOnly = true;
    }
    emit disconnected();
}
void FakeAgent::listSessions(Completion completion)
{
    request(Operation::List, {}, std::move(completion), [] { return QString(); });
}
void FakeAgent::createSession(const QString &name, const QString &workspace, Completion completion)
{
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request(Operation::Create, id, std::move(completion),
            [this, id, name, workspace]
            {
                if (name.trimmed().isEmpty() || name.size() > 256 || !workspaces().contains(workspace))
                    return tr("请输入 1–256 字符的名称并选择已授权工作区。");
                if (m_sessions.size() >= 32)
                    return tr("模拟会话已达 32 条上限。");
                m_sessions.push_back({id, name.trimmed(), workspace});
                return QString();
            });
}
void FakeAgent::attachSession(const QString &id, Completion completion)
{
    request(Operation::Attach, id, std::move(completion),
            [this, id]
            {
                auto *session = findSession(id);
                if (!session)
                    return tr("会话不存在。");
                session->attached = true;
                session->readOnly = !session->running || !session->writerAvailable;
                return QString();
            });
}
void FakeAgent::detachSession(const QString &id, Completion completion)
{
    request(Operation::Detach, id, std::move(completion),
            [this, id]
            {
                auto *session = findSession(id);
                if (!session)
                    return tr("会话不存在。");
                session->attached = false;
                session->readOnly = true;
                return QString();
            });
}
void FakeAgent::terminateSession(const QString &id, Completion completion)
{
    request(Operation::Terminate, id, std::move(completion),
            [this, id]
            {
                auto *session = findSession(id);
                if (!session)
                    return tr("会话不存在。");
                if (!session->attached || session->readOnly)
                    return tr("当前客户端没有终止此会话的控制权。");
                if (session->running)
                    ++m_terminateCount;
                session->running = false;
                session->readOnly = true;
                return QString();
            });
}
}
