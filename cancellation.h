#pragma once

#include <coroutine>
#include <type_traits>
#include <utility>

#include <QException>
#include <QFuture>
#include <QFutureWatcher>
#include <QObject>

namespace async {

class OperationCancelled : public QException {
public:
    OperationCancelled() = default;

    void raise() const override { throw *this; }
    OperationCancelled *clone() const override { return new OperationCancelled(*this); }

    const char *what() const noexcept override { return "async::OperationCancelled"; }
};

class CancellationSource : public QObject {
    Q_OBJECT

public:
    void requestCancel()
    {
        cancelled_ = true;
        emit cancelled();
    }

    bool isCancelRequested() const noexcept { return cancelled_; }

signals:
    void cancelled();

private:
    bool cancelled_ = false;
};

template<class T>
class CancellableFutureAwaiter {
public:
    CancellableFutureAwaiter(QFuture<T> future, CancellationSource *source)
        : future_(std::move(future))
        , source_(source)
    {}

    CancellableFutureAwaiter(const CancellableFutureAwaiter&) = delete;
    CancellableFutureAwaiter& operator=(const CancellableFutureAwaiter&) = delete;

    bool await_ready()
    {
        if (source_ && source_->isCancelRequested()) {
            cancelled_ = true;
            return true;
        }
        return future_.isFinished();
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        watcher_ = new QFutureWatcher<T>();

        QObject::connect(watcher_, &QFutureWatcher<T>::finished, watcher_, [this, h]() mutable {
            h.resume();
        });

        if (source_) {
            QObject::connect(source_, &CancellationSource::cancelled, watcher_, [this, h]() mutable {
                cancelled_ = true;
                h.resume();
            });
        }

        watcher_->setFuture(future_);
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
        delete watcher_;
    }

private:
    QFuture<T> future_;
    CancellationSource *source_ = nullptr;
    QFutureWatcher<T> *watcher_ = nullptr;
    bool cancelled_ = false;
};

} // namespace async
