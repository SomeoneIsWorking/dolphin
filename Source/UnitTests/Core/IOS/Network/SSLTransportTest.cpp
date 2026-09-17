// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <array>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#ifndef _WIN32
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "Common/SocketContext.h"
#include "Core/IOS/Network/SSLTransport.h"

namespace
{
using Common::INVALID_SOCKET_HANDLE;
using Common::SocketHandle;
namespace Transport = IOS::HLE::SSLTransport;

void CloseSocket(SocketHandle socket)
{
  if (socket == INVALID_SOCKET_HANDLE)
    return;
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

class SSLTransportTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(listener, INVALID_SOCKET_HANDLE);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(listener, 1), 0);
#ifdef _WIN32
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    ASSERT_EQ(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    sender = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_NE(sender, INVALID_SOCKET_HANDLE);
    ASSERT_EQ(connect(sender, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    receiver = accept(listener, nullptr, nullptr);
    ASSERT_NE(receiver, INVALID_SOCKET_HANDLE);
    for (const SocketHandle handle : {sender, receiver})
    {
#ifdef _WIN32
      u_long nonblocking = 1;
      ASSERT_EQ(ioctlsocket(handle, FIONBIO, &nonblocking), 0);
#else
      const int flags = fcntl(handle, F_GETFL, 0);
      ASSERT_GE(flags, 0);
      ASSERT_EQ(fcntl(handle, F_SETFL, flags | O_NONBLOCK), 0);
#endif
    }
  }

  void TearDown() override
  {
    CloseSocket(receiver);
    CloseSocket(sender);
    CloseSocket(listener);
  }

  [[nodiscard]] bool Readable() const
  {
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(receiver, &descriptors);
    timeval timeout{2, 0};
    return select(Common::SelectNfds(receiver), &descriptors, nullptr, nullptr, &timeout) == 1;
  }

  Common::SocketContext context;
  SocketHandle listener = INVALID_SOCKET_HANDLE;
  SocketHandle sender = INVALID_SOCKET_HANDLE;
  SocketHandle receiver = INVALID_SOCKET_HANDLE;
};

TEST_F(SSLTransportTest, BinaryPayloadRetryAndEndOfStream)
{
  constexpr std::array<unsigned char, 5> payload{0, 0xff, 1, 0, 42};
  std::array<unsigned char, payload.size()> received{};
  EXPECT_EQ(Transport::Receive(receiver, received.data(), received.size()),
            MBEDTLS_ERR_SSL_WANT_READ);
  ASSERT_EQ(Transport::Send(sender, payload.data(), payload.size()), int(payload.size()));
  ASSERT_TRUE(Readable());
  ASSERT_EQ(Transport::Receive(receiver, received.data(), received.size()), int(received.size()));
  EXPECT_EQ(received, payload);
  EXPECT_EQ(Transport::Receive(receiver, received.data(), received.size()),
            MBEDTLS_ERR_SSL_WANT_READ);
#ifdef _WIN32
  ASSERT_EQ(shutdown(sender, SD_SEND), 0);
#else
  ASSERT_EQ(shutdown(sender, SHUT_WR), 0);
#endif
  ASSERT_TRUE(Readable());
  EXPECT_EQ(Transport::Receive(receiver, received.data(), received.size()), 0);
}

TEST_F(SSLTransportTest, InvalidContextAndWriteBackpressure)
{
  std::array<unsigned char, 16384> buffer{};
  EXPECT_EQ(Transport::Send(INVALID_SOCKET_HANDLE, buffer.data(), buffer.size()),
            MBEDTLS_ERR_NET_INVALID_CONTEXT);
  EXPECT_EQ(Transport::Receive(INVALID_SOCKET_HANDLE, buffer.data(), buffer.size()),
            MBEDTLS_ERR_NET_INVALID_CONTEXT);
  const int buffer_size = 1024;
  ASSERT_EQ(setsockopt(sender, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buffer_size),
                       sizeof(buffer_size)),
            0);
  bool backpressure = false;
  for (int attempts = 0; attempts < 4096; ++attempts)
  {
    const int result = Transport::Send(sender, buffer.data(), buffer.size());
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      backpressure = true;
      break;
    }
    ASSERT_GT(result, 0);
  }
  EXPECT_TRUE(backpressure) << "64 MiB send budget exhausted without exercising WANT_WRITE";
}

TEST_F(SSLTransportTest, ClosedSocketFailsAndResetIsDistinct)
{
  const linger reset_on_close{1, 0};
  ASSERT_EQ(setsockopt(sender, SOL_SOCKET, SO_LINGER,
                       reinterpret_cast<const char*>(&reset_on_close), sizeof(reset_on_close)),
            0);
  CloseSocket(sender);
  sender = INVALID_SOCKET_HANDLE;
  ASSERT_TRUE(Readable());
  unsigned char byte = 0;
  EXPECT_EQ(Transport::Receive(receiver, &byte, 1), MBEDTLS_ERR_NET_CONN_RESET);
  const SocketHandle closed = receiver;
  CloseSocket(receiver);
  receiver = INVALID_SOCKET_HANDLE;
  EXPECT_EQ(Transport::Receive(closed, &byte, 1), MBEDTLS_ERR_NET_RECV_FAILED);
  EXPECT_EQ(Transport::Send(closed, &byte, 1), MBEDTLS_ERR_NET_SEND_FAILED);
}

#ifndef _WIN32
// The bundled mbedTLS net context is int-sized. POSIX descriptors let it act as an independent
// reference; Windows native identity is covered separately without narrowing a live SOCKET.
TEST_F(SSLTransportTest, MatchesMbedTLSReference)
{
  mbedtls_net_context reference_sender{sender};
  mbedtls_net_context reference_receiver{receiver};
  std::array<unsigned char, 4> payload{9, 0, 0xfe, 3};
  std::array<unsigned char, 4> received{};
  EXPECT_EQ(Transport::Receive(receiver, received.data(), received.size()),
            mbedtls_net_recv(&reference_receiver, received.data(), received.size()));
  ASSERT_EQ(Transport::Send(sender, payload.data(), payload.size()), int(payload.size()));
  ASSERT_TRUE(Readable());
  ASSERT_EQ(mbedtls_net_recv(&reference_receiver, received.data(), received.size()),
            int(payload.size()));
  EXPECT_EQ(received, payload);
  received.fill(0);
  ASSERT_EQ(mbedtls_net_send(&reference_sender, payload.data(), payload.size()),
            int(payload.size()));
  ASSERT_TRUE(Readable());
  ASSERT_EQ(Transport::Receive(receiver, received.data(), received.size()), int(payload.size()));
  EXPECT_EQ(received, payload);
}
#endif
}  // namespace
