// tests/test_qawaitablefuture.cpp
// AsyncTask / co_await QFuture 的行为测试（Qt Test）
//
// 覆盖：
//   1. QtConcurrent::run 正常返回值
//   2. 子线程 QPromise 兑现结果
//   3. QtConcurrent::run 抛异常，co_await 处能 catch
//   4. QPromise::setException 抛异常，co_await 处能 catch
//   5. 未捕获异常进 promise_type::unhandled_exception
//   6. destroy() 能取消仍挂起的协程
//   7. 恢复后回到调用方线程
//   8. 拷贝被禁用、移动可用（编译期特性）
//   9. 移动构造后协程仍存活、可被新对象取消
//   10. 移动赋值只转移取消权，不取消目标原有协程
//   11. 自移动赋值安全
//   12. 默认构造 / 被移动走之后 destroy() 都是 no-op
//   13. detach() 放手但不取消
//   14. swap() 交换取消权
//   15. 放进容器（依赖移动构造）
//   16. QtFuture::makeReadyValueFuture：已完成 future 不挂起，同步返回结果
//   17. 已完成 future 携带 QString 这类非平凡类型
//   18. 已完成 future + 真正异步 future 混用，恢复线程不变
//   19. 已完成 future：协程已经结束，destroy() 是 no-op
//
// 注意：头文件里的日志开关是按编译单元生效的，
//       这里在 include 之前把 TASK_LOG_ENABLED 打开，才能断言 unhandled_exception 的输出。

#define TASK_LOG_ENABLED 1
#include "qawaitablefuture.h"

#include <QtConcurrent>
#include <QCoreApplication>
#include <QException>
#include <QPromise>
#include <QThread>
#include <QtTest>

#include <exception>
#include <memory>
#include <type_traits>
#include <vector>

namespace {

// 自定义异常：必须继承 QException，QtConcurrent/QFuture 才会原样存下来
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

struct State {
    bool started = false;
    bool done = false;
    bool caught = false;
    int value = 0;
    QString message;
    QThread *resumeThread = nullptr;
};

// ---- 被测协程 ----

AsyncTask awaitRunResult(State *s)
{
    s->started = true;
    s->value = co_await QtConcurrent::run([] {
        QThread::msleep(200);
        return 42;
    });
    s->resumeThread = QThread::currentThread();
    s->done = true;
}

AsyncTask awaitPromiseResult(State *s)
{
    QPromise<int> promise;
    promise.start();
    QFuture<int> future = promise.future();

    (void)QtConcurrent::run([&promise] {
        QThread::msleep(200);
        promise.addResult(2024);
        promise.finish();
    });

    s->started = true;
    s->value = co_await future;
    s->resumeThread = QThread::currentThread();
    s->done = true;
}

AsyncTask awaitRunException(State *s)
{
    s->started = true;
    try {
        const int value = co_await QtConcurrent::run([] {
            QThread::msleep(200);
            throw MyException("boom from QtConcurrent::run");
            return 0; // 不可达，只为推导成 int
        });
        Q_UNUSED(value);
    } catch (const MyException &e) {
        s->caught = true;
        s->message = e.message();
    }
    s->resumeThread = QThread::currentThread();
    s->done = true;
}

AsyncTask awaitPromiseException(State *s)
{
    QPromise<void> promise;
    promise.start();
    QFuture<void> future = promise.future();

    (void)QtConcurrent::run([&promise] {
        QThread::msleep(200);
        promise.setException(std::make_exception_ptr(MyException("boom from QPromise")));
        promise.finish();
    });

    s->started = true;
    try {
        co_await future;
    } catch (const MyException &e) {
        s->caught = true;
        s->message = e.message();
    }
    s->done = true;
}

// 已完成 future：await_ready() 为 true，co_await 处不挂起，调用返回时协程已结束
AsyncTask awaitReadyValue(State *s)
{
    s->started = true;
    s->value = co_await QtFuture::makeReadyValueFuture(7);
    s->resumeThread = QThread::currentThread();
    s->done = true;
}

// 已完成 future 携带非平凡类型
AsyncTask awaitReadyText(State *s)
{
    s->started = true;
    s->message = co_await QtFuture::makeReadyValueFuture(QStringLiteral("hello future"));
    s->resumeThread = QThread::currentThread();
    s->done = true;
}

// 已完成 future 之后再等一个真正异步的 future，恢复线程应与发起线程一致
AsyncTask awaitReadyThenRun(State *s)
{
    s->started = true;
    const int readyPart = co_await QtFuture::makeReadyValueFuture(10);
    const int asyncPart = co_await QtConcurrent::run([] {
        QThread::msleep(200);
        return 32;
    });
    s->value = readyPart + asyncPart;
    s->resumeThread = QThread::currentThread();
    s->done = true;
}

// 故意不 catch：异常应进 promise_type::unhandled_exception，且后续代码不再执行
AsyncTask awaitUncaught(State *s)
{
    s->started = true;
    const int value = co_await QtConcurrent::run([] {
        QThread::msleep(200);
        throw MyException("boom, nobody catches me");
        return 0;
    });
    Q_UNUSED(value);
    s->done = true; // 正常情况不会走到这
}

// 挂在一个永远不兑现的 promise 上，用来测 destroy() 取消
AsyncTask suspendForever(const std::shared_ptr<QPromise<int>> &promise, State *s)
{
    QFuture<int> future = promise->future();
    s->started = true;
    s->value = co_await future;
    s->done = true;
}

// ---- 日志捕获（测 unhandled_exception）----

QString g_captured;
QtMessageHandler g_oldHandler = nullptr;

void captureHandler(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    g_captured += msg;
}

} // namespace

