#include <iostream>
#include <unistd.h>

#include "torrent/shm/segment.h"
#include "torrent/shm/channel.h"

void
child_process(torrent::shm::Segment* segment) {
  std::cout << "Child process started, reading messages..." << std::endl;

  auto metadata = static_cast<torrent::shm::Channel*>(segment->address());

  while (true) {
    std::cout << "Child process checking for message..." << std::endl;

    auto header = metadata->read_header();

    if (header == nullptr) {
      std::cout << "Child process: no message available, waiting..." << std::endl;
      sleep(1);
      // sleep shorter
      // usleep(100); // 100 ms
      continue;
    }

    std::cout << "Child process received message (id=" << header->id << "): " << header->data << std::endl;

    metadata->consume_header(header);

    sleep(1);
  }
}

void
parent_process(torrent::shm::Segment* segment) {
  std::cout << "Parent process started, writing messages..." << std::endl;

  auto metadata = static_cast<torrent::shm::Channel*>(segment->address());

  while (true) {
    std::cout << "Parent process writing message..." << std::endl;

    const char* message = "Hello from parent process XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXxx!";

    while (!metadata->write(1, strlen(message) + 1, (void*)message)) {
      std::cout << "Parent process: channel full, waiting..." << std::endl;
      // sleep(2);
      usleep(100000); // 10 s
    }

    // sleep(5);
    // usleep(500); // 500 ms
  }
}

int
main() {
  auto segment = std::make_unique<torrent::shm::Segment>();

  // segment->create(12 * torrent::shm::Segment::page_size);
  segment->create(1 * torrent::shm::Segment::page_size);
  segment->attach();

  auto metadata = static_cast<torrent::shm::Channel*>(segment->address());

  metadata->initialize(segment->address(), segment->size());

  pid_t pid = ::fork();

  if (pid == -1)
    throw std::runtime_error("fork() failed: " + std::string(strerror(errno)));

  if (pid == 0)
    child_process(segment.get());
  else
    parent_process(segment.get());

  ::wait(nullptr);

  return 0;
}
