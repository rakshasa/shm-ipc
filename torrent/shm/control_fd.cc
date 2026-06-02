#include "config.h"

#include "torrent/shm/control_fd.h"

#include <iostream>
#include <unistd.h>
#include <sys/socket.h>

#include "torrent/exceptions.h"

namespace torrent::shm {

void
ControlFd::open(int fd) {
  set_file_descriptor(fd);
}

void
ControlFd::close() {
  if (!is_open())
    return;

  if (::close(file_descriptor()) == -1)
    throw internal_error("ControlFd::close() error closing control fd: " + std::string(std::strerror(errno)));

  set_file_descriptor(-1);
}

void
ControlFd::event_read() {
  char buffer[2048];
  ssize_t result = ::recv(file_descriptor(), buffer, sizeof(buffer), MSG_DONTWAIT);

  if (result == 0)
    return m_slot_closed(0);

  if (result > 0)
    return m_slot_message(std::string(buffer, result));

  if (errno != EAGAIN && errno != EWOULDBLOCK)
    return m_slot_closed(errno);
}

void
ControlFd::event_write() {
}

void
ControlFd::event_error() {
  throw internal_error("ControlFd::event_error() error on control fd: " + std::string(std::strerror(errno)));
}

} // namespace torrent::shm
