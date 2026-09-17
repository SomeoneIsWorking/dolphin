// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/IOS/Network/SSLTransport.h"

#include <algorithm>
#include <cerrno>
#include <limits>

#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#ifndef _WIN32
#include <sys/socket.h>
#endif

#include "Common/Network.h"

namespace IOS::HLE::SSLTransport
{
namespace
{
int TranslateError(bool writing)
{
  const int retry = writing ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_WANT_READ;
#ifdef _WIN32
  const int error = WSAGetLastError();
  if (error == WSAEWOULDBLOCK)
    return retry;
  if (error == WSAECONNRESET)
    return MBEDTLS_ERR_NET_CONN_RESET;
#else
  const int error = errno;
  if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR)
    return retry;
  if (error == EPIPE || error == ECONNRESET)
    return MBEDTLS_ERR_NET_CONN_RESET;
#endif
  return writing ? MBEDTLS_ERR_NET_SEND_FAILED : MBEDTLS_ERR_NET_RECV_FAILED;
}

int TransferLength(std::size_t length)
{
  // Both the Winsock byte-count argument and the mbedTLS callback result are int-sized.
  return static_cast<int>(std::min(length, std::size_t{std::numeric_limits<int>::max()}));
}
}  // namespace

int Send(Common::SocketHandle socket, const unsigned char* data, std::size_t length)
{
  if (socket == Common::INVALID_SOCKET_HANDLE)
    return MBEDTLS_ERR_NET_INVALID_CONTEXT;
  const auto result =
      send(socket, reinterpret_cast<const char*>(data), TransferLength(length), Common::SEND_FLAGS);
  return result < 0 ? TranslateError(true) : static_cast<int>(result);
}

int Receive(Common::SocketHandle socket, unsigned char* data, std::size_t length)
{
  if (socket == Common::INVALID_SOCKET_HANDLE)
    return MBEDTLS_ERR_NET_INVALID_CONTEXT;
  const auto result = recv(socket, reinterpret_cast<char*>(data), TransferLength(length), 0);
  return result < 0 ? TranslateError(false) : static_cast<int>(result);
}
}  // namespace IOS::HLE::SSLTransport
