#include "ClientController.h"

namespace rad::client
{
ClientController::ClientController(FakeAgent *agent, QObject *parent) : QObject(parent), m_agent(agent)
{
    Q_ASSERT(agent);
    connect(agent, &FakeAgent::disconnected, this,
            [this]
            {
                ++m_generation;
                m_busy = false;
                for (auto &session : m_sessions)
                {
                    session.attached = false;
                    session.readOnly = true;
                }
                emit changed();
            });
}
bool ClientController::isConnected() const
{
    return m_agent->isConnected();
}
bool ClientController::isBusy() const
{
    return m_busy;
}
QVector<ClientSession> ClientController::sessions() const
{
    return m_sessions;
}
QStringList ClientController::workspaces() const
{
    return m_agent->workspaces();
}
QString ClientController::error() const
{
    return m_error;
}
bool ClientController::begin(bool requireConnection)
{
    if (m_busy)
        return false;
    if (requireConnection && !isConnected())
    {
        m_error = tr("尚未连接 FakeAgent，请先连接。");
        emit changed();
        emit operationFailed(m_error);
        return false;
    }
    m_busy = true;
    m_error.clear();
    emit changed();
    return true;
}
FakeAgent::Completion ClientController::completion(const QString &action)
{
    const QPointer<ClientController> guard(this);
    const auto generation = m_generation;
    return [guard, generation, action](const QString &error, const QString &id)
    {
        if (!guard || generation != guard->m_generation)
            return;
        guard->m_busy = false;
        guard->m_error = error;
        if (error.isEmpty())
            guard->m_sessions = guard->m_agent->sessions();
        emit guard->changed();
        if (!error.isEmpty())
        {
            emit guard->operationFailed(error);
            return;
        }
        if (action == QStringLiteral("open"))
            emit guard->sessionOpened(id);
        else if (action == QStringLiteral("detach"))
            emit guard->sessionDetached(id);
        else if (action == QStringLiteral("create"))
        {
            emit guard->sessionCreated(id);
            guard->openSession(id);
        }
    };
}
void ClientController::connectAgent()
{
    if (begin(false))
        m_agent->connectAgent(completion(QStringLiteral("connect")));
}
void ClientController::disconnectAgent()
{
    m_agent->disconnectAgent();
}
void ClientController::refresh()
{
    if (begin())
        m_agent->listSessions(completion(QStringLiteral("list")));
}
void ClientController::createSession(const QString &name, const QString &workspace)
{
    if (begin())
        m_agent->createSession(name, workspace, completion(QStringLiteral("create")));
}
void ClientController::openSession(const QString &id)
{
    if (begin())
        m_agent->attachSession(id, completion(QStringLiteral("open")));
}
void ClientController::detachSession(const QString &id)
{
    if (begin())
        m_agent->detachSession(id, completion(QStringLiteral("detach")));
}
void ClientController::terminateSession(const QString &id)
{
    if (begin())
        m_agent->terminateSession(id, completion(QStringLiteral("terminate")));
}
}
