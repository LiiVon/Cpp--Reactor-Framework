#include "buffer.h"
// test_codec.cpp
// 测试LengthFieldCodec的拆包粘包处理能力，验证帧完整性和回调触发
#include "length_field_codec.h"
#include "tests/test_common.h"

using namespace TcFrame;

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

int main()
{
    return TestMain("codec", []() {
        std::vector<std::string> decoded;
        LengthFieldCodec codec([&decoded](const TcpConnectionPtr&, const std::string& message) {
            decoded.push_back(message);
        });

        Buffer buf;
        const std::string f1 = PackFrame("hello");
        const std::string f2 = PackFrame("world");

        // Partial frame should not trigger callback.
        buf.Append(f1.data(), 2);
        codec.OnMessage(TcpConnectionPtr{}, &buf);
        TEST_ASSERT(decoded.empty());

        // Complete first frame + second frame in one shot.
        buf.Append(f1.data() + 2, f1.size() - 2);
        buf.Append(f2);
        codec.OnMessage(TcpConnectionPtr{}, &buf);

        TEST_ASSERT(decoded.size() == 2);
        TEST_ASSERT(decoded[0] == "hello");
        TEST_ASSERT(decoded[1] == "world");
        TEST_ASSERT(buf.ReadableBytes() == 0);
    });
}
