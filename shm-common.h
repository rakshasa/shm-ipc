#ifndef SHMIPC_SHM_COMMON_H
#define SHMIPC_SHM_COMMON_H

#include <atomic>
#include <cstdint>
#include <string>

#define align_cacheline alignas(LT_SMP_CACHE_BYTES)

namespace torrent::system::ipc {
class Router;
}

namespace torrent::system {
class Poll;
}

extern std::atomic<bool> g_should_shutdown;
extern std::atomic<bool> g_should_graceful_shutdown;
extern std::atomic<bool> g_should_forced_shutdown;
extern std::atomic<bool> g_control_fd_closed;

void parent_process(torrent::system::ipc::Router* router);
void child_process(torrent::system::ipc::Router* router);

void handle_control_closed(torrent::system::ipc::Router* router, const char* name, int error_code);
void handle_control_shutdown(const char* name, bool graceful);
void handle_control_message(const char* name, std::string msg);

void register_signal_shutdown();

//
// Common definitions for parent and child process.
//

struct NewChannelMessage {
  uint32_t id;
};

struct TestHandler {
  void on_read(void* data, uint32_t size);
  void on_error(void* data, uint32_t size);

  uint32_t id;
};

#endif
