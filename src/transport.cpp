#include "apf/transport.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <cerrno>
#endif

namespace apf {
namespace {

constexpr std::uintptr_t kInvalidSocket = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));

#if defined(_WIN32)
std::atomic<int> g_network_users{0};
std::mutex g_network_mutex;

SOCKET to_socket(std::uintptr_t handle) { return static_cast<SOCKET>(handle); }

Status last_socket_error(const char* what) {
  return failure(ErrorCode::DeviceUnavailable, what, std::to_string(WSAGetLastError()));
}
#else
std::atomic<int> g_network_users{0};
std::mutex g_network_mutex;

int to_socket(std::uintptr_t handle) { return static_cast<int>(handle); }

Status last_socket_error(const char* what) {
  return failure(ErrorCode::DeviceUnavailable, what, std::strerror(errno));
}
#endif

}  // namespace

Status initialize_network() {
  std::lock_guard<std::mutex> lock(g_network_mutex);
  if (g_network_users.fetch_add(1) == 0) {
#if defined(_WIN32)
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
      g_network_users.fetch_sub(1);
      return failure(ErrorCode::DeviceUnavailable, "WSAStartup failed", std::to_string(result));
    }
#endif
  }
  return success();
}

Status shutdown_network() {
  std::lock_guard<std::mutex> lock(g_network_mutex);
  if (g_network_users.fetch_sub(1) == 1) {
#if defined(_WIN32)
    WSACleanup();
#endif
  }
  return success();
}

std::string Endpoint::to_string() const {
  return host + ":" + std::to_string(port);
}

Result<Endpoint> Endpoint::parse(std::string_view text) {
  if (text.empty()) {
    return make_error(ErrorCode::InvalidArgument, "endpoint text is empty");
  }
  Endpoint endpoint;
  const std::size_t colon = text.rfind(':');
  std::string_view port_text;
  if (colon == std::string_view::npos) {
    port_text = text;
    endpoint.host = "127.0.0.1";
  } else {
    std::string_view host_text = text.substr(0, colon);
    if (!host_text.empty() && host_text.front() == '[' && host_text.back() == ']') {
      host_text = host_text.substr(1, host_text.size() - 2);
    }
    endpoint.host = std::string(host_text.empty() ? "127.0.0.1" : host_text);
    port_text = text.substr(colon + 1);
  }
  std::uint32_t port = 0;
  if (port_text.empty() || port_text.size() > 5) {
    return make_error(ErrorCode::InvalidArgument, "endpoint port is malformed",
                      std::string(text));
  }
  for (const char ch : port_text) {
    if (ch < '0' || ch > '9') {
      return make_error(ErrorCode::InvalidArgument, "endpoint port is not numeric",
                        std::string(text));
    }
    port = port * 10 + static_cast<std::uint32_t>(ch - '0');
  }
  if (port > 65535) {
    return make_error(ErrorCode::InvalidArgument, "endpoint port is out of range",
                      std::string(text));
  }
  endpoint.port = static_cast<std::uint16_t>(port);
  return endpoint;
}

TcpSocket::TcpSocket(std::uintptr_t handle) noexcept : handle_(handle) {}

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
  other.handle_ = kInvalidSocket;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidSocket;
  }
  return *this;
}

bool TcpSocket::valid() const noexcept { return handle_ != kInvalidSocket; }

void TcpSocket::close() noexcept {
  if (handle_ == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  closesocket(to_socket(handle_));
#else
  ::close(to_socket(handle_));
#endif
  handle_ = kInvalidSocket;
}

void TcpSocket::shutdown_both() noexcept {
  if (handle_ == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  shutdown(to_socket(handle_), SD_BOTH);
#else
  shutdown(to_socket(handle_), SHUT_RDWR);
#endif
}

void TcpSocket::set_nodelay(bool enabled) noexcept {
  if (handle_ == kInvalidSocket) {
    return;
  }
  const int value = enabled ? 1 : 0;
#if defined(_WIN32)
  (void)setsockopt(to_socket(handle_), IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&value), sizeof(value));
#else
  (void)setsockopt(to_socket(handle_), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value));
#endif
}

