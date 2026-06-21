#ifndef SHMIPC_SHM_COMMON_H
#define SHMIPC_SHM_COMMON_H

#include <atomic>
#include <cstdint>
#include <string>

namespace torrent::shm {
class Router;
}

namespace torrent::system {
class Poll;
}

extern std::atomic<bool> g_should_shutdown;
extern std::atomic<bool> g_control_fd_closed;

void parent_process(torrent::shm::Router* router);
void child_process(torrent::shm::Router* router);

void handle_control_closed(const char* name, int error_code);
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
