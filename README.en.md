# qawaitablefuture

[简体中文](README.md) | **English**

A single-header C++20 coroutine utility that lets you `co_await` a `QFuture` in Qt, so asynchronous flows read like synchronous code.

```cpp
AsyncTask task()
{
    const int v = co_await QtConcurrent::run([] {
        QThread::sleep(1);
        return 42;
    });
    qDebug() << v; // 42, executed back on the calling thread
}
```

## Features

- `co_await QFuture<T>` for any `T`, including `void`
- Results are delivered through `QFutureWatcher` with `Qt::QueuedConnection`, so the coroutine **resumes on the thread that started the await** (that thread must run an event loop)
- Exceptions stored in a `QFuture` are rethrown at the `co_await` site and can be handled with plain `try/catch`
- `AsyncTask` is a "cancellable reference" to the coroutine frame: keep it to `destroy()` (cancel) the coroutine early
- Header-only, no source file, no moc; logging can be compiled out entirely

## Requirements

| Item | Requirement |
| --- | --- |
| C++ standard | C++20 (coroutines) |
| Qt | Qt 6 (Core, Concurrent; Test additionally for the test suite) |
| Compiler | Any coroutine-capable MSVC / GCC / Clang |

## Integration

Copy `qawaitablefuture.h` into your project, `#include "qawaitablefuture.h"`, and link `Qt::Core` and `Qt::Concurrent`:

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent)

qt_add_executable(app main.cpp)
target_link_libraries(app PRIVATE Qt::Core Qt::Concurrent)
```

## Minimal usage

### 1. Awaiting `QtConcurrent::run`

```cpp
#include "qawaitablefuture.h"
#include <QtConcurrent>

AsyncTask compute()
{
    const int v = co_await QtConcurrent::run([] {
        QThread::sleep(1);
        return 42;
    });
    qDebug() << "result:" << v;
}
```

The return value may be discarded (fire-and-forget):

```cpp
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    compute();            // runs to the first co_await, resumes from the event loop
    return app.exec();
}
```

### 2. Awaiting `QFuture<void>`

```cpp
AsyncTask doWork()
{
    co_await QtConcurrent::run([] { QThread::sleep(1); });
    qDebug() << "done";
}
```

### 3. Returning a result from a worker thread via `QPromise`

```cpp
AsyncTask withPromise()
{
    QPromise<int> promise;
    promise.start();
    QFuture<int> future = promise.future();

    QtConcurrent::run([&promise] {
        promise.addResult(2024);
        promise.finish();
    });

    const int v = co_await future;   // waits for the future without blocking the calling thread
    qDebug() << v;                   // 2024
}
```

### 4. Exception handling

```cpp
AsyncTask onError()
{
    try {
        co_await QtConcurrent::run([]() -> int {
            throw MyException("boom");   // MyException must derive from QException
        });
    } catch (const MyException &e) {
        qDebug() << "caught:" << e.message();
    }
}
```

An uncaught exception reaches `promise_type::unhandled_exception()`, which logs it (visible when `TASK_LOG_ENABLED=1`) and ends the coroutine; the remaining code never runs.

### 5. Cancellation

```cpp
AsyncTask task = compute();   // you must keep the return value
task.destroy();               // destroys a coroutine frame still suspended in co_await
```

```cpp
AsyncTask cancellable()
{
    co_await QtConcurrent::run([] { QThread::sleep(10); });
    qDebug() << "this line never shows if cancelled";
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    AsyncTask task = cancellable();
    QTimer::singleShot(2000, &app, [&task] { task.destroy(); });

    return app.exec();
}
```

### 6. Already-finished futures (`QtFuture::makeReadyValueFuture`)

```cpp
AsyncTask withReadyValue()
{
    const int v = co_await QtFuture::makeReadyValueFuture(1234);
    qDebug() << v;   // 1234
}
```

Such a future is finished from birth: `await_ready()` returns `true`, so the `co_await` **never suspends**. No `QFutureWatcher` is created and no event loop is needed — the coroutine just continues synchronously. Handy for feeding a plain value into a coroutine flow, or mixing it with genuinely asynchronous futures.

## `AsyncTask` ownership semantics

`AsyncTask` is not a `unique_ptr`: it owns the *right to cancel*, not the coroutine's lifetime.

| Action | Effect on the coroutine |
| --- | --- |
| Return value discarded | Runs to completion and frees its own frame; no longer cancellable |
| Return value kept | Can be `destroy()`ed (cancelled) at any time |
| `~AsyncTask()` | Does **not** call `destroy()`; it simply lets go |
| Move construction / assignment | Transfers the cancel right, source becomes empty; neither coroutine is affected |
| `detach()` | Gives up the cancel right; the coroutine keeps running |
| `swap()` | Exchanges the cancel right |
| `valid()` | Whether this object still holds a coroutine |

Key point: **to cancel, you must keep the return value.** `destroy()` is a safe no-op on finished, detached, default-constructed, or moved-from objects, and may be called repeatedly.

Note that `destroy()` only destroys the coroutine frame — it does **not** stop work already running inside `QtConcurrent::run`; that task keeps running and its result is discarded.

## Resume thread and the event loop

After suspension, resumption is posted by `QFutureWatcher::finished` via `Qt::QueuedConnection` to the thread that created the awaiter. Therefore:

- The thread that starts the `co_await` must run an event loop (`QCoreApplication::exec()` / `QEventLoop`), otherwise the coroutine never resumes
- Do not await inside a `QThread::run()` that has no event loop
- Call `destroy()` from the same thread the coroutine lives on; do not call it from another thread

## Exception rules

`QtConcurrent::run` only understands `QException` and its subclasses; any other exception type is replaced by `QUnhandledException`. So custom exceptions should derive from `QException`:

```cpp
class MyException : public QException {
public:
    explicit MyException(QString m) : m_message(std::move(m)) {}
    void raise() const override { throw *this; }
    MyException *clone() const override { return new MyException(*this); }
    const QString &message() const noexcept { return m_message; }
private:
    QString m_message;
};
```

To propagate arbitrary exception types, use `QPromise::setException(std::make_exception_ptr(...))`.

## Logging

Disabled by default (`TASK_LOG_ENABLED 0`). Enable it to print the function name and current thread in red on every suspend/resume/completion:

```cpp
#define TASK_LOG_ENABLED 1     // must precede the include, or pass -DTASK_LOG_ENABLED=1
#include "qawaitablefuture.h"
```

The switch is per translation unit.

## Known limitations

- If the `QFuture` is canceled, `await_ready()` returns `true` immediately and `await_resume()` still calls `result()`; check `isCanceled()` yourself when awaiting a cancellable future
- `destroy()` can only destroy a suspended coroutine frame; it cannot stop computation already submitted to the thread pool
- Re-awaiting the same `QFuture` returns the already-available result instead of waiting again

## Build and test

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`main.cpp` is a demo with seven scenarios (normal result, two exception paths, uncaught exception, `destroy()` cancellation, `makeReadyValueFuture`); `tests/test_qawaitablefuture.cpp` covers the ownership and threading behaviour described above.

## Layout

```
qawaitablefuture.h              # everything: AsyncTask / FutureAwaiter / operator co_await
main.cpp                       # demo program
tests/test_qawaitablefuture.cpp # Qt Test cases
CMakeLists.txt                 # demo target
tests/CMakeLists.txt           # test target
```