Status TcpSocket::send_all(const std::uint8_t* data, std::size_t size) {
  if (handle_ == kInvalidSocket) {
    return failure(ErrorCode::Closed, "socket is not connected");
  }
  std::size_t sent = 0;
  while (sent < size) {
    const std::size_t remaining = size - sent;
    const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 1u << 20));
#if defined(_WIN32)
    const int written = ::send(to_socket(handle_), reinterpret_cast<const char*>(data + sent),
                               chunk, 0);
#else
    const ssize_t written = ::send(to_socket(handle_), data + sent, static_cast<std::size_t>(chunk),
                                   MSG_NOSIGNAL);
#endif
    if (written <= 0) {
      return last_socket_error("socket send failed").error();
    }
    sent += static_cast<std::size_t>(written);
  }
  return success();
}

Result<std::size_t> TcpSocket::recv_some(std::uint8_t* data, std::size_t size) {
  if (handle_ == kInvalidSocket) {
    return make_error(ErrorCode::Closed, "socket is not connected");
  }
  const int chunk = static_cast<int>(std::min<std::size_t>(size, 1u << 20));
#if defined(_WIN32)
  const int received =
      ::recv(to_socket(handle_), reinterpret_cast<char*>(data), chunk, 0);
#else
  const ssize_t received = ::recv(to_socket(handle_), data, static_cast<std::size_t>(chunk), 0);
#endif
  if (received == 0) {
    return static_cast<std::size_t>(0);
  }
  if (received < 0) {
    return last_socket_error("socket receive failed").error();
  }
  return static_cast<std::size_t>(received);
}

Status TcpSocket::recv_exact(std::uint8_t* data, std::size_t size) {
  std::size_t received = 0;
  while (received < size) {
    Result<std::size_t> chunk = recv_some(data + received, size - received);
    if (!chunk.ok()) {
      return chunk.error();
    }
    if (chunk.value() == 0) {
      return failure(ErrorCode::Closed, "peer closed the connection",
                     std::to_string(received) + " of " + std::to_string(size) + " bytes read");
    }
    received += chunk.value();
  }
  return success();
}

std::uintptr_t TcpSocket::native_handle() const noexcept { return handle_; }

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept
    : handle_(other.handle_), endpoint_(other.endpoint_) {
  other.handle_ = kInvalidSocket;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    endpoint_ = other.endpoint_;
    other.handle_ = kInvalidSocket;
  }
  return *this;
}

Result<TcpListener> TcpListener::bind(const Endpoint& endpoint, int backlog) {
  const Status network = initialize_network();
  if (!network.ok()) {
    return network.error();
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string port_text = std::to_string(endpoint.port);
  const int resolved = getaddrinfo(endpoint.host.empty() ? "127.0.0.1" : endpoint.host.c_str(),
                                   port_text.c_str(), &hints, &results);
  if (resolved != 0 || results == nullptr) {
    (void)shutdown_network();
    return make_error(ErrorCode::InvalidArgument, "could not resolve the listen endpoint",
                      endpoint.to_string());
  }
#if defined(_WIN32)
  SOCKET handle = ::socket(results->ai_family, results->ai_socktype, results->ai_protocol);
  if (handle == INVALID_SOCKET) {
    freeaddrinfo(results);
    (void)shutdown_network();
    return make_error(ErrorCode::DeviceUnavailable, "could not create the listening socket");
  }
#else
  int handle = ::socket(results->ai_family, results->ai_socktype, results->ai_protocol);
  if (handle < 0) {
    freeaddrinfo(results);
    (void)shutdown_network();
    return make_error(ErrorCode::DeviceUnavailable, "could not create the listening socket");
  }
#endif
  const int reuse = 1;
#if defined(_WIN32)
  (void)setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                   sizeof(reuse));
#else
  (void)setsockopt(handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
  if (::bind(handle, results->ai_addr, static_cast<int>(results->ai_addrlen)) != 0) {
#if defined(_WIN32)
    closesocket(handle);
#else
    ::close(handle);
#endif
    freeaddrinfo(results);
    (void)shutdown_network();
    return make_error(ErrorCode::DeviceUnavailable, "could not bind the listening socket",
                      endpoint.to_string());
  }
  freeaddrinfo(results);
  if (::listen(handle, backlog) != 0) {
#if defined(_WIN32)
    closesocket(handle);
#else
    ::close(handle);
#endif
    (void)shutdown_network();
    return make_error(ErrorCode::DeviceUnavailable, "could not listen on the socket");
  }
  TcpListener listener;
  listener.handle_ = static_cast<std::uintptr_t>(handle);
  listener.endpoint_ = endpoint;
  if (endpoint.port == 0) {
    sockaddr_in address{};
#if defined(_WIN32)
    int length = sizeof(address);
#else
    socklen_t length = sizeof(address);
#endif
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
      listener.endpoint_.port = ntohs(address.sin_port);
    }
  }
  if (listener.endpoint_.host.empty()) {
    listener.endpoint_.host = "127.0.0.1";
  }
  return listener;
}

