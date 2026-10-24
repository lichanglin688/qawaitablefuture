// main.cpp
// 演示：用 async::Task 协程 co_await QFuture
//
//   demo1  QtConcurrent::run 正常返回值
//   demo2  QtConcurrent::run 抛异常（QException 子类，异常经 QFuture 带回来）
//   demo3  子线程里用 QPromise 兑现结果，主线程 co_await
//   demo4  子线程里用 QPromise::setException 抛异常，主线程 co_await 时 catch
//   demo5  不 catch，异常最终进 promise_type::unhandled_exception
//   demo6  被外部 destroy() 取消，演示 async::Task 的所有权语义
//   demo7  QtFuture::makeReadyValueFuture 完成的 future：co_await 处不挂起，同步往下跑
//
// 说明：QtConcurrent::run 只认 QException（及其子类），
//       其它类型的异常会被替换成 QUnhandledException；
//       所以自定义异常请继承 QException，或用 QPromise::setException 自行投递。
//
// async::Task 对调用方的意义：它是协程句柄的所有权包装（像 unique_ptr 一样管着协程帧）
//   - 不接收返回值（fire-and-forget）：协程自己跑到结束并释放帧，代价是调用方无法取消它；
//   - 接收并持有：可以在外部调用 destroy()，把一个还挂在 co_await 上的协程提前销毁（取消）。
//     注意 ~async::Task() 不会自动 destroy()，忘记持有就会一直挂在内存里等 future。

#include "qawaitablefuture.h"

#include <QException>
#include <QPromise>
#include <QThread>
#include <QTimer>
#include <QtConcurrent>

#include <exception>

namespace {

// 自定义异常：继承 QException，才能被 QFuture / QtConcurrent 正确存储与重抛
class MyException : public QException {
public:
    explicit MyException(QString message)
        : m_message(std::move(message))
    {}

    void raise() const override { throw *this; }
    MyException *clone() const override { return new MyException(*this); }

    const QString &message() const noexcept { return m_message; }

private:
    QString m_message;
};

void log(const QString &text)
{
    qDebug().nospace().noquote()
        << text << "  [thread: " << QThread::currentThread() << "]";
}

// demo1：最基础的用法，子线程算完，回到发起线程继续
async::Task demoRunOk()
{
    log("demo1 begin");

    const int value = co_await QtConcurrent::run([] {
        QThread::sleep(1);
        log("demo1 worker computing");
        return 42;
    });

    log(QString("demo1 end, result = %1").arg(value));
}

// demo2：QtConcurrent::run 里抛异常，co_await 处直接 try/catch
async::Task demoRunThrow()
{
    log("demo2 begin");

    try {
        const int value = co_await QtConcurrent::run([] {
            QThread::sleep(1);
            log("demo2 worker throwing");
            throw MyException("boom from QtConcurrent::run");
            return 0; // 不可达，只为让返回值推导成 int
        });
        log(QString("demo2 never reaches here, result = %1").arg(value));
    } catch (const MyException &e) {
        log(QString("demo2 caught MyException: %1").arg(e.message()));
    } catch (const std::exception &e) {
        log(QString("demo2 caught std::exception: %1").arg(e.what()));
    } catch (...) {
        log("demo2 caught unknown exception");
    }
}

// demo3：子线程持有 QPromise，算完后 addResult + finish，主线程 await 对应的 future
async::Task demoPromiseOk()
{
    log("demo3 begin");

    QPromise<int> promise;
    promise.start();
    QFuture<int> future = promise.future();

    (void)QtConcurrent::run([&promise] {
        QThread::sleep(1);
        log("demo3 worker addResult");
        promise.addResult(2024);
        promise.finish();
    });

    const int value = co_await future;
    log(QString("demo3 end, promise result = %1").arg(value));
}

// demo4：子线程里 setException，异常随 future 回到 co_await 处
async::Task demoPromiseThrow()
{
    log("demo4 begin");

    QPromise<void> promise;
    promise.start();
    QFuture<void> future = promise.future();

    (void)QtConcurrent::run([&promise] {
        QThread::sleep(1);
        log("demo4 worker setException");
        promise.setException(std::make_exception_ptr(MyException("boom from QPromise")));
        promise.finish();
    });

    try {
        co_await future;
    } catch (const MyException &e) {
        log(QString("demo4 caught MyException: %1").arg(e.message()));
    } catch (const std::exception &e) {
        log(QString("demo4 caught std::exception: %1").arg(e.what()));
    } catch (...) {
        log("demo4 caught unknown exception");
    }
}

// demo5：不做任何 catch，异常冒泡到 promise_type::unhandled_exception
async::Task demoUncaught()
{
    log("demo5 begin");

    const int value = co_await QtConcurrent::run([] {
        QThread::sleep(1);
        log("demo5 worker throwing");
        throw MyException("boom, nobody catches me");
        return 0; // 不可达，只为让返回值推导成 int
    });

    log(QString("demo5 never reaches here, result = %1").arg(value));
}

// demo6：故意跑很久，交给调用方在外部 destroy() 取消
async::Task demoCancellable()
{
    log("demo6 begin, 我会睡 10 秒，正常等不到");

    const int value = co_await QtConcurrent::run([] {
        QThread::sleep(10);
        return -1;
    });

    log(QString("demo6 end, result = %1（被取消的话这行不会出现）").arg(value));
}

// demo7：QtFuture::makeReadyValueFuture 造出来的 future 一出生就是完成态
//         await_ready() 直接返回 true，co_await 处不会挂起，协程同步往下跑，
//         既不创建 QFutureWatcher，也不依赖事件循环
async::Task demoReadyValue()
{
    log("demo7 begin");

    const QString tag = co_await QtFuture::makeReadyValueFuture(QStringLiteral("ready"));
    log(QString("demo7 第一个 co_await 同步返回，tag = %1").arg(tag));

    // 再等一个真正异步的 future：这一次才会挂起，并由事件循环恢复到本线程
    const int value = co_await QtConcurrent::run([] {
        QThread::msleep(300);
        return 1234;
    });

    log(QString("demo7 end, ready value = %1").arg(value));
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    log("main start");

    // 丢弃返回值：fire-and-forget。协程在调用线程同步跑到第一个 co_await，
    // 之后由 future 的完成事件驱动恢复，跑完自己释放协程帧。
    demoRunOk();
    demoRunThrow();
    demoPromiseOk();
    demoPromiseThrow();
    demoUncaught();
    demoReadyValue();

    // 持有返回值：async::Task 就是这段协程的所有权，拿着它才能提前 destroy()（取消）
    async::Task cancelable = demoCancellable();
    QTimer::singleShot(2000, &app, [&cancelable] {
        log("main: 2 秒到了，销毁还挂着的 demo6 协程");
        cancelable.destroy(); // 已结束的协程是 no-op，这里它还在 co_await 上
    });

    // demo1~5 都在 1 秒左右结束，留足时间后退出；也可以改成每个 demo 结束时自行 quit
    QTimer::singleShot(5000, &app, &QCoreApplication::quit);

    return app.exec();
}
