#include "SshSession.h"
#include "ConPtyProcess.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace sshworkbench
{
namespace
{
constexpr int kInputLimit = 256 * 1024;
constexpr int kOutputLimit = 1024 * 1024;
bool validSize(int columns, int rows)
{
    return columns > 0 && rows > 0 && columns <= 32767 && rows <= 32767;
}
QString encodeScript(const QString &script)
{
    QByteArray encoded;
    for (const QChar character : script)
    {
        encoded.append(char(character.unicode() & 255));
        encoded.append(char(character.unicode() >> 8));
    }
    return QString::fromLatin1(encoded.toBase64());
}
}

bool remoteCommandArguments(const QString &directory, QStringList &arguments, QString &error)
{
    error.clear();
    if (directory.size() > 4096 || directory.contains(QChar::Null) || directory.contains('\r') ||
        directory.contains('\n'))
    {
        error = QStringLiteral("远程目录过长或包含无效字符。");
        return false;
    }
    // 目录单独编码，避免 PowerShell 将弯引号识别为脚本分隔符。
    const QString location =
        QStringLiteral("([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('%1')))")
            .arg(encodeScript(directory));
    const QString changeDirectory =
        directory.isEmpty()
            ? QStringLiteral("Set-Location -LiteralPath $HOME -ErrorAction Stop; ")
            : QStringLiteral("Set-Location -LiteralPath %1 -ErrorAction Stop; ").arg(location);
    const QString script = QStringLiteral("$ErrorActionPreference='Stop'; try { ") + changeDirectory +
                           QStringLiteral("$cli=Get-Command claude.exe -ErrorAction SilentlyContinue; "
                                          "if (!$cli) { $cli=Get-Command claude.cmd -ErrorAction Stop }; "
                                          "& $cli.Source; exit $LASTEXITCODE } catch { "
                                          "[Console]::Error.WriteLine($_.Exception.Message); exit 1 }");
    arguments =
        QStringList{QStringLiteral("powershell.exe"), QStringLiteral("-NoLogo"), QStringLiteral("-NoProfile"),
                    QStringLiteral("-EncodedCommand"), encodeScript(script)};
    return true;
}

class SshSession::Impl
{
public:
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    QTimer *timer = nullptr;
    QByteArray input;
    QByteArray output;
    QString error;
    int columns = 120;
    int rows = 32;
    int exitCode = -1;
    bool stopRequested = false;
    bool resizeRequested = false;
    bool hostVerificationPending = false;
    bool startedPending = false;
    bool finished = false;
    bool attempted = false;
    bool active = false;

    void fail(const QString &message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (error.isEmpty())
            error = message;
        stopRequested = true;
    }
};

SshSession::SshSession(QObject *parent) : QObject(parent), m_impl(std::make_unique<Impl>())
{
    m_impl->timer = new QTimer(this);
    m_impl->timer->setInterval(16);
    connect(m_impl->timer, &QTimer::timeout, this, &SshSession::poll);
}

SshSession::~SshSession()
{
    stop();
    if (m_impl->worker.joinable())
        m_impl->worker.join();
}

void SshSession::start(const sshsetup::ConnectionProfile &profile, const QString &remoteDirectory,
                       int columns, int rows)
{
    startProcess(profile, remoteDirectory, columns, rows);
}

void SshSession::startProcess(const sshsetup::ConnectionProfile &profile, const QString &directory,
                              int columns, int rows, const QString &testProgram,
                              const QStringList &testArguments)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_impl->attempted)
    {
        emit errorOccurred(QStringLiteral("此会话已启动过，请新建终端。"));
        return;
    }
    m_impl->attempted = true;
    m_impl->active = true;
    m_impl->columns = columns;
    m_impl->rows = rows;
    m_impl->timer->start();
    m_impl->worker = std::thread(
        [this, profile, directory, columns, rows, testProgram, testArguments]
        {
            int exitCode = -1;
            try
            {
                if (!validSize(columns, rows))
                    throw std::runtime_error("Invalid terminal dimensions");
                QTemporaryDir trustDirectory;
                QString error;
                QStringList arguments;
                QString program = testProgram;
                if (program.isEmpty())
                {
                    QStringList command;
                    if (!remoteCommandArguments(directory, command, error))
                        throw std::runtime_error(error.toUtf8().constData());
                    {
                        std::lock_guard<std::mutex> lock(m_impl->mutex);
                        m_impl->hostVerificationPending = true;
                    }
                    if (!sshsetup::prepareSshArguments(profile, trustDirectory.path(), arguments, error,
                                                       true))
                        throw std::runtime_error(error.toUtf8().constData());
                    arguments.append(command);
                    program =
                        qEnvironmentVariable("SystemRoot") + QStringLiteral("/System32/OpenSSH/ssh.exe");
                }
                else
                    arguments = testArguments;
                if (!QFileInfo(program).isFile())
                    throw std::runtime_error("System SSH executable does not exist");
                std::vector<std::wstring> nativeArguments;
                for (const QString &argument : arguments)
                    nativeArguments.push_back(argument.toStdWString());
                bool cancelled = false;
                {
                    std::lock_guard<std::mutex> lock(m_impl->mutex);
                    cancelled = m_impl->stopRequested;
                }
                if (!cancelled)
                {
                    rad::windows::ConPtyProcess process;
                    process.startValidated(program.toStdWString(), QDir::homePath().toStdWString(),
                                           nativeArguments, short(columns), short(rows));
                    {
                        std::lock_guard<std::mutex> lock(m_impl->mutex);
                        m_impl->startedPending = true;
                    }
                    std::uint64_t cursor = 0;
                    for (;;)
                    {
                        QByteArray input;
                        bool stopping = false;
                        bool resizing = false;
                        int width = 0, height = 0;
                        {
                            std::lock_guard<std::mutex> lock(m_impl->mutex);
                            input.swap(m_impl->input);
                            stopping = m_impl->stopRequested;
                            resizing = m_impl->resizeRequested;
                            m_impl->resizeRequested = false;
                            width = m_impl->columns;
                            height = m_impl->rows;
                        }
                        bool finished = process.waitForExit(std::chrono::milliseconds(0));
                        if (stopping && !finished)
                        {
                            process.terminate();
                            finished = true;
                        }
                        if (!finished)
                        {
                            if (resizing)
                                process.resize(short(width), short(height));
                            if (!input.isEmpty())
                                process.writeInput(input.toStdString());
                        }
                        const auto delta = process.outputSince(cursor);
                        {
                            std::lock_guard<std::mutex> lock(m_impl->mutex);
                            if (delta.droppedBytes ||
                                delta.bytes.size() > std::size_t(kOutputLimit - m_impl->output.size()))
                            {
                                if (m_impl->error.isEmpty())
                                    m_impl->error =
                                        QStringLiteral("终端输出超过缓冲上限，会话已停止，请新建终端。");
                                m_impl->stopRequested = true;
                            }
                            else
                                m_impl->output.append(delta.bytes.data(), int(delta.bytes.size()));
                        }
                        if (!process.ioError().empty())
                            m_impl->fail(QString::fromStdString(process.ioError()));
                        if (finished)
                        {
                            exitCode = int(process.exitCode());
                            break;
                        }
                        std::unique_lock<std::mutex> lock(m_impl->mutex);
                        m_impl->changed.wait_for(lock, std::chrono::milliseconds(10),
                                                 [this]
                                                 {
                                                     return m_impl->stopRequested ||
                                                            !m_impl->input.isEmpty() ||
                                                            m_impl->resizeRequested;
                                                 });
                    }
                }
            }
            catch (const std::exception &error)
            {
                m_impl->fail(QStringLiteral("SSH 终端：") + QString::fromUtf8(error.what()));
            }
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->exitCode = exitCode;
            m_impl->finished = true;
        });
}