Result<TcpSocket> TcpListener::accept() {
  if (handle_ == kInvalidSocket) {
    return make_error(ErrorCode::Closed, "listener is not bound");
  }
#if defined(_WIN32)
  SOCKET accepted = ::accept(to_socket(handle_), nullptr, nullptr);
  if (accepted == INVALID_SOCKET) {
    return make_error(ErrorCode::Closed, "accept failed", std::to_string(WSAGetLastError()));
  }
  TcpSocket socket(static_cast<std::uintptr_t>(accepted));
#else
  const int accepted = ::accept(to_socket(handle_), nullptr, nullptr);
  if (accepted < 0) {
    return make_error(ErrorCode::Closed, "accept failed", std::strerror(errno));
  }
  TcpSocket socket(static_cast<std::uintptr_t>(accepted));
#endif
  socket.set_nodelay(true);
  return socket;
}

void TcpListener::close() noexcept {
  if (handle_ == kInvalidSocket) {
    return;
  }
#if defined(_WIN32)
  closesocket(to_socket(handle_));
#else
  ::close(to_socket(handle_));
#endif
  handle_ = kInvalidSocket;
  (void)shutdown_network();
}

bool TcpListener::valid() const noexcept { return handle_ != kInvalidSocket; }

Result<TcpSocket> connect_to(const Endpoint& endpoint) {
  const Status network = initialize_network();
  if (!network.ok()) {
    return network.error();
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string port_text = std::to_string(endpoint.port);
  const int resolved = getaddrinfo(endpoint.host.empty() ? "127.0.0.1" : endpoint.host.c_str(),
                                   port_text.c_str(), &hints, &results);
  if (resolved != 0 || results == nullptr) {
    (void)shutdown_network();
    return make_error(ErrorCode::InvalidArgument, "could not resolve the coordinator endpoint",
                      endpoint.to_string());
  }
#if defined(_WIN32)
  SOCKET handle = ::socket(results->ai_family, results->ai_socktype, results->ai_protocol);
  if (handle == INVALID_SOCKET) {
    freeaddrinfo(results);
    (void)shutdown_network();
    return make_error(ErrorCode::DeviceUnavailable, "could not create a client socket");
  }
  const int connected = ::connect(handle, results->ai_addr, static_cast<int>(results->ai_addrlen));
#else
  const int handle = ::socket(results->ai_family, results->ai_socktype, results->ai_protocol);
  if (handle < 0) {
    freeaddrinfo(results);
    (void)shutdown_network();
    return make_error(ErrorCode::DeviceUnavailable, "could not create a client socket");
  }
  const int connected = ::connect(handle, results->ai_addr, results->ai_addrlen);
#endif
  freeaddrinfo(results);
  if (connected != 0) {
#if defined(_WIN32)
    closesocket(handle);
#else
    ::close(handle);
#endif
    (void)shutdown_network();
    return make_error(ErrorCode::DeviceUnavailable, "could not connect to the coordinator",
                      endpoint.to_string());
  }
  TcpSocket socket(static_cast<std::uintptr_t>(handle));
  socket.set_nodelay(true);
  return socket;
}

}  // namespace apf
