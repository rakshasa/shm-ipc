#include "shm-common.h"

#include <iostream>
#include <signal.h>

#include "torrent/common.h"

#include "torrent/exceptions.h"
#include "torrent/system/poll.h"
#include "torrent/shm/control_fd.h"
#include "torrent/shm/channel.h"
#include "torrent/shm/router.h"
#include "torrent/shm/segment.h"

struct ParentHandler {
  void on_read(void* data, uint32_t size) {
    if (size == 0)
      throw std::runtime_error("PARENT:HANDLER: received close message");

    std::cout << "PARENT:HANDLER: received message: id:" << id << " size:" << size << " : " << std::string(static_cast<char*>(data), size) << std::endl;

    throw std::runtime_error("PARENT:HANDLER: throwing error as test");
  }

  void on_error(void* data, uint32_t size) {
    std::cout << "PARENT:HANDLER: received error:   id:" << id << " size:" << size << " : " << std::string(static_cast<char*>(data), size) << std::endl;
    throw std::runtime_error("PARENT:HANDLER: throwing error as test");
  }

  TestHandler* create_new_channel(torrent::shm::Router* router);

  uint32_t id;
};

TestHandler*
ParentHandler::create_new_channel(torrent::shm::Router* router) {
  auto handler = new TestHandler{};

  handler->id = router->register_handler([handler](void* data, uint32_t size) { handler->on_read(data, size); },
                                         [handler](void* data, uint32_t size) { handler->on_error(data, size); });

  std::cout << "PARENT:HANDLER: created new channel with id: " << handler->id << std::endl;

  // Send a message to ChildHandler to tell it the id of this new channel.

  NewChannelMessage msg;
  msg.id = handler->id;

  if (!router->write(id, sizeof(msg), &msg))
    throw std::runtime_error("PARENT:HANDLER: failed to send new channel message");

  return handler;
}

//
// Parent process:
//

constexpr auto message_interval = 1s;

void
parent_process(torrent::shm::Router* router) {
  g_poll = torrent::system::Poll::create();

  register_signal_shutdown();

  // interrupt_handler should only be called when an atomic flag in shm is set to indicate we're
  // entering/already-in polling. Otherwise we do shm channel reads right after poll / before poll.

  // router->control_fd().register_interrupt_handler([]()                  { });
  router->control_fd().register_interrupt_handler([]()                  { std::cout << "PARENT: received interrupt on control fd." << std::endl; });
  router->control_fd().register_message_handler([](auto msg)            { handle_control_message("PARENT:CONTROL", msg); });
  router->control_fd().register_closed_handler([router](int error_code) { handle_control_closed(router, "PARENT:CONTROL", error_code); });
  router->control_fd().register_shutdown_handler([](bool graceful)      { handle_control_shutdown("PARENT:CONTROL", graceful); });

  std::cout << "PARENT: started: fd." << std::endl;

  auto parent_handler = new ParentHandler{};
  parent_handler->id = 1;

  router->register_handler(1,
                           [parent_handler](void* data, uint32_t size) { parent_handler->on_read(data, size); },
                           [parent_handler](void* data, uint32_t size) { parent_handler->on_error(data, size); });

  auto handler_1 = parent_handler->create_new_channel(router);
  auto handler_2 = parent_handler->create_new_channel(router);

  std::chrono::steady_clock::time_point shutdown_timestamp{};

  [[maybe_unused]] auto start_time = std::chrono::steady_clock::now();
  auto                  last_write = std::chrono::steady_clock::now();

  router->open_control_fd();

  try {

    torrent::this_thread::poll()->init_thread();

    for (int i = 0; ; ++i) {
      if (g_should_shutdown) {
        if (g_control_fd_closed) {

          if (g_should_graceful_shutdown)
            std::cout << "PARENT: control fd closed, shutdown was graceful, exiting." << std::endl;
          else if (g_should_forced_shutdown)
            std::cout << "PARENT: control fd closed, shutdown was forceful, exiting." << std::endl;
          else
            std::cout << "PARENT: control fd closed, shutdown was of unknown type, exiting." << std::endl;

          break;
        }

        // Parent waits for child to close.

        if (shutdown_timestamp.time_since_epoch() == 0s) {
          shutdown_timestamp = std::chrono::steady_clock::now();

          std::cout << "PARENT: shutdown signal received, waiting for graceful shutdown..." << std::endl;

          router->send_graceful_shutdown();
          continue;
        }

        if (std::chrono::steady_clock::now() - shutdown_timestamp > 5s) {
          std::cout << "PARENT: graceful shutdown timeout exceeded, exiting..." << std::endl;
          router->send_forceful_shutdown();
          break;
        }
      }

      // std::cout << "PARENT: checking for message..." << std::endl;

      if (std::chrono::steady_clock::now() - last_write > message_interval) {
        std::cout << "PARENT: writing message..." << std::endl;

        uint32_t id = (i % 2 == 0) ? handler_1->id : handler_2->id;

        const char* message = "Hello from PARENT process!";

        if (!router->write(id, strlen(message), (void*)message)) {
          std::cout << "PARENT: channel full, waiting..." << std::endl;
          std::this_thread::sleep_for(100ms);

          i--;

        } else {
          last_write = std::chrono::steady_clock::now();
        }
      }

      // // Send SIGSEGV to self after 30 seconds to test that the child process handles it properly.
      // if (std::chrono::steady_clock::now() - start_time > 30s) {
      //   std::cout << "PARENT: sending SIGSEGV to self to test child process handling..." << std::endl;
      //   raise(SIGSEGV);
      // }

      //
      // Thread:
      //

      router->process_reads_pre_polling();

      auto timeout = message_interval - (std::chrono::steady_clock::now() - last_write);

      if (timeout < std::chrono::steady_clock::duration::zero())
        timeout = std::chrono::steady_clock::duration::zero();

      std::cout << "PARENT: polling for events with timeout: " << std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count() << "ms" << std::endl;

      [[maybe_unused]] int event_count = torrent::this_thread::poll()->do_poll(std::chrono::duration_cast<std::chrono::microseconds>(timeout).count());

      router->process_reads_post_polling();

      if (event_count > 0) {
        std::cout << "PARENT: poll returned with event count: " << event_count << std::endl;

        torrent::this_thread::poll()->process();
      }
    }

  } catch (...) {
    router->test_close_control_fd();
    torrent::this_thread::poll()->cleanup_thread();

    throw;
  }

  router->test_close_control_fd();
  torrent::this_thread::poll()->cleanup_thread();
}
