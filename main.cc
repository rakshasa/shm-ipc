#include <execinfo.h>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "torrent/shm/segment.h"
#include "torrent/shm/channel.h"
#include "torrent/shm/factory.h"
#include "torrent/shm/router.h"

// handle segfault and other signals by closing fd
int                   g_fd_to_close_on_signal = -1;
torrent::shm::Router* g_router = nullptr;

void
do_panic(int signum) {
  if (g_router) {
    std::string msg = "Process terminated due to signal: " + std::to_string(signum);

    void* stackPtrs[20];

    // Print the stack and exit.
    int    stackSize    = backtrace(stackPtrs, 20);
    char** stackStrings = backtrace_symbols(stackPtrs, stackSize);

    for (int i = 0; i < stackSize; ++i)
      msg += "\n" + std::string(stackStrings[i]);

    g_router->send_fatal_error(msg.c_str(), msg.size());
    g_fd_to_close_on_signal = -1;
  }

  // If we don't close the socketpair fd, then the other process won't get EOF. (on macos, not
  // tested on other archs)
  if (g_fd_to_close_on_signal != -1) {
    close(g_fd_to_close_on_signal);
    g_fd_to_close_on_signal = -1;
  }

  std::cerr << "Process terminating due to signal: " << signum << std::endl;

  signal(signum, SIG_DFL);
  signal(SIGABRT, SIG_DFL);

  abort();
}

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

bool
check_socket_closed(int fd) {
  errno = 0;

  char buffer[2048];
  ssize_t result = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);

  if (result == 0) {
    std::cout << "check_socket_closed(): socket closed (recv() returned 0)" << std::endl;
    return true;
  }

  if (result > 0) {
    std::cout << "check_socket_closed(): received error data"
              << std::endl << std::endl
              << std::string(buffer, result)
              << std::endl << std::endl;

    // throw std::runtime_error("check_socket_closed(): recv() returned unexpected data");
    return true;
  }

  if (errno != EAGAIN && errno != EWOULDBLOCK) {
    std::cerr << "check_socket_closed(): recv() failed with error: " << std::strerror(errno) << std::endl;
    return true;
  }

  return false;
}

void
child_process(torrent::shm::Router* router) {
  std::cout << "Child process started, reading messages..." << std::endl;

  g_router = router;

  auto child_handler = new ChildHandler{};
  child_handler->id = 1;
  child_handler->router = router;

  router->register_handler(1,
                           [child_handler](void* data, uint32_t size) { child_handler->on_read(data, size); },
                           [child_handler](void* data, uint32_t size) { child_handler->on_error(data, size); });

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

      if (child_handler->channels.empty()) {
        std::cout << "Child process: no channels to write to, waiting..." << std::endl;
        sleep(1);
        continue;
      }

      uint32_t id = child_handler->channels[i % child_handler->channels.size()]->id;

      while (!router->write(id, strlen(message) + 1, (void*)message)) {
        std::cout << "Child process: channel full, waiting..." << std::endl;
        usleep(1000); // 100 ms
      }
    }

    sleep(1);
  }
}

void
parent_process(torrent::shm::Router* router) {
  std::cout << "Parent process started, writing messages..." << std::endl;

  auto parent_handler = new ParentHandler{};
  parent_handler->id = 1;

  router->register_handler(1,
                           [parent_handler](void* data, uint32_t size) { parent_handler->on_read(data, size); },
                           [parent_handler](void* data, uint32_t size) { parent_handler->on_error(data, size); });

  auto handler_1 = parent_handler->create_new_channel(router);
  auto handler_2 = parent_handler->create_new_channel(router);

  for (int i = 0; ; ++i) {
    if (check_socket_closed(router->file_descriptor())) {
      std::cout << "Parent process: socket closed, exiting..." << std::endl;
      break;
    }

    std::cout << "Parent process checking for message..." << std::endl;
    router->process_reads();

    std::cout << "Parent process writing message..." << std::endl;
    const char* message = "Hello from parent process!";

    uint32_t id = (i % 2 == 0) ? handler_1->id : handler_2->id;

    while (!router->write(id, strlen(message) + 1, (void*)message)) {
      std::cout << "Parent process: channel full, waiting..." << std::endl;
      usleep(1000); // 100 ms
    }

    sleep(1);
  }
}

int
main() {
  // add signal handlers:
  signal(SIGSEGV, do_panic);
  signal(SIGABRT, do_panic);
  signal(SIGFPE, do_panic);
  signal(SIGILL, do_panic);
  signal(SIGTERM, do_panic);
  signal(SIGINT, do_panic);
  signal(SIGPIPE, SIG_IGN); // ignore SIGPIPE

  torrent::shm::RouterFactory factory;

  factory.initialize(1 * torrent::shm::Segment::page_size);

  pid_t pid = fork();

  if (pid == -1)
    throw std::runtime_error("fork() failed: " + std::string(strerror(errno)));

  if (pid == 0) {
    // sleep(15);

    auto router = factory.create_child_router();

    g_fd_to_close_on_signal = router->file_descriptor();

    try {
      child_process(router.get());
    } catch (const std::exception& e) {
      std::cerr << "Child process exception: " << e.what() << std::endl;
    }

    std::cout << "Child process exiting..." << std::endl;
    return 0;

  } else {
    auto router = factory.create_parent_router();

    g_fd_to_close_on_signal = router->file_descriptor();

    try {
      parent_process(router.get());
    } catch (const std::exception& e) {
      std::cerr << "Parent process exception: " << e.what() << std::endl;
    }

    std::cout << "Parent process exiting..." << std::endl;

    ::wait(nullptr);
    return 0;
  }
}
