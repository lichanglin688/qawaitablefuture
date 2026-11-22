// cancellation.h
// 取消机制，只由两个东西组成：
//   async::CancellationSource        取消对象（QObject），requestCancel() 发出 cancelled() 信号
//   async::CancellableFutureAwaiter  把 QFuture 和取消对象连在一起：谁先到谁赢
//
// 用法：
//   async::CancellationSource source;
//   const int v = co_await async::CancellableFutureAwaiter(QtConcurrent::run([] { ...; return 42; }), &source);
//
//   // 与 await 发起方在同一线程调用（可多次调用，只有第一次真正发出信号）：
//   source.requestCancel();
//
// 语义约定：
//   - 单线程使用：CancellationSource 与 CancellableFutureAwaiter 不做跨线程保护，
//     requestCancel() 必须与发起 await 的线程相同（通常就是主线程）。
//   - 谁先到谁赢：future 先完成则正常返回结果，之后到来的取消信号被忽略；
//     取消信号先到则抛 OperationCancelled，不再等待 future 的结果。
//   - 恢复线程与 qawaitablefuture.h 一致：总是回到发起 await 的线程（需要事件循环）。
//   - source 允许传 nullptr，此时退化为普通 FutureAwaiter。
//
// 注意：CancellationSource 带 Q_OBJECT，需要 moc。本工程 CMake 已开启 AUTOMOC，
// 只要某个编译单元 #include 了本头文件就会自动处理；外部使用方需保证 AUTOMOC 开启。
#pragma once

#include <coroutine>
#include <type_traits>
#include <utility>

#include <QException>
#include <QFuture>
#include <QFutureWatcher>
#include <QObject>

namespace async {

// 取消异常：取消信号先到时，在 co_await 处抛出。
// 继承 QException，需要时也可以被存进 QFuture 做跨线程重抛。
class OperationCancelled : public QException {
public:
    OperationCancelled() = default;

    void raise() const override { throw *this; }
    OperationCancelled *clone() const override { return new OperationCancelled(*this); }

    const char *what() const noexcept override { return "async::OperationCancelled"; }
};

// 取消对象：requestCancel() 发出 cancelled() 信号。
// 幂等保护很重要 —— 同一个 source 只发一次信号，挂在它上面的协程就不会被重复 resume。
class CancellationSource : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // 发出取消信号。重复调用是 no-op。仅允许在发起 await 的线程调用。
    void requestCancel()
    {
        if (!cancelled_) {
            cancelled_ = true;
            emit cancelled();
        }
    }

    bool isCancelRequested() const noexcept { return cancelled_; }

signals:
    void cancelled();

private:
    bool cancelled_ = false; // 单线程使用，无需原子
};

// 可取消的 QFuture awaiter：
//   - QFutureWatcher::finished  → 恢复协程，await_resume 返回结果
//   - source 的 cancelled() 信号 → 槽里标记取消并恢复协程，await_resume 抛 OperationCancelled
// 两条路共用 resumed_ 守卫，先到者赢，绝不可能 resume 两次。
template<class T>
class CancellableFutureAwaiter {
public:
    CancellableFutureAwaiter(QFuture<T> future, CancellationSource *source)
        : future_(std::move(future))
        , source_(source)
    {}

    CancellableFutureAwaiter(const CancellableFutureAwaiter&) = delete;
    CancellableFutureAwaiter& operator=(const CancellableFutureAwaiter&) = delete;

    bool await_ready() const noexcept
    {
        // future 已完成且没有取消请求：不挂起，直接取结果
        return future_.isFinished() && !(source_ && source_->isCancelRequested());
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        watcher_ = new QFutureWatcher<T>();

        // 事件一：future 完成 → 恢复
        QObject::connect(watcher_, &QFutureWatcher<T>::finished, watcher_, [this, h]() mutable {
            if (resumed_)
                return;
            resumed_ = true;
            h.resume();
        });

        // 事件二：取消信号的"槽"。异常不能在这里抛（会打穿 Qt 事件循环），
        // 只标记取消并恢复协程，让 await_resume() 在 co_await 处把异常抛给用户。
        // QueuedConnection 让恢复统一走事件循环，避免在 requestCancel() 的调用栈里同步重入
        // （本机制只支持单线程使用，不支持从其他线程 requestCancel）。
        if (source_) {
            QObject::connect(source_, &CancellationSource::cancelled, watcher_, [this, h]() mutable {
                if (resumed_)
                    return;
                resumed_ = true;
                cancelled_ = true;
                h.resume();
            }, Qt::QueuedConnection);
        }

        watcher_->setFuture(future_);

        // 信号是"发出即逝"的：如果 requestCancel() 早于本次挂起，信号不会再补发，
        // 这里手动补一次取消路径
        if (source_ && source_->isCancelRequested()) {
            resumed_ = true;
            cancelled_ = true;
            QMetaObject::invokeMethod(watcher_, [h] { h.resume(); }, Qt::QueuedConnection);
        }
    }

    T await_resume()
    {
        if (cancelled_)
            throw OperationCancelled{};

        future_.waitForFinished();
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return future_.result();
        }
    }

    ~CancellableFutureAwaiter()
    {
        QObject::disconnect(watcher_, nullptr, nullptr, nullptr);
        delete watcher_; // watcher 一死，所有挂着它的 queued 恢复事件都被丢弃
        watcher_ = nullptr;
    }

private:
    QFuture<T> future_;
    CancellationSource *source_ = nullptr;
    QFutureWatcher<T> *watcher_ = nullptr;
    bool resumed_ = false;   // future / 取消信号只能有一个真正 resume（两个槽都在本线程排队执行）
    bool cancelled_ = false; // await_resume 据此抛出取消异常
};

template<class T>
CancellableFutureAwaiter(QFuture<T>, CancellationSource *) -> CancellableFutureAwaiter<T>;

} // namespace async