void SshSession::writeInput(const QByteArray &bytes)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_impl->active)
    {
        emit errorOccurred(QStringLiteral("会话未运行，无法发送输入。"));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (bytes.size() > kInputLimit - m_impl->input.size())
        {
            m_impl->error = QStringLiteral("输入超过缓冲上限，会话已停止。请分段粘贴。");
            m_impl->stopRequested = true;
        }
        else if (!m_impl->stopRequested)
            m_impl->input.append(bytes);
    }
    m_impl->changed.notify_one();
}

void SshSession::resize(int columns, int rows)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!validSize(columns, rows))
    {
        emit errorOccurred(QStringLiteral("终端行列数无效。"));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->columns = columns;
        m_impl->rows = rows;
        m_impl->resizeRequested = true;
    }
    m_impl->changed.notify_one();
}

void SshSession::stop()
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->stopRequested = true;
    m_impl->changed.notify_one();
}

bool SshSession::isActive() const
{
    return m_impl->active;
}

void SshSession::poll()
{
    QByteArray output;
    QString error;
    bool hostVerification = false, started = false, finished = false;
    int exitCode = -1;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        output.swap(m_impl->output);
        error.swap(m_impl->error);
        hostVerification = m_impl->hostVerificationPending;
        m_impl->hostVerificationPending = false;
        started = m_impl->startedPending;
        m_impl->startedPending = false;
        finished = m_impl->finished;
        exitCode = m_impl->exitCode;
    }
    if (finished)
    {
        m_impl->active = false;
        m_impl->timer->stop();
    }
    const QPointer<SshSession> guard(this);
    if (hostVerification)
        emit hostVerificationStarted();
    if (!guard)
        return;
    if (started)
        emit this->started();
    if (!guard)
        return;
    if (!output.isEmpty())
        emit outputReady(output);
    if (!guard)
        return;
    if (!error.isEmpty())
        emit errorOccurred(error);
    if (!guard)
        return;
    if (finished)
        emit exited(exitCode);
}
}
