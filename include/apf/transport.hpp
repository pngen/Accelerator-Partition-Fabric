#pragma once

#include "apf/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace apf {

/// Process-wide socket subsystem lifetime. Safe to call repeatedly; the
/// runtime calls it internally, so embedders never have to.
Status initialize_network();
Status shutdown_network();

struct Endpoint {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};

  std::string to_string() const;
  /// Accepts "host:port", "[v6]:port" and bare "port" forms. Port 0 is legal
  /// and means "let the operating system choose".
  static Result<Endpoint> parse(std::string_view text);
  bool operator==(const Endpoint& other) const noexcept {
    return host == other.host && port == other.port;
  }
};

/// Blocking TCP socket. No timeouts are used anywhere: a peer that stops
/// responding is detected by connection close, which is the same event the
/// operating system reports for process death.
class TcpSocket {
 public:
  TcpSocket() noexcept = default;
  explicit TcpSocket(std::uintptr_t handle) noexcept;
  ~TcpSocket();

  TcpSocket(TcpSocket&& other) noexcept;
  TcpSocket& operator=(TcpSocket&& other) noexcept;
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  bool valid() const noexcept;
  void close() noexcept;
  void shutdown_both() noexcept;
  void set_nodelay(bool enabled) noexcept;

  /// Sends the whole buffer or fails.
  Status send_all(const std::uint8_t* data, std::size_t size);
  /// Receives up to size bytes. A returned value of zero means orderly peer
  /// shutdown.
  Result<std::size_t> recv_some(std::uint8_t* data, std::size_t size);
  /// Receives exactly size bytes or fails.
  Status recv_exact(std::uint8_t* data, std::size_t size);

  std::uintptr_t native_handle() const noexcept;

 private:
  std::uintptr_t handle_{static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0))};
};

/// Blocking TCP listener.
class TcpListener {
 public:
  TcpListener() noexcept = default;
  ~TcpListener();

  TcpListener(TcpListener&& other) noexcept;
  TcpListener& operator=(TcpListener&& other) noexcept;
  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  static Result<TcpListener> bind(const Endpoint& endpoint, int backlog = 32);

  Result<TcpSocket> accept();
  void close() noexcept;
  bool valid() const noexcept;
  /// The actually bound endpoint, with the real port when port 0 was requested.
  const Endpoint& endpoint() const noexcept { return endpoint_; }

 private:
  std::uintptr_t handle_{static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0))};
  Endpoint endpoint_{};
};

/// Establishes an outbound connection.
Result<TcpSocket> connect_to(const Endpoint& endpoint);

}  // namespace apf
