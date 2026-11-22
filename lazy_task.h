// lazy_task.h
// async::LazyTask<T>：惰性协程，调用时不执行，只有被 co_await 时才开始跑。
//
// 与 async::Task 的区别（两者互补，不要互相替代）：
//   async::Task        饥饿执行（调用即跑到第一个挂起点）、无返回值、可被外部 destroy() 取消
//   async::LazyTask<T> 惰性执行（调用一行都不跑）、有返回值 T、一次性 co_await、值语义独占帧
//
// 用法上的唯一约束：await 必须针对右值（一次性语义由类型系统表达）：
//   const int v = co_await makeLazyTask();   // prvalue，可以
//   auto t = makeLazyTask();
//   const int v = co_await std::move(t);     // 具名对象要显式 move
//
// 与 QFuture 配合：本头文件不依赖 Qt，单独可用；若要在 LazyTask 里 co_await QFuture，
// 同时包含 qawaitablefuture.h，届时恢复线程由最内层的 QFuture 决定（走 QFutureWatcher 的队列投递）。
#pragma once

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace async {

template<class T = void>
class LazyTask;

namespace detail {

// 公共部分：协程属于 to-CRTP 的 Derived 类型，这样 from_promise 拿到的才是真正的帧地址
template<class Derived, class T>
class LazyPromiseBase {
public:
    LazyTask<T> get_return_object() noexcept
    {
        return LazyTask<T>(std::coroutine_handle<Derived>::from_promise(static_cast<Derived&>(*this)));
    }

    // 惰性：初始挂起，协程体一行都不执行，直到有人 await 它（由对称转移跳进来）
    std::suspend_always initial_suspend() const noexcept
    {
        return {};
    }

    void unhandled_exception() noexcept
    {
        error = std::current_exception();
    }

    // 结束时把执行流直接交给等待者：这里用对称转移，而不是在 final_suspend 里手写 resume，
    // 一是避免嵌套 resume 的栈增长与悬栈风险，二是天然继承"叶子 await"所在的线程
    struct FinalTransfer {
        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<Derived> h) const noexcept
        {
            const Derived& p = h.promise();
            // 没有人 await（比如被丢弃了），回到 noop 而不是返回空 handle
            return p.continuation ? p.continuation : std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    FinalTransfer final_suspend() const noexcept
    {
        return {};
    }

    std::coroutine_handle<> continuation{}; // 正在 await 我的那个协程
    std::exception_ptr error;               // 协程体内的异常，等到 await 时才抛给等待者
    bool started = false;                   // 协程体是否已经起步（LazyTask 是一次性的）
};

// 有返回值的版本
template<class T>
class LazyPromise final : public LazyPromiseBase<LazyPromise<T>, T> {
public:
    template<class U>
    void return_value(U&& value)
    {
        result.emplace(std::forward<U>(value));
    }

    std::optional<T> result;
};

// void 版本：注意承诺类型不允许同时有 return_value 和 return_void，所以必须偏特化拆开
template<>
class LazyPromise<void> final : public LazyPromiseBase<LazyPromise<void>, void> {
public:
    void return_void() const noexcept {}
};

template<class T>
class LazyAwaiter {
public:
    explicit LazyAwaiter(std::coroutine_handle<LazyPromise<T>> h) noexcept
        : h_(h) {}

    // 惰性：必须挂起一次，才有机会把执行权对称转移给 LazyTask 的协程体
    bool await_ready() const noexcept { return false; }

    // 返回 LazyTask 自己的 handle，编译器直接尾跳转进它的协程体，
    // 而不是在 await_suspend 里调用 resume 造成嵌套栈帧
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept
    {
        h_.promise().continuation = awaiting;
        return h_;
    }

    decltype(auto) await_resume()
    {
        LazyPromise<T>& p = h_.promise();
        if (p.error)
            std::rethrow_exception(std::exchange(p.error, std::exception_ptr{}));

        if constexpr (std::is_void_v<T>)
            return;
        else
            return std::move(*p.result);
    }

private:
    std::coroutine_handle<LazyPromise<T>> h_;
};

} // namespace detail

// LazyTask<T>：一次性、可移动、拥有协程帧。
// 帧的所有权跟随对象本身（析构即销毁帧），因此 await 期间 LazyTask 必须活着 —— 通常它就在等待方
// 的协程帧里天然满足；千万不要 await 一个生命期比自己短的 LazyTask。
template<class T>
class LazyTask {
public:
    using promise_type = detail::LazyPromise<T>;

    LazyTask() noexcept = default;

    explicit LazyTask(std::coroutine_handle<promise_type> h) noexcept
        : h_(h) {}

    LazyTask(const LazyTask&) = delete;
    LazyTask& operator=(const LazyTask&) = delete;

    LazyTask(LazyTask&& other) noexcept
        : h_(std::exchange(other.h_, {})) {}

    LazyTask& operator=(LazyTask&& other) noexcept
    {
        if (this != &other) {
            destroyFrame();                 // 接管别人的帧之前，先把自己原有的帧销毁
            h_ = std::exchange(other.h_, {});
        }
        return *this;
    }

    ~LazyTask()
    {
        destroyFrame();
    }

    // 是否还持有一个协程帧（默认构造、被移动走之后为 false）
    bool valid() const noexcept
    {
        return static_cast<bool>(h_);
    }

    // 协程体是否已经跑到终点（从未起步、或仍停在初始挂起点时都是 false）
    bool done() const noexcept
    {
        return h_ && h_.done();
    }

    // 提前销毁帧。注意：正在被 await 的时候不要 destroy() —— 等待者的 continuation 会
    // 因为收不到回执而永远挂在 co_await 上；等 await 结束（或确定不 await）再销毁。
    void destroy() noexcept
    {
        destroyFrame();
    }

    // 只对右值开放 await：一次性语义直接由类型表达，同一个对象无法 await 两次
    detail::LazyAwaiter<T> operator co_await() &&
    {
        if (!h_)
            throw std::logic_error("async::LazyTask: awaiting an empty task");

        promise_type& p = h_.promise();
        if (p.started)
            throw std::logic_error("async::LazyTask: can only be awaited once");

        p.started = true;
        return detail::LazyAwaiter<T>(h_);
    }

private:
    void destroyFrame() noexcept
    {
        if (h_) {
            h_.destroy();
            h_ = {};
        }
    }

    std::coroutine_handle<promise_type> h_{};
};

} // namespace async
