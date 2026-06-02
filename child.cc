#include <iostream>

#include "common.h"

#include "torrent/shm/segment.h"
#include "torrent/shm/channel.h"
#include "torrent/shm/router.h"


struct ChildHandler {
  void on_read(void* data, uint32_t size);

  void on_error(void* data, uint32_t size) {
    std::cout << "ChildHandler received error:   id:" << id << " size:" << size << " : " << std::string(static_cast<char*>(data), size) << std::endl;
    throw std::runtime_error("ChildHandler throwing error as test");
  }

  torrent::shm::Router*     router;
  uint32_t                  id;
  std::vector<TestHandler*> channels;
};

void
ChildHandler::on_read(void* data, uint32_t size) {
  if (size == 0) {
    std::cout << "ChildHandler received close message: id:" << id << std::endl;
    return;
  }

  // Add a new channel id:
  if (size != sizeof(NewChannelMessage))
    throw std::runtime_error("ChildHandler received message with invalid size for new channel message");

  auto* msg = static_cast<NewChannelMessage*>(data);
  std::cout << "ChildHandler received new channel message with id: " << msg->id << std::endl;

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

  std::cout << "Child process started: fd." << std::endl;

  router->register_control_closed_handler([](int error_code) { handle_control_closed("Child", error_code); });
  router->register_control_message_handler([](auto msg) { handle_control_message("Child", msg); });

  auto child_handler = new ChildHandler{};
  child_handler->id = 1;
  child_handler->router = router;

  router->register_handler(1,
                           [child_handler](void* data, uint32_t size) { child_handler->on_read(data, size); },
                           [child_handler](void* data, uint32_t size) { child_handler->on_error(data, size); });

  auto last_write = std::chrono::steady_clock::now();

  for (int i = 0; ; ++i) {
    // if (check_socket_closed(router->file_descriptor())) {
    //   std::cout << "Child process: socket closed, exiting..." << std::endl;
    //   break;
    // }

    if (g_should_shutdown) {
      std::cout << "Child process: shutdown signal received, exiting..." << std::endl;

      router->send_fatal_error("Child process shutting down due to interrupt signal");
      break;
    }

    std::cout << "Child process checking for message..." << std::endl;
    router->process_reads();

    if (std::chrono::steady_clock::now() - last_write > 5s) {
      last_write = std::chrono::steady_clock::now();

      const char* message = "Hello from CHILD process!";
      std::cout << "Child process writing message..." << std::endl;

      if (child_handler->channels.empty()) {
        std::cout << "Child process: no channels to write to, waiting..." << std::endl;
        continue;
      }

      uint32_t id = child_handler->channels[i % child_handler->channels.size()]->id;

      while (!router->write(id, strlen(message) + 1, (void*)message)) {
        std::cout << "Child process: channel full, waiting..." << std::endl;
        std::this_thread::sleep_for(100ms);
      }
    }

    // Wait for event.
    std::this_thread::sleep_for(1s);
  }
}
