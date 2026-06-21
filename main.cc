#include <chrono>
#include <execinfo.h>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "shm-common.h"

#include "torrent/shm/segment.h"
#include "torrent/shm/channel.h"
#include "torrent/shm/factory.h"
#include "torrent/shm/router.h"
#include "torrent/system/poll.h"

// handle segfault and other signals by closing fd

std::unique_ptr<torrent::system::Poll> g_poll;

torrent::shm::Router*  g_router{};

std::atomic<bool>      g_should_shutdown{};
std::atomic<bool>      g_control_fd_closed{};

void
do_panic(int signum) {
  signal(signum, SIG_DFL);
  signal(SIGABRT, SIG_DFL);

  if (g_router) {
    std::string msg = "Process terminated due to signal: g_router : " + std::to_string(signum);

    void* stackPtrs[20];

    // Print the stack and exit.
    int    stackSize    = backtrace(stackPtrs, 20);
    char** stackStrings = backtrace_symbols(stackPtrs, stackSize);

    for (int i = 0; i < stackSize; ++i)
      msg += "\n" + std::string(stackStrings[i]);

    g_router->send_fatal_error(msg.c_str(), msg.size());
  }

  std::cerr << "Process terminating due to signal: " << signum << std::endl;

  abort();
}

void
do_signal_shutdown(int) {
  g_should_shutdown = true;

  // TODO: Poke the main process thread.
  // TODO: Check if errno should be saved.
}

void
handle_control_closed(torrent::shm::Router* router, const char* name, int error_code) {
  if (error_code == 0)
    std::cout << name << " process: control fd closed normally." << std::endl;
  else
    std::cout << name << " process: control fd closed with error code: " << error_code << std::endl;

  g_should_shutdown   = true;
  g_control_fd_closed = true;

  router->test_close_control_fd();
}

void
handle_control_message(const char* name, std::string msg) {
  std::cout << name << " process: received control message: "
            << std::endl << std::endl << msg << std::endl << std::endl;
}

void
register_signal_shutdown() {
  signal(SIGTERM, do_signal_shutdown);
  signal(SIGINT,  do_signal_shutdown);
}

void
TestHandler::on_read(void* data, uint32_t size) {
  if (size == 0) {
    std::cout << "TestHandler received close message: id:" << id << std::endl;
    return;
  }

  std::cout << "TestHandler received message: id:" << id << " size:" << size << " : " << std::string(static_cast<char*>(data), size) << std::endl;
}

void
TestHandler::on_error(void* data, uint32_t size) {
  std::cout << "TestHandler received error:   id:" << id << " size:" << size << " : " << std::string(static_cast<char*>(data), size) << std::endl;
}

bool
check_socket_closed(int fd) {
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

  // std::this_thread::sleep_for(20s);

  pid_t pid = fork();

  if (pid == -1)
    throw std::runtime_error("fork() failed: " + std::string(strerror(errno)));

  if (pid == 0) {
    auto router = factory.create_child_router();

    g_router = router.get();

    try {
      child_process(router.get());
    } catch (const std::exception& e) {
      std::cerr << "Child process exception: " << e.what() << std::endl;
    }

    std::cout << "Child process exiting..." << std::endl;
    return 0;

  } else {
    auto router = factory.create_parent_router();

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