class TestAwaitableFuture : public QObject
{
    Q_OBJECT

private slots:
    void cleanup()
    {
        if (g_oldHandler) {
            qInstallMessageHandler(g_oldHandler);
            g_oldHandler = nullptr;
        }
        g_captured.clear();
    }

    // 1. 丢弃返回值也能跑完（fire-and-forget），并拿到结果
    void testRunReturnsValue()
    {
        State s;
        awaitRunResult(&s);

        QTRY_VERIFY_WITH_TIMEOUT(s.done, 5000);
        QVERIFY(s.started);
        QCOMPARE(s.value, 42);
    }

    // 2. 子线程 QPromise 兑现结果
    void testPromiseResult()
    {
        State s;
        awaitPromiseResult(&s);

        QTRY_VERIFY_WITH_TIMEOUT(s.done, 5000);
        QCOMPARE(s.value, 2024);
    }

    // 3. QtConcurrent::run 抛异常
    void testRunException()
    {
        State s;
        awaitRunException(&s);

        QTRY_VERIFY_WITH_TIMEOUT(s.done, 5000);
        QVERIFY(s.caught);
        QCOMPARE(s.message, QStringLiteral("boom from QtConcurrent::run"));
    }

    // 4. QPromise::setException 抛异常
    void testPromiseException()
    {
        State s;
        awaitPromiseException(&s);

        QTRY_VERIFY_WITH_TIMEOUT(s.done, 5000);
        QVERIFY(s.caught);
        QCOMPARE(s.message, QStringLiteral("boom from QPromise"));
    }

    // 5. 未捕获异常 -> unhandled_exception（靠日志断言），且之后的代码不执行
    void testUnhandledException()
    {
        g_oldHandler = qInstallMessageHandler(captureHandler);

        State s;
        awaitUncaught(&s);

        // 只断言走了 unhandled_exception：MyException 只提供 message()，
        // 头文件里 e.what() 拿到的是 std::exception 的默认文本
        QTRY_VERIFY_WITH_TIMEOUT(g_captured.contains(QStringLiteral("Unhandled exception")), 5000);
        QVERIFY(s.started);
        QVERIFY(!s.done); // 异常后协程直接结束，不会继续往下走
    }

    // 6. 持有 AsyncTask 才能 destroy() 取消；取消后再兑现 promise 也不会恢复协程
    void testDestroyCancelsSuspendedCoroutine()
    {
        auto promise = std::make_shared<QPromise<int>>();
        promise->start();

        State s;
        AsyncTask task = suspendForever(promise, &s);
        QTRY_VERIFY_WITH_TIMEOUT(s.started, 1000);
        QVERIFY(!s.done);

        task.destroy();

        promise->addResult(7);
        promise->finish();
        QTest::qWait(500);

        QVERIFY(!s.done);
        QCOMPARE(s.value, 0);

        task.destroy(); // 重复 destroy 是安全的 no-op
    }

