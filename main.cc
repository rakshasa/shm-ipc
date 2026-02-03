#include <iostream>
#include <unistd.h>

#include "torrent/shm/segment.h"
#include "torrent/shm/channel.h"
#include "torrent/shm/router.h"

struct TestHandler {
  void on_read(void* data, uint32_t size) {
    if (size == 0) {
      std::cout << "TestHandler received close message: id:" << id << std::endl;
      return;
    }

    std::cout << "TestHandler received message: id:" << id << " size:" << size << " : " << std::string(static_cast<char*>(data), size) << std::endl;
  }

  void on_error(void* data, uint32_t size) {
    std::cout << "TestHandler received error:   id:" << id << " size:" << size << " : " << std::string(static_cast<char*>(data), size) << std::endl;
  }

  uint32_t id;
};

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

  TestHandler create_new_channel(torrent::shm::Router* router);

  uint32_t id;
};

TestHandler
ParentHandler::create_new_channel(torrent::shm::Router* router) {
  TestHandler handler;

  handler.id = router->register_handler(
    [&](void* data, uint32_t size) { handler.on_read(data, size); },
    [&](void* data, uint32_t size) { handler.on_error(data, size); }
  );

  std::cout << "ParentHandler created new channel with id: " << handler.id << std::endl;

  // Send a message to ChildHandler to tell it the id of this new channel.

  struct NewChannelMessage {
    uint32_t id;
  } msg;

  msg.id = handler.id;

  if (!router->write(id, sizeof(msg), &msg))
    throw std::runtime_error("ParentHandler failed to send new channel message");

  return handler;
}




void
child_process(torrent::shm::Segment* read_segment, torrent::shm::Segment* write_segment) {
  std::cout << "Child process started, reading messages..." << std::endl;

  auto read_channel  = static_cast<torrent::shm::Channel*>(read_segment->address());
  auto write_channel = static_cast<torrent::shm::Channel*>(write_segment->address());
  auto router        = std::make_unique<torrent::shm::Router>(read_channel, write_channel);

  TestHandler handler_1;
  TestHandler handler_2;

  handler_1.id = router->register_handler(
    [&](void* data, uint32_t size) { handler_1.on_read(data, size); },
    [&](void* data, uint32_t size) { handler_1.on_error(data, size); }
  );
  handler_2.id = router->register_handler(
    [&](void* data, uint32_t size) { handler_2.on_read(data, size); },
    [&](void* data, uint32_t size) { handler_2.on_error(data, size); }
  );

  for (int i = 0; ; ++i) {
    std::cout << "Child process checking for message..." << std::endl;

    router->process_reads();

    if (i % 5 == 0) {
      const char* message = "Hello from child process!";
      std::cout << "Child process writing message..." << std::endl;

      uint32_t id = (i % 2 == 0) ? handler_1.id : handler_2.id;

      while (!write_channel->write(id, strlen(message) + 1, (void*)message)) {
        std::cout << "Child process: channel full, waiting..." << std::endl;
        usleep(1000); // 100 ms
      }
    }

    sleep(1);
  }
}

void
parent_process(torrent::shm::Segment* read_segment, torrent::shm::Segment* write_segment) {
  std::cout << "Parent process started, writing messages..." << std::endl;

  auto read_channel  = static_cast<torrent::shm::Channel*>(read_segment->address());
  auto write_channel = static_cast<torrent::shm::Channel*>(write_segment->address());
  auto router        = std::make_unique<torrent::shm::Router>(read_channel, write_channel);

  // TODO: Use special channel to tell other side to initialize something, and create a new handler.

  TestHandler handler_1;
  TestHandler handler_2;

  handler_1.id = router->register_handler(
    [&](void* data, uint32_t size) { handler_1.on_read(data, size); },
    [&](void* data, uint32_t size) { handler_1.on_error(data, size); }
  );
  handler_2.id = router->register_handler(
    [&](void* data, uint32_t size) { handler_2.on_read(data, size); },
    [&](void* data, uint32_t size) { handler_2.on_error(data, size); }
  );

  for (int i = 0; ; ++i) {
    std::cout << "Parent process checking for message..." << std::endl;
    router->process_reads();

    std::cout << "Parent process writing message..." << std::endl;
    const char* message = "Hello from parent process!";

    uint32_t id = (i % 2 == 0) ? handler_1.id : handler_2.id;

    while (!write_channel->write(id, strlen(message) + 1, (void*)message)) {
      std::cout << "Parent process: channel full, waiting..." << std::endl;
      usleep(1000); // 100 ms
    }

    sleep(1);
  }
}

int
main() {
  auto segment_1 = std::make_unique<torrent::shm::Segment>();
  auto segment_2 = std::make_unique<torrent::shm::Segment>();

  // segment->create(12 * torrent::shm::Segment::page_size);
  segment_1->create(1 * torrent::shm::Segment::page_size);
  segment_2->create(1 * torrent::shm::Segment::page_size);

  segment_1->attach();
  segment_2->attach();

  static_cast<torrent::shm::Channel*>(segment_1->address())->initialize(segment_1->address(), segment_1->size());
  static_cast<torrent::shm::Channel*>(segment_2->address())->initialize(segment_2->address(), segment_2->size());

  // SEGFAULT when detaching before fork()
  // segment_1->detach();
  // segment_2->detach();

  pid_t pid = ::fork();

  if (pid == -1)
    throw std::runtime_error("fork() failed: " + std::string(strerror(errno)));

  if (pid == 0) {
    child_process(segment_2.get(), segment_1.get());
  } else {
    parent_process(segment_1.get(), segment_2.get());
  }

  ::wait(nullptr);

  return 0;
}
