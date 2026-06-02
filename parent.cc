#include "common.h"

#include <iostream>

#include "torrent/exceptions.h"
#include "torrent/system/poll.h"

#include "torrent/shm/segment.h"
#include "torrent/shm/channel.h"
#include "torrent/shm/router.h"


struct ParentHandler {
  void on_read(void* data, uint32_t size) {
    if (size == 0)
      throw std::runtime_error("ParentHandler received close message");

    std::cout << "ParentHandler received message: id:" << id << " size:" << size << " : " << std::string(static_cast<char*>(data), size) << std::endl;

    throw std::runtime_error("ParentHandler throwing error as test");
  }

  void on_error(void* data, uint32_t size) {
    std::cout << "ParentHandler received error:   id:" << id << " size:" << size << " : " << std::string(static_cast<char*>(data), size) << std::endl;
    throw std::runtime_error("ParentHandler throwing error as test");
  }

  TestHandler* create_new_channel(torrent::shm::Router* router);

  uint32_t id;
};

TestHandler*
ParentHandler::create_new_channel(torrent::shm::Router* router) {
  auto handler = new TestHandler{};

  handler->id = router->register_handler([handler](void* data, uint32_t size) { handler->on_read(data, size); },
                                         [handler](void* data, uint32_t size) { handler->on_error(data, size); });

  std::cout << "ParentHandler created new channel with id: " << handler->id << std::endl;

  // Send a message to ChildHandler to tell it the id of this new channel.

  NewChannelMessage msg;
  msg.id = handler->id;

  if (!router->write(id, sizeof(msg), &msg))
    throw std::runtime_error("ParentHandler failed to send new channel message");

  return handler;
}

//
// Parent process:
//

void
parent_process(torrent::shm::Router* router) {
  register_signal_shutdown();

  std::cout << "Parent process started: fd." << std::endl;

  router->register_control_closed_handler([](int error_code) { handle_control_closed("Parent", error_code); });
  router->register_control_message_handler([](auto msg) { handle_control_message("Parent", msg); });

  auto parent_handler = new ParentHandler{};
  parent_handler->id = 1;

  router->register_handler(1,
                           [parent_handler](void* data, uint32_t size) { parent_handler->on_read(data, size); },
                           [parent_handler](void* data, uint32_t size) { parent_handler->on_error(data, size); });

  auto handler_1 = parent_handler->create_new_channel(router);
  auto handler_2 = parent_handler->create_new_channel(router);

  std::chrono::steady_clock::time_point shutdown_timestamp{};

  auto m_poll = torrent::system::Poll::create();

  try {

    m_poll->init_thread();

    for (int i = 0; ; ++i) {
      if (g_should_shutdown) {
        if (shutdown_timestamp == std::chrono::steady_clock::time_point{}) {
          shutdown_timestamp = std::chrono::steady_clock::now();

          std::cout << "Parent process: shutdown signal received, waiting for graceful shutdown..." << std::endl;
          continue;
        }

        if (std::chrono::steady_clock::now() - shutdown_timestamp > 5s) {
          std::cout << "Parent process: graceful shutdown timeout exceeded, exiting..." << std::endl;
          break;
        }

        // If control_fd is closed, we can exit immediately.

      }

      // if (check_socket_closed(router->file_descriptor())) {
      //   std::cout << "Parent process: socket closed, exiting..." << std::endl;
      //   break;
      // }

      std::cout << "Parent process checking for message..." << std::endl;

      router->process_reads();

      std::cout << "Parent process writing message..." << std::endl;

      const char* message = "Hello from PARENT process!";

      uint32_t id = (i % 2 == 0) ? handler_1->id : handler_2->id;

      if (!router->write(id, strlen(message) + 1, (void*)message)) {
        std::cout << "Parent process: channel full, waiting..." << std::endl;
        std::this_thread::sleep_for(100ms);

        i--;
        continue;
      }

      //
      // Thread:
      //

      // process_events();

      // auto timeout = std::max(next_timeout(), std::chrono::microseconds(0));
      auto timeout = 0ms;

      // if (!m_scheduler->empty())
      //   timeout = std::min(timeout, m_scheduler->next_timeout());

      [[maybe_unused]] int event_count = m_poll->do_poll(timeout.count());
    }

  } catch (const torrent::internal_error& e) {
    m_poll->cleanup_thread();
    throw;
  }

  m_poll->cleanup_thread();
}
