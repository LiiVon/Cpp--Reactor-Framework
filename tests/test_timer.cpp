#include "eventloop.h"
// test_timer.cpp
// 测试EventLoop定时任务功能，验证周期任务、一次性任务、取消任务等逻辑
#include "logger.h"
#include "tests/test_common.h"

using namespace TcFrame;

int main()
{
    return TestMain("timer", []() {
        Logger::Instance().Init(LogLevel::ERROR, ".");

        EventLoop loop(false);
        std::atomic<int> repeat_count{0};
        std::atomic<bool> after_fired{false};
        std::atomic<bool> cancelled_fired{false};
        EventLoop::TimerId repeat_id = 0;

        repeat_id = loop.RunEvery(0.02, [&]() {
            int now = repeat_count.fetch_add(1) + 1;
            if (now >= 3)
            {
                bool ok = loop.Cancel(repeat_id);
                TEST_ASSERT(ok);
                loop.Quit();
            }
        });

        loop.RunAfter(0.03, [&]() {
            after_fired.store(true);
        });

        EventLoop::TimerId cancelled_id = loop.RunAfter(0.15, [&]() {
            cancelled_fired.store(true);
        });
        bool cancelled = loop.Cancel(cancelled_id);
        TEST_ASSERT(cancelled);

        // Guard timer to avoid hanging tests if repeat timer fails.
        loop.RunAfter(1.0, [&]() {
            loop.Quit();
        });

        loop.Loop();

        TEST_ASSERT(after_fired.load());
        TEST_ASSERT(repeat_count.load() >= 3);
        TEST_ASSERT(!cancelled_fired.load());

        Logger::Instance().Shutdown();
    });
}
