#pragma once
#include <QObject>
#include <model/SessionTypes.h>

namespace rad
{
// 单实例单进程：未启动→Creating→Running→Exiting→Exited/Failed，不复用。
// 所有方法/信号在对象所属的 Agent 控制线程执行，方法不得阻塞等待进程或管道。
// 同线程 parent 或调用方二选一拥有对象；销毁前请求 terminate 并等待 finished。
// 析构须兜底回收资源，不得向已销毁对象回调；参数只借用到方法返回，异步操作须复制。
// 同步失败不改变状态且不产生此次操作的完成事件。start 成功仅代表接受：
// 请求返回后才可发送 started/failed/finished；每次接受的 start 最终恰好一次 finished。
// 正常启动先 started，启动失败先 failed(fatal=true)；Creating 取消可直接 finished。
// 致命 failed 后必须 finished；非致命错误不伪造退出。工作线程尾部输出归并后才发 finished，
// 之后不再生产输出，仍可拉取尾部缓存。
class ITerminalBackend : public QObject
{
    Q_OBJECT
public:
    explicit ITerminalBackend(QObject *parent = nullptr) : QObject(parent)
    {
        registerSessionMetaTypes();
    }
    ~ITerminalBackend() override = default;
    // 重复 start 返回 InvalidState；无效路径/尺寸返回 InvalidArgument，未接受不启动。
    virtual OperationResult start(const LaunchSpec &spec) = 0;
    // 仅 Running 接受；全部字节入有界队列或以 QueueFull 拒绝，不允许部分成功。
    // 后续写失败由 failed(InputFailed, false) 通知；无法继续运行则 fatal=true。
    virtual OperationResult writeInput(const QByteArray &bytes) = 0;
    // 仅 Running 接受有效尺寸；后续失败通过 failed(ResizeFailed, false) 通知。
    virtual OperationResult resize(TerminalSize size) = 0;
    // 未启动/已结束无操作成功，Exiting 重复调用幂等；其余状态接受后异步 finished。
    virtual OperationResult terminate() = 0;
    // maxBytes 必须为正，返回不超过该值；合法空结果与失败分开。终态允许排空。
    // 有界缓存超限丢弃最旧字节并累计 droppedBytes，不允许静默丢失。
    virtual Result<OutputChunk> takeOutput(int maxBytes) = 0;
signals:
    void started();
    void failed(const rad::Error &error, bool fatal);
    void finished(rad::ExitStatus status);
    // 空→非空（含丢弃计数）时通知一次，拉空前合并通知；hasMore 时消费者继续有界拉取。
    // 控制线程前的工作线程投递也必须合并，不能每个输出块创建一个 queued 事件。
    void outputAvailable();
};
}
