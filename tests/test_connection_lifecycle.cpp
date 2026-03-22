#include "address.h"
// test_connection_lifecycle.cpp
// 测试TcpServer连接生命周期，包含连接建立、收发、优雅关闭、统计校验
#include "eventloop.h"
#include "length_field_codec.h"
#include "logger.h"
#include "socket_utils.h"
#include "tcpserver.h"
#include "tests/test_common.h"

using namespace TcFrame;

static uint64_t ExtractStat(const std::string& stats, const std::string& key)
{
    const std::string token = key + "=";
    size_t pos = stats.find(token);
    if (pos == std::string::npos)
    {
        return 0;
    }
    pos += token.size();
    size_t end = stats.find(',', pos);
    const std::string value = (end == std::string::npos) ? stats.substr(pos) : stats.substr(pos, end - pos);
    return static_cast<uint64_t>(std::strtoull(value.c_str(), nullptr, 10));
}

static uint16_t PickPort()
{
    for (uint16_t p = 19000; p < 19100; ++p)
    {
        if (SocketUtils::IsPortAvailable(p))
        {
            return p;
        }
    }
    return 19000;
}

static std::string PackFrame(const std::string& payload)
{
    uint32_t net_len = SocketUtils::HostToNetLong(static_cast<uint32_t>(payload.size()));
    std::string frame;
    frame.resize(sizeof(uint32_t) + payload.size());
    std::memcpy(&frame[0], &net_len, sizeof(uint32_t));
    if (!payload.empty())
    {
        std::memcpy(&frame[sizeof(uint32_t)], payload.data(), payload.size());
    }
    return frame;
}

static std::string RecvExact(SocketType fd, size_t len)
{
    std::string out;
    out.resize(len);
    size_t off = 0;
    while (off < len)
    {
#ifdef _WIN32
        int n = recv(fd, &out[off], static_cast<int>(len - off), 0);
#else
        ssize_t n = recv(fd, &out[off], len - off, 0);
#endif
        if (n <= 0)
        {
            throw std::runtime_error("recv failed");
        }
        off += static_cast<size_t>(n);
    }
    return out;
}

int main()
{
    return TestMain("connection_lifecycle", []() {
        Logger::Instance().Init(LogLevel::ERROR, ".");

        const uint16_t port = PickPort();
        EventLoop loop(false);
        TcpServer server(&loop, Address("127.0.0.1", port), 1);

        std::atomic<int> connected{0};
        std::atomic<int> disconnected{0};

        server.SetConnectionCallback([&](const TcpConnectionPtr& conn) {
            if (conn->IsConnected())
            {
                connected.fetch_add(1);
            }
            else
            {
                disconnected.fetch_add(1);
            }
        });

        auto codec = std::make_shared<LengthFieldCodec>([](const TcpConnectionPtr&, const std::string&) {});
        std::weak_ptr<LengthFieldCodec> weak = codec;
        codec = std::make_shared<LengthFieldCodec>([&weak](const TcpConnectionPtr& conn, const std::string& msg) {
            auto locked = weak.lock();
            if (locked)
            {
                locked->Send(conn, msg);
            }
        });
        weak = codec;

        server.SetMessageCallback([codec](const TcpConnectionPtr& conn, Buffer* buf) {
            codec->OnMessage(conn, buf);
        });

        server.Start();

        std::thread client([port]() {
            SocketType fd = SocketUtils::CreateTcpSocket();
            TEST_ASSERT(fd != INVALID_SOCKET_VALUE);

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = SocketUtils::HostToNetShort(port);
            TEST_ASSERT(SocketUtils::IpV4StrToBin("127.0.0.1", &addr.sin_addr));

            int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
            TEST_ASSERT(rc == 0);

            const std::string frame = PackFrame("lifecycle");
#ifdef _WIN32
            int sent = send(fd, frame.data(), static_cast<int>(frame.size()), 0);
            TEST_ASSERT(sent == static_cast<int>(frame.size()));
#else
            ssize_t sent = send(fd, frame.data(), frame.size(), 0);
            TEST_ASSERT(sent == static_cast<ssize_t>(frame.size()));
#endif

            const std::string header = RecvExact(fd, 4);
            uint32_t net_len = 0;
            std::memcpy(&net_len, header.data(), sizeof(uint32_t));
            uint32_t len = SocketUtils::NetToHostLong(net_len);
            std::string payload = RecvExact(fd, len);
            TEST_ASSERT(payload == "lifecycle");

            SocketUtils::CloseSocket(fd);
        });

        loop.RunAfter(1.0, [&]() {
            server.StopGracefully(1000, [&]() {
                loop.Quit();
            });
        });

        loop.RunAfter(5.0, [&]() {
            loop.Quit();
        });

        loop.Loop();
        client.join();

        const std::string stats = server.GetStats();
        TEST_ASSERT(ExtractStat(stats, "total_connections") >= 1);
        TEST_ASSERT(ExtractStat(stats, "active_connections") == 0);
        TEST_ASSERT(connected.load() >= 1);

        Logger::Instance().Shutdown();
    });
}