    // 7. 恢复后回到调用方线程（这里是主线程）
    void testResumeHappensOnCallingThread()
    {
        State s;
        awaitRunResult(&s);

        QTRY_VERIFY_WITH_TIMEOUT(s.done, 5000);
        QCOMPARE(s.resumeThread, QThread::currentThread());
        QCOMPARE(s.resumeThread, QCoreApplication::instance()->thread());
    }

    // 8. 拷贝必须被禁用，移动必须可用（编译期断言）
    void testTypeTraits()
    {
        static_assert(!std::is_copy_constructible_v<AsyncTask>);
        static_assert(!std::is_copy_assignable_v<AsyncTask>);
        static_assert(std::is_move_constructible_v<AsyncTask>);
        static_assert(std::is_move_assignable_v<AsyncTask>);
        static_assert(std::is_nothrow_move_constructible_v<AsyncTask>);
        static_assert(std::is_nothrow_move_assignable_v<AsyncTask>);
        QVERIFY(true); // 断言都在编译期完成，这里只是让槽函数有内容
    }

    // 9. 移动构造：协程帧不受影响，仍然能被恢复
    void testMoveConstructKeepsCoroutineAlive()
    {
        auto promise = std::make_shared<QPromise<int>>();
        promise->start();

        State s;
        AsyncTask first = suspendForever(promise, &s);
        QTRY_VERIFY_WITH_TIMEOUT(s.started, 1000);

        AsyncTask second = std::move(first);

        promise->addResult(9);
        promise->finish();

        QTRY_VERIFY_WITH_TIMEOUT(s.done, 5000);
        QCOMPARE(s.value, 9);

        second.destroy(); // 已结束，no-op
        first.destroy();  // 被移动走，state_ 为空，no-op
    }

    // 10. 移动构造：所有权转到新对象，用新对象 destroy() 能取消
    void testMoveConstructTransfersOwnership()
    {
        auto promise = std::make_shared<QPromise<int>>();
        promise->start();

        State s;
        AsyncTask first = suspendForever(promise, &s);
        QTRY_VERIFY_WITH_TIMEOUT(s.started, 1000);

        AsyncTask second = std::move(first);
        second.destroy();

        promise->addResult(1);
        promise->finish();
        QTest::qWait(300);

        QVERIFY(!s.done);
        QCOMPARE(s.value, 0);

        first.destroy(); // 被移动走后依然是安全的 no-op
    }

    // 11. 移动赋值：只转移取消权，不取消目标原有协程
    void testMoveAssignDetachesPrevious()
    {
        auto promise1 = std::make_shared<QPromise<int>>();
        auto promise2 = std::make_shared<QPromise<int>>();
        promise1->start();
        promise2->start();

        State s1;
        State s2;
        AsyncTask a = suspendForever(promise1, &s1);
        AsyncTask b = suspendForever(promise2, &s2);
        QTRY_VERIFY_WITH_TIMEOUT(s1.started && s2.started, 1000);

        b = std::move(a); // b 放弃 s2 的取消权，s2 继续跑

        promise2->addResult(2);
        promise2->finish();
        QTRY_VERIFY_WITH_TIMEOUT(s2.done, 5000); // 放手 ≠ 取消
        QCOMPARE(s2.value, 2);

        promise1->addResult(1);
        promise1->finish();
        QTRY_VERIFY_WITH_TIMEOUT(s1.done, 5000); // b 现在持有 s1，照常恢复
        QCOMPARE(s1.value, 1);
    }

    // 12. 自移动赋值：实现里有 this != &other 保护，不能把自己销毁掉
    void testSelfMoveAssignIsSafe()
    {
        auto promise = std::make_shared<QPromise<int>>();
        promise->start();

        State s;
        AsyncTask task = suspendForever(promise, &s);
        QTRY_VERIFY_WITH_TIMEOUT(s.started, 1000);

        AsyncTask &self = task;
        task = std::move(self);

        promise->addResult(5);
        promise->finish();
        QTRY_VERIFY_WITH_TIMEOUT(s.done, 5000);
        QCOMPARE(s.value, 5);
    }

    // 13. 默认构造 / 被移动走的对象：destroy() 是安全的 no-op，valid() 为 false
    void testEmptyTaskIsSafe()
    {
        AsyncTask empty;
        QVERIFY(!empty.valid());
        empty.destroy();

        AsyncTask moved = std::move(empty);
        moved.destroy();
        empty.destroy(); // 源对象已被掏空
        QVERIFY(!empty.valid());
    }

