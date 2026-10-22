// qawaitablefuture.h
#pragma once

#include <coroutine>
#include <exception>
#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QFuture>
#include <QFutureWatcher>
#include <QObject>
#include <QDebug>
#include <QThread>

// 日志总开关：改为 0（或在外部 -DTASK_LOG_ENABLED=0 / 包含本头文件前定义）即完全不打印
#ifndef TASK_LOG_ENABLED
#define TASK_LOG_ENABLED 0
#endif

// 红色日志宏：打印当前函数名 + 当前线程
#define TASK_LOG_COLOR_RED "\x1b[31m"
#define TASK_LOG_COLOR_OFF "\x1b[0m"

#if TASK_LOG_ENABLED

#define TASK_LOG()                                                          \
    qDebug().nospace().noquote()                                            \
        << TASK_LOG_COLOR_RED                                               \
        << "[" << __FUNCTION__ << "] "                                      \
        << QThread::currentThread()                                         \
        << TASK_LOG_COLOR_OFF

// 追加自定义信息时，msg 形如："xxx: " << value，整体仍为红色
#define TASK_LOG_WITH(msg)                                                  \
    qDebug().nospace().noquote()                                            \
        << TASK_LOG_COLOR_RED                                               \
        << "[" << __FUNCTION__ << "] "                                      \
        << QThread::currentThread()                                         \
        << " | " << msg                                                     \
        << TASK_LOG_COLOR_OFF

#else

#define TASK_LOG()          (void)0
#define TASK_LOG_WITH(msg)  (void)0

#endif

// AsyncTask：协程句柄的"可取消引用"（不是 strict unique_ptr 所有权）
//   - 调用方丢弃返回值：fire-and-forget，协程跑到结束自行释放帧，但无法再取消它；
//   - 调用方持有返回值：可以在外部 destroy()，把仍挂在 co_await 上的协程提前销毁（取消）；
//   - ~AsyncTask() 不自动 destroy()，需要提前结束时必须显式 destroy()；
//   - 移动 / detach() 只转移或放弃"取消权"，不会取消协程（与析构保持一致）；
//     因此 detach() 或放手之后，协程会继续跑到自然结束，不要再指望能取消它。
class AsyncTask {
public:
    struct Data{
        std::coroutine_handle<> handle{};
        bool finished = false;
    };
    struct promise_type {
        std::shared_ptr<Data> state =
            std::make_shared<Data>();

        AsyncTask get_return_object()
        {
            TASK_LOG();
            auto h = std::coroutine_handle<promise_type>::from_promise(*this);
            state->handle = h;
            return AsyncTask(state);
        }

        std::suspend_never initial_suspend() noexcept
        {
            TASK_LOG();
            return {};
        }

        std::suspend_never final_suspend() noexcept
        {
            TASK_LOG();
            return {};
        }

        void return_void() noexcept
        {
            TASK_LOG();
            state->finished = true;
            state->handle = {};
        }

        void unhandled_exception() noexcept
        {
            try {
                throw;
            } catch (const std::exception& e) {
                TASK_LOG_WITH("Unhandled exception in coroutine: " << e.what());
            } catch (...) {
                TASK_LOG_WITH("Unhandled unknown exception in coroutine.");
            }
        }
    };

    AsyncTask() = default;

    explicit AsyncTask(std::shared_ptr<Data> state)
        : state_(std::move(state)) {}

    AsyncTask(const AsyncTask&) = delete;
    AsyncTask& operator=(const AsyncTask&) = delete;

    AsyncTask(AsyncTask&& other) noexcept
        : state_(std::move(other.state_)) {}

    // 只转移取消权，不取消原先持有的协程（它继续跑到自然结束）
    AsyncTask& operator=(AsyncTask&& other) noexcept
    {
        if (this != &other)
            state_ = std::move(other.state_);
        return *this;
    }

    // 放弃取消权但不取消协程：等价于 fire-and-forget
    void detach() noexcept
    {
        state_.reset();
    }

    void swap(AsyncTask& other) noexcept
    {
        state_.swap(other.state_);
    }

    // 是否仍持有某个协程（默认构造、被移动走、detach 之后为 false）
    bool valid() const noexcept
    {
        return static_cast<bool>(state_);
    }

    ~AsyncTask()
    {
    }

    void destroy()
    {
        if (!state_)
            return;

        if (state_->finished)
            return;
        state_->finished = true;
        state_->handle.destroy();
        state_->handle = {};
    }

private:
    std::shared_ptr<Data> state_;
};

inline void swap(AsyncTask& a, AsyncTask& b) noexcept
{
    a.swap(b);
}

template<class T>
class FutureAwaiter {
public:
    explicit FutureAwaiter(QFuture<T> future)
        : future_(std::move(future)) {}

    FutureAwaiter(const FutureAwaiter&) = delete;
    FutureAwaiter& operator=(const FutureAwaiter&) = delete;

    bool await_ready() const noexcept
    {
        TASK_LOG();
        return future_.isFinished();
    }

    void await_suspend(std::coroutine_handle<> h)
    {
        TASK_LOG();
        watcher_ = new QFutureWatcher<T>();

        QObject::connect(watcher_,&QFutureWatcher<T>::finished, watcher_,[h] ()mutable {
                h.resume();
            },Qt::QueuedConnection);
        watcher_->setFuture(future_);
    }

    T await_resume()
    {
        TASK_LOG();
        if constexpr (std::is_void_v<T>) {
            future_.waitForFinished();
            return;
        }
        else {
            future_.waitForFinished();
            return future_.result();
        }
    }

    ~FutureAwaiter()
    {
        TASK_LOG();
        if (watcher_) {
            QObject::disconnect(watcher_, nullptr, nullptr, nullptr);
            delete watcher_;
            watcher_ = nullptr;
        }
    }

private:
    QFuture<T> future_;
    QFutureWatcher<T>* watcher_ = nullptr;
};

template<class T>
FutureAwaiter<T> operator co_await(QFuture<T> future)
{
    return FutureAwaiter<T>(std::move(future));
}