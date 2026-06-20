#include "common.h"

#include <algorithm>
#include <iostream>

#include "torrent/exceptions.h"
#include "torrent/system/poll.h"
#include "torrent/shm/segment.h"
#include "torrent/shm/channel.h"
#include "torrent/shm/router.h"


struct ChildHandler {
  void on_read(void* data, uint32_t size);

  void on_error(void* data, uint32_t size) {
    std::cout << "CHILD:HANDLER: received error:   id:" << id << " size:" << size << " : " << std::string(static_cast<char*>(data), size) << std::endl;
    throw std::runtime_error("CHILD:HANDLER throwing error as test");
  }

  torrent::shm::Router*     router;
  uint32_t                  id;
  std::vector<TestHandler*> channels;
};

void
ChildHandler::on_read(void* data, uint32_t size) {
  if (size == 0) {
    std::cout << "CHILD:HANDLER: received close message: id:" << id << std::endl;
    return;
  }

  // Add a new channel id:
  if (size != sizeof(NewChannelMessage))
    throw std::runtime_error("CHILD:HANDLER: received message with invalid size for new channel message");

  auto* msg = static_cast<NewChannelMessage*>(data);
  std::cout << "CHILD:HANDLER: received new channel message with id: " << msg->id << std::endl;

  auto handler = new TestHandler{};
  handler->id = msg->id;

  channels.push_back(handler);

  router->register_handler(handler->id,
                           [handler](void* data, uint32_t size) { handler->on_read(data, size); },
                           [handler](void* data, uint32_t size) { handler->on_error(data, size); });
}


//
// Child process:
//

void
child_process(torrent::shm::Router* router) {
  register_signal_shutdown();

  std::cout << "CHILD: started: fd." << std::endl;

  router->register_control_closed_handler([](int error_code) { handle_control_closed("CHILD:CONTROL", error_code); });
  router->register_control_message_handler([](auto msg)      { handle_control_message("CHILD:CONTROL", msg); });

  auto child_handler = new ChildHandler{};
  child_handler->id = 1;
  child_handler->router = router;

  router->register_handler(1,
                           [child_handler](void* data, uint32_t size) { child_handler->on_read(data, size); },
                           [child_handler](void* data, uint32_t size) { child_handler->on_error(data, size); });

  auto last_write = std::chrono::steady_clock::now();

  auto m_poll = torrent::system::Poll::create();

  try {

    m_poll->init_thread();

    for (int i = 0; ; ++i) {
      // if (g_should_shutdown) {
      //   if (shutdown_timestamp == std::chrono::steady_clock::time_point{}) {
      //     shutdown_timestamp = std::chrono::steady_clock::now();

      //     std::cout << "CHILD:: shutdown signal received, waiting for graceful shutdown..." << std::endl;
      //     continue;
      //   }

      //   if (std::chrono::steady_clock::now() - shutdown_timestamp > 5s) {
      //     std::cout << "CHILD:: graceful shutdown timeout exceeded, exiting..." << std::endl;
      //     break;
      //   }

      //   // If control_fd is closed, we can exit immediately.

      // }

      std::cout << "CHILD: checking for message..." << std::endl;

      router->process_reads();

      if (std::chrono::steady_clock::now() - last_write > 5s) {
        std::cout << "CHILD: writing message..." << std::endl;

        if (child_handler->channels.empty()) {
          std::cout << "CHILD: no channels to write to, waiting..." << std::endl;
          last_write = std::chrono::steady_clock::now();

          ///////////////////

        } else {

          uint32_t id = child_handler->channels[i % child_handler->channels.size()]->id;

          const char* message = "Hello from CHILD process!";

          // while (!router->write(id, strlen(message) + 1, (void*)message)) {
          if (!router->write(id, strlen(message), (void*)message)) {
            std::cout << "CHILD: channel full, waiting..." << std::endl;
            std::this_thread::sleep_for(100ms);

            i--;

            /////////////
          } else {
            last_write = std::chrono::steady_clock::now();
          }
        }
      }

      //
      // Thread:
      //

      // process_events();

      auto timeout = 5s - (std::chrono::steady_clock::now() - last_write);

      std::cout << "CHILD: calculated timeout: " << std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count() << " ms" << std::endl;

      if (timeout < std::chrono::steady_clock::duration::zero())
        timeout = std::chrono::steady_clock::duration::zero();

      // if (!m_scheduler->empty())
      //   timeout = std::min(timeout, m_scheduler->next_timeout());

      std::cout << "CHILD: polling with timeout: " << std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count() << " ms" << std::endl;

      [[maybe_unused]] int event_count = m_poll->do_poll(std::chrono::duration_cast<std::chrono::microseconds>(timeout).count());
    }

  } catch (const torrent::internal_error& e) {
    m_poll->cleanup_thread();
    throw;
  }

  m_poll->cleanup_thread();
}
