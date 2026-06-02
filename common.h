#ifndef SHMIPC_COMMON_H
#define SHMIPC_COMMON_H

#include <atomic>
#include <cstdint>

namespace torrent::shm {
  class Router;
}

extern std::atomic<bool> g_should_shutdown;

void parent_process(torrent::shm::Router* router);
void child_process(torrent::shm::Router* router);

bool check_socket_closed(int fd);
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
