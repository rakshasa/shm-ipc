#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "torrent/shm/segment.h"
#include "torrent/shm/channel.h"
#include "torrent/shm/router.h"

struct NewChannelMessage {
  uint32_t id;
};

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

  NewChannelMessage msg;
  msg.id = handler.id;

  if (!router->write(id, sizeof(msg), &msg))
    throw std::runtime_error("ParentHandler failed to send new channel message");

  return handler;
}

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

  router->register_handler(
    [&](void* data, uint32_t size) { handler->on_read(data, size); },
    [&](void* data, uint32_t size) { handler->on_error(data, size); }
  );
}

bool
check_socket_closed(int fd) {
  char buffer[1];
  ssize_t result = recv(fd, buffer, sizeof(buffer), MSG_PEEK | MSG_DONTWAIT);

  if (result == 0)
    return true;

  if (result > 0)
    throw std::runtime_error("check_socket_closed(): recv() returned unexpected data");

  if (errno == EAGAIN || errno == EWOULDBLOCK)
    return false;

  return false;
}

void
child_process(int fd, torrent::shm::Segment* read_segment, torrent::shm::Segment* write_segment) {
  std::cout << "Child process started, reading messages..." << std::endl;

  auto read_channel  = static_cast<torrent::shm::Channel*>(read_segment->address());
  auto write_channel = static_cast<torrent::shm::Channel*>(write_segment->address());
  auto router        = std::make_unique<torrent::shm::Router>(fd, read_channel, write_channel);

  ChildHandler child_handler;

  for (int i = 0; ; ++i) {
    if (check_socket_closed(router->file_descriptor())) {
      std::cout << "Child process: socket closed, exiting..." << std::endl;
      break;
    }

    std::cout << "Child process checking for message..." << std::endl;
    router->process_reads();

    if (i % 5 == 0) {
      const char* message = "Hello from child process!";
      std::cout << "Child process writing message..." << std::endl;

      if (child_handler.channels.empty()) {
        std::cout << "Child process: no channels to write to, waiting..." << std::endl;
        sleep(1);
        continue;
      }

      uint32_t id = child_handler.channels[i % child_handler.channels.size()]->id;

      while (!write_channel->write(id, strlen(message) + 1, (void*)message)) {
        std::cout << "Child process: channel full, waiting..." << std::endl;
        usleep(1000); // 100 ms
      }
    }

    sleep(1);
  }
}

void
parent_process(int fd, torrent::shm::Segment* read_segment, torrent::shm::Segment* write_segment) {
  std::cout << "Parent process started, writing messages..." << std::endl;

  auto read_channel  = static_cast<torrent::shm::Channel*>(read_segment->address());
  auto write_channel = static_cast<torrent::shm::Channel*>(write_segment->address());
  auto router        = std::make_unique<torrent::shm::Router>(fd, read_channel, write_channel);

  ParentHandler parent_handler;
  parent_handler.id = 1;

  router->register_handler(1,
    [&](void* data, uint32_t size) { parent_handler.on_read(data, size); },
    [&](void* data, uint32_t size) { parent_handler.on_error(data, size); }
  );

  TestHandler handler_1 = parent_handler.create_new_channel(router.get());
  TestHandler handler_2 = parent_handler.create_new_channel(router.get());

  for (int i = 0; ; ++i) {
    if (check_socket_closed(router->file_descriptor())) {
      std::cout << "Parent process: socket closed, exiting..." << std::endl;
      break;
    }

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

// TODO: Detect when the other process has exited and exit cleanly
//
// Can we use socketpair where one side closes?
//
// Anser: No, socketpair doesn't work with fork() because the file descriptors are shared and closing them in one process would affect the other process. We need to use shared memory and synchronization primitives to detect when the other process has exited.
//
// The above seems wrong, if we use socketpair and fork, the child process will inherit the file descriptors and can use them to communicate with the parent process. When one process closes its end of the socket, the other process can detect this and exit cleanly.

int
main() {
  auto segment_1 = std::make_unique<torrent::shm::Segment>();
  auto segment_2 = std::make_unique<torrent::shm::Segment>();

  // segment->create(12 * torrent::shm::Segment::page_size);
  segment_1->create(1 * torrent::shm::Segment::page_size);
  segment_2->create(1 * torrent::shm::Segment::page_size);

  // segment_1->attach();
  // segment_2->attach();

  static_cast<torrent::shm::Channel*>(segment_1->address())->initialize(segment_1->address(), segment_1->size());
  static_cast<torrent::shm::Channel*>(segment_2->address())->initialize(segment_2->address(), segment_2->size());

  // SEGFAULT when detaching before fork()
  // TODO: When forking multiple processes, pass a list of segments to detach that aren't relevant.
  // segment_1->detach();
  // segment_2->detach();

  // Create socketpair for communication between parent and child process. Set nonblock
  int socket_pair[2];

  if (socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_pair) == -1)
    throw std::runtime_error("socketpair() failed: " + std::string(strerror(errno)));

  if (fcntl(socket_pair[0], F_SETFL, O_NONBLOCK) == -1)
    throw std::runtime_error("fcntl() failed: " + std::string(strerror(errno)));
  if (fcntl(socket_pair[1], F_SETFL, O_NONBLOCK) == -1)
    throw std::runtime_error("fcntl() failed: " + std::string(strerror(errno)));

  pid_t pid = ::fork();

  if (pid == -1)
    throw std::runtime_error("fork() failed: " + std::string(strerror(errno)));

  if (pid == 0) {
    close(socket_pair[0]);
    child_process(socket_pair[1], segment_2.get(), segment_1.get());
  } else {
    close(socket_pair[1]);
    parent_process(socket_pair[0], segment_1.get(), segment_2.get());
  }

  ::wait(nullptr);

  return 0;
}
