#include "logger.h"
#include "eventloop.h"
#include "tcpserver.h"
#include "address.h"
#include "length_field_codec.h"

using namespace TcFrame;

namespace
{
    EventLoop* g_main_loop = nullptr;
    std::atomic<bool> g_stop_requested(false);

    void HandleSignal(int signo)
    {
        if (signo == SIGINT && g_main_loop != nullptr)
        {
            g_stop_requested.store(true);
        }
    }

    uint16_t ParsePort(const char* arg)
    {
        if (arg == nullptr)
        {
            return 9000;
        }

        const int port = std::atoi(arg);
        if (port <= 0 || port > 65535)
        {
            return 9000;
        }

        return static_cast<uint16_t>(port);
    }

    size_t ParseThreads(const char* arg)
    {
        if (arg == nullptr)
        {
            return 4;
        }

        const int threads = std::atoi(arg);
        if (threads < 0)
        {
            return 4;
        }

        return static_cast<size_t>(threads);
    }
}

int main(int argc, char* argv[])
{
    const uint16_t port = ParsePort(argc > 1 ? argv[1] : nullptr);
    const size_t threads = ParseThreads(argc > 2 ? argv[2] : nullptr);

    Logger::Instance().Init(LogLevel::DEBUG, ".");
    LOG_INFO("EchoServer booting, port=" + std::to_string(port) + ", threads=" + std::to_string(threads));

    EventLoop main_loop(false);
    g_main_loop = &main_loop;

    Address listen_addr("0.0.0.0", port);
    TcpServer server(&main_loop, listen_addr, threads);
    server.SetIdleTimeoutSeconds(120);
    server.SetHighWaterMarkBytes(256 * 1024);
    server.SetHighWaterMarkCallback([](const TcpConnectionPtr& conn, size_t pending_bytes) {
        LOG_WARN("[backpressure] high watermark reached, conn=" + conn->GetName() + ", pending_bytes=" + std::to_string(pending_bytes));
    });
    server.EnablePrometheusMetricsExport("./tcframe_metrics.prom", 5);

    std::weak_ptr<LengthFieldCodec> codec_weak;
    auto codec = std::make_shared<LengthFieldCodec>([&codec_weak](const TcpConnectionPtr& conn, const std::string& message) {
        if (LengthFieldCodec::HandleHeartbeat(conn, message))
        {
            return;
        }
        auto codec_locked = codec_weak.lock();
        if (codec_locked)
        {
            codec_locked->Send(conn, message);
        }
    });
    codec_weak = codec;

    server.SetConnectionCallback([](const TcpConnectionPtr& conn) {
        if (conn->IsConnected())
        {
            LOG_INFO("[connected] " + conn->GetName());
        }
        else
        {
            LOG_INFO("[disconnected] " + conn->GetName());
        }
    });

    server.SetMessageCallback([codec](const TcpConnectionPtr& conn, Buffer* buffer) {
        codec->OnMessage(conn, buffer);
    });

    server.SetWriteCompleteCallback([](const TcpConnectionPtr& conn) {
        LOG_DEBUG("[write complete] " + conn->GetName());
    });

    std::signal(SIGINT, HandleSignal);

    main_loop.RunEvery(10.0, [&server]() {
        LOG_INFO("[server stats] " + server.GetStats());
    });

    main_loop.RunEvery(0.2, [&server, &main_loop]() {
        if (!g_stop_requested.exchange(false))
        {
            return;
        }

        LOG_WARN("SIGINT received, start graceful shutdown...");
        server.StopGracefully(5000, [&main_loop]() {
            LOG_INFO("server graceful shutdown complete, quit main loop");
            main_loop.Quit();
        });
    });

    server.Start();
    main_loop.Loop();

    g_main_loop = nullptr;
    Logger::Instance().Shutdown();
    return 0;
}