    // 14. detach()：放弃取消权但协程继续跑，之后 destroy() 也管不着了
    void testDetachKeepsCoroutineRunning()
    {
        auto promise = std::make_shared<QPromise<int>>();
        promise->start();

        State s;
        AsyncTask task = suspendForever(promise, &s);
        QTRY_VERIFY_WITH_TIMEOUT(s.started, 1000);
        QVERIFY(task.valid());

        task.detach();
        QVERIFY(!task.valid());

        task.destroy(); // 已经放手，取消不了

        promise->addResult(3);
        promise->finish();
        QTRY_VERIFY_WITH_TIMEOUT(s.done, 5000); // 协程照常完成
        QCOMPARE(s.value, 3);
    }

    // 15. swap()：交换取消权，两个协程本身都不受影响
    void testSwapExchangesOwnership()
    {
        auto promise1 = std::make_shared<QPromise<int>>();
        auto promise2 = std::make_shared<QPromise<int>>();
        promise1->start();
        promise2->start();

        State s1;
        State s2;
        AsyncTask a = suspendForever(promise1, &s1);
        AsyncTask b = suspendForever(promise2, &s2);
        QTRY_VERIFY_WITH_TIMEOUT(s1.started && s2.started, 1000);

        swap(a, b); // 现在 a 持有 s2、b 持有 s1

        a.destroy(); // 取消的是 s2
        promise2->addResult(2);
        promise2->finish();
        QTest::qWait(300);
        QVERIFY(!s2.done);

        promise1->addResult(1);
        promise1->finish();
        QTRY_VERIFY_WITH_TIMEOUT(s1.done, 5000);
        QCOMPARE(s1.value, 1);
    }

    // 16. 放进容器：vector 扩容依赖移动构造，且不影响协程
    void testMoveIntoVector()
    {
        State states[3];
        std::vector<std::shared_ptr<QPromise<int>>> promises;
        std::vector<AsyncTask> tasks;

        for (int i = 0; i < 3; ++i) {
            auto promise = std::make_shared<QPromise<int>>();
            promise->start();
            promises.push_back(promise);
            tasks.push_back(suspendForever(promise, &states[i])); // 移动进容器
            QVERIFY(states[i].started);
        }

        for (int i = 0; i < 3; ++i) {
            promises[i]->addResult(i + 1);
            promises[i]->finish();
        }

        for (int i = 0; i < 3; ++i) {
            QTRY_VERIFY_WITH_TIMEOUT(states[i].done, 5000);
            QCOMPARE(states[i].value, i + 1);
        }
    }

    // 16. makeReadyValueFuture 已完成：co_await 不挂起，调用返回时协程就已经跑完
    void testReadyValueResumeSynchronously()
    {
        State s;
        awaitReadyValue(&s);

        // 没有挂起，因此不需要 QTRY / 事件循环，返回即完成
        QVERIFY(s.started);
        QVERIFY(s.done);
        QCOMPARE(s.value, 7);
        QCOMPARE(s.resumeThread, QThread::currentThread());
    }

    // 17. 已完成 future 携带 QString，值完整带回
    void testReadyValueWithString()
    {
        State s;
        awaitReadyText(&s);

        QVERIFY(s.done);
        QCOMPARE(s.message, QStringLiteral("hello future"));
        QCOMPARE(s.resumeThread, QThread::currentThread());
    }

    // 18. 已完成 future 与真正异步 future 混用，恢复线程仍是发起线程
    void testReadyValueThenAsync()
    {
        State s;
        AsyncTask task = awaitReadyThenRun(&s);

        QVERIFY(s.started);
        QTRY_VERIFY_WITH_TIMEOUT(s.done, 5000);
        QCOMPARE(s.value, 42);
        QCOMPARE(s.resumeThread, QThread::currentThread());

        task.destroy(); // 已结束，no-op
    }

    // 19. 已完成 future：协程早已结束，valid() 仍为真但 destroy() 是安全的 no-op
    void testReadyValueDestroyIsNoOp()
    {
        State s;
        AsyncTask task = awaitReadyValue(&s);

        QVERIFY(s.done);
        QVERIFY(task.valid());

        task.destroy(); // finished == true，直接返回
        task.destroy(); // 可重复调用
        QCOMPARE(s.value, 7);
    }
};

QTEST_MAIN(TestAwaitableFuture)

#include "test_qawaitablefuture.moc"
