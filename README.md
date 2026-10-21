# qawaitablefuture

**简体中文** | [English](README.en.md)

一个只有单头文件的 C++20 协程小库，让你在 Qt 里用 `co_await` 直接等待 `QFuture`，像写同步代码一样写异步流程。

```cpp
AsyncTask task()
{
    const int v = co_await QtConcurrent::run([] {
        QThread::sleep(1);
        return 42;
    });
    qDebug() << v; // 42，回到调用线程执行
}
```

## 特性

- `co_await QFuture<T>`，支持 `T` 为任意类型以及 `void`
- 结果通过 `QFutureWatcher` 以 `Qt::QueuedConnection` 投递，**协程恢复到发起 await 的那个线程**（该线程需要有事件循环）
- 异常从 `QFuture` 原样带回 `co_await` 处，可直接 `try/catch`
- `AsyncTask` 是协程帧的"可取消引用"：持有它就能 `destroy()` 提前取消
- 头文件即可用，无源文件、无 moc，日志可通过宏整体关掉

## 环境要求

| 项 | 要求 |
| --- | --- |
| C++ 标准 | C++20（协程） |
| Qt | Qt 6（Core、Concurrent；测试额外需要 Test） |
| 编译器 | 支持协程的 MSVC / GCC / Clang |

## 接入方式

把 `qawaitablefuture.h` 复制进工程，在需要的地方 `#include "qawaitablefuture.h"`，并链接 `Qt::Core` 与 `Qt::Concurrent`：

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Concurrent)

qt_add_executable(app main.cpp)
target_link_libraries(app PRIVATE Qt::Core Qt::Concurrent)
```

## 最精简用法

### 1. 等待 `QtConcurrent::run`

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

返回值可以直接丢弃（fire-and-forget）：

```cpp
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    compute();            // 协程跑到第一个 co_await 后返回，之后由事件循环恢复
    return app.exec();
}
```

### 2. 等待 `QFuture<void>`

```cpp
AsyncTask doWork()
{
    co_await QtConcurrent::run([] { QThread::sleep(1); });
    qDebug() << "done";
}
```

### 3. 用 `QPromise` 从子线程回填结果

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

    const int v = co_await future;   // 阻塞式等待 future 完成，不阻塞调用线程
    qDebug() << v;                   // 2024
}
```

### 4. 异常处理

```cpp
AsyncTask onError()
{
    try {
        co_await QtConcurrent::run([]() -> int {
            throw MyException("boom");   // MyException 必须继承 QException
        });
    } catch (const MyException &e) {
        qDebug() << "caught:" << e.message();
    }
}
```

不捕获的异常会进入 `promise_type::unhandled_exception()`，默认只打日志（`TASK_LOG_ENABLED=1` 时可见）并结束协程，之后代码不再执行。

### 5. 取消

```cpp
AsyncTask task = compute();   // 必须持有返回值
task.destroy();               // 销毁仍挂在 co_await 上的协程帧
```

```cpp
AsyncTask cancellable()
{
    co_await QtConcurrent::run([] { QThread::sleep(10); });
    qDebug() << "被取消的话这行不会出现";
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    AsyncTask task = cancellable();
    QTimer::singleShot(2000, &app, [&task] { task.destroy(); });

    return app.exec();
}
```

## AsyncTask 的所有权语义

`AsyncTask` 不是 `unique_ptr`，它只管"取消权"，不管协程的生命周期：

| 行为 | 协程 |
| --- | --- |
| 丢弃返回值 | 继续执行直到自然结束并自行释放帧，之后无法取消 |
| 持有返回值 | 可随时 `destroy()` 提前销毁（取消） |
| `~AsyncTask()` | **不会** `destroy()`，只是不管了 |
| 移动构造 / 移动赋值 | 取消权转移，原对象变空；两侧协程都不受影响 |
| `detach()` | 放弃取消权，协程继续跑到自然结束 |
| `swap()` | 交换取消权 |
| `valid()` | 是否仍持有某个协程 |

要点：**想能取消，就必须接住返回值**；`destroy()` 对已结束、已 `detach()`、默认构造、被移动走的对象都是安全的 no-op，可重复调用。

注意 `destroy()` 只销毁协程帧，**不会**中断 `QtConcurrent::run` 里已经在跑的线程，任务本身会继续跑完（结果被丢弃）。

## 恢复线程与事件循环

`co_await` 挂起后，恢复动作由 `QFutureWatcher::finished` 通过 `Qt::QueuedConnection` 投递到 **创建 awaiter 的线程**。因此：

- 发起 `co_await` 的线程必须有事件循环（`QCoreApplication::exec()` / `QEventLoop`），否则协程永远不恢复
- 不要在无事件循环的 `QThread::run()` 里 await
- 协程帧的 `destroy()` 请与协程所在线程保持一致，不要跨线程调用

## 异常约定

`QtConcurrent::run` 只认 `QException` 及其子类，其它异常会被替换成 `QUnhandledException`。所以自定义异常请继承 `QException`：

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

需要投递任意异常时，用 `QPromise::setException(std::make_exception_ptr(...))`。

## 日志

默认关闭（`TASK_LOG_ENABLED 0`）。打开后每次挂起/恢复/完成都会以红色打印函数名和当前线程：

```cpp
#define TASK_LOG_ENABLED 1     // 必须在 include 之前，或在编译选项里加 -DTASK_LOG_ENABLED=1
#include "qawaitablefuture.h"
```

该开关按编译单元生效。

## 已知限制

- `QFuture` 被取消时 `await_ready()` 会直接返回 `true`，随后 `await_resume()` 仍会调用 `result()`；对取消态 future 取结果需要自行判断 `isCanceled()`
- `destroy()` 只能销毁挂起点上的协程帧，无法停止已提交到线程池的计算
- 重复 `co_await` 同一个 `QFuture` 不会重新等待，会立即返回同样的结果

## 构建与测试

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

`main.cpp` 是六个场景的演示程序（正常返回、两种异常回传、未捕获异常、`destroy()` 取消），`tests/test_qawaitablefuture.cpp` 覆盖了上述所有权与线程行为。

## 文件结构

```
qawaitablefuture.h              # 全部实现：AsyncTask / FutureAwaiter / operator co_await
main.cpp                       # 演示程序
tests/test_qawaitablefuture.cpp # Qt Test 用例
CMakeLists.txt                 # demo 目标
tests/CMakeLists.txt           # 测试目标
```
