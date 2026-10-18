#include "ClientController.h"

namespace rad::client
{
ClientController::ClientController(IAgentClient *agent, QObject *parent) : QObject(parent), m_agent(agent)
{
    Q_ASSERT(agent);
    connect(agent, &IAgentClient::sessionsChanged, this,
            [this]
            {
                m_sessions = m_agent->sessions();
                emit changed();
            });
    connect(agent, &IAgentClient::errorOccurred, this,
            [this](const QString &error)
            {
                m_error = error;
                emit changed();
                emit operationFailed(error);
            });
    connect(agent, &IAgentClient::outputReceived, this,
            [this](const QString &id, const QByteArray &bytes, int historyBytes)
            {
                if (m_readyTerminals.contains(id))
                    emit outputReady(id, bytes, historyBytes);
                else
                {
                    auto &pending = m_pendingOutput[id];
                    if (pending.size() + bytes.size() > 1024 * 1024)
                    {
                        terminalFailed(tr("终端尚未就绪且输出超过缓冲上限，已断开；远程进程未终止。"));
                        return;
                    }
                    pending += bytes;
                    m_pendingHistoryBytes[id] += historyBytes;
                }
            });
    connect(agent, &IAgentClient::disconnected, this,
            [this]
            {
                ++m_generation;
                m_readyTerminals.clear();
                m_pendingOutput.clear();
                m_pendingHistoryBytes.clear();
                m_resizePending.clear();
                m_latestSizes.clear();
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
        m_error = tr("尚未连接 Agent，请先连接。");
        emit changed();
        emit operationFailed(m_error);
        return false;
    }
    m_busy = true;
    m_error.clear();
    emit changed();
    return true;
}
IAgentClient::Completion ClientController::completion(const QString &action)
{
    const QPointer<ClientController> guard(this);
    const auto generation = m_generation;
    return [guard, generation, action](const QString &error, const QString &id)
    {
        if (!guard || generation != guard->m_generation)
            return;
        guard->m_busy = false;
        if (!error.isEmpty())
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
        {
            guard->m_readyTerminals.remove(id);
            guard->m_pendingOutput.remove(id);
            guard->m_pendingHistoryBytes.remove(id);
            emit guard->sessionDetached(id);
        }
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
    {
        for (const auto &session : m_sessions)
            if (session.id == id && !session.attached)
            {
                m_readyTerminals.remove(id);
                m_pendingOutput.remove(id);
                m_pendingHistoryBytes.remove(id);
            }
        m_agent->attachSession(id, completion(QStringLiteral("open")));
    }
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
bool ClientController::isSimulation() const
{
    return m_agent->isSimulation();
}
QString ClientController::displayName() const
{
    return m_agent->displayName();
}
bool ClientController::terminalIsReady(const QString &id) const
{
    return m_readyTerminals.contains(id);
}
void ClientController::terminalReady(const QString &id)
{
    m_readyTerminals.insert(id);
    const auto bytes = m_pendingOutput.take(id);
    const int historyBytes = m_pendingHistoryBytes.take(id);
    if (!bytes.isEmpty())
        emit outputReady(id, bytes, historyBytes);
}
void ClientController::terminalFailed(const QString &message)
{
    m_agent->disconnectAgent();
    m_error = message;
    emit changed();
    emit operationFailed(message);
}
void ClientController::sendInput(const QString &id, const QByteArray &bytes)
{
    QString error;
    if (!m_agent->sendInput(id, bytes, error))
    {
        m_error = error;
        emit changed();
        emit operationFailed(error);
    }
}
void ClientController::resizeTerminal(const QString &id, int columns, int rows)
{
    if (!isConnected())
        return;
    m_latestSizes.insert(id, QSize(columns, rows));
    if (m_resizePending.contains(id))
        return;
    m_resizePending.insert(id);
    const QSize requested = m_latestSizes.take(id);
    const QPointer<ClientController> guard(this);
    const auto generation = m_generation;
    m_agent->resizeSession(id, requested.width(), requested.height(),
                           [guard, generation, id](const QString &error, const QString &)
                           {
                               if (!guard || guard->m_generation != generation)
                                   return;
                               guard->m_resizePending.remove(id);
                               if (!error.isEmpty())
                               {
                                   guard->m_latestSizes.remove(id);
                                   guard->m_error = error;
                                   emit guard->changed();
                                   emit guard->operationFailed(error);
                                   return;
                               }
                               // 网络往返期间的中间尺寸合并，只发送最后一次，避免拖拽丢失最终尺寸。
                               if (guard->m_latestSizes.contains(id))
                               {
                                   const QSize latest = guard->m_latestSizes.take(id);
                                   guard->resizeTerminal(id, latest.width(), latest.height());
                               }
                           });
}
}
