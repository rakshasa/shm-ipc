#include "config.h"

#include "torrent/system/ipc/router.h"

#include <cassert>
#include <unistd.h>
#include <sys/socket.h>

#include <fcntl.h>

#include "torrent/exceptions.h"
#include "torrent/system/ipc/channel.h"
#include "torrent/system/ipc/control_fd.h"
#include "torrent/system/ipc/segment.h"
#include "torrent/system/ipc/wakeup_fd.h"
#include "torrent/system/poll.h"

namespace torrent::system::ipc {

// TODO: Add entry_fd and kqueue support.
// TODO: Add watchdog.

Router::Router(create_args args)
  : m_read_segment(std::move(args.read_segment)),
    m_write_segment(std::move(args.write_segment)) {

  m_control_fd = std::make_unique<ControlFd>();
  m_control_fd->open(args.control_fd);

  m_wakeup_fd  = std::make_unique<WakeupFd>();
  m_wakeup_fd->open(args.wakeup_fds, args.is_parent);

  m_keepalive_fd = args.keepalive_fd;

  m_read_channel  = static_cast<Channel*>(m_read_segment->address());
  m_write_channel = static_cast<Channel*>(m_write_segment->address());
}

Router::~Router() = default;

// TODO: Rename...
void
Router::open_fds() {
  this_thread::poll()->open_and_insert_read(m_control_fd.get());

  // runtime::socket_manager()->register_event_or_throw(this, runtime::category_internal, [this]() {
  this_thread::poll()->open_and_insert_read(m_wakeup_fd.get());
    // });
}

void
Router::close_fds() {
  m_control_fd->close();
  m_wakeup_fd->close();
}

PublicControlFd
Router::control_fd() {
  return PublicControlFd(m_control_fd.get());
}

uint32_t
Router::register_handler(data_func on_read, data_func on_error) {
  auto id = m_next_id;

  while (true) {
    if ((m_next_id & flag_mask) != 0)
      id = 1;

    if (try_register_handler(id, on_read, on_error))
      break;

    id++;

    if (id == 0)
      throw torrent::internal_error("Router::register_handler(): no available ids");
  }

  m_next_id = id + 1;
  return id;
}

void
Router::register_handler(uint32_t id, data_func on_read, data_func on_error) {
  if (!try_register_handler(id, on_read, on_error))
    throw torrent::internal_error("Router::register_handler(): id already in use");
}

bool
Router::try_register_handler(uint32_t id, data_func on_read, data_func on_error) {
  if ((id & flag_mask) != 0)
    throw torrent::internal_error("Router::try_register_handler(): id has reserved bits set");

  // TODO: Optimize to avoid double lookup.
  auto itr = m_handlers.find(id);

  if (itr != m_handlers.end())
    return false;

  if (!on_read)
    throw torrent::internal_error("Router::try_register_handler(): on_read handler is required");
  if (!on_error)
    throw torrent::internal_error("Router::try_register_handler(): on_error handler is required");

  m_handlers[id] = RouterHandler{on_read, on_error};
  return true;
}

void
Router::close(uint32_t id) {
  auto itr = m_handlers.find(id);

  if (itr == m_handlers.end())
    throw torrent::internal_error("Router::close(): id not found");

  if (itr->second.is_closed_write())
    throw torrent::internal_error("Router::close(): handler already closed for write");

  // TODO: This should check we've had the close acknowledged back from the other side.
  // TODO: However if we're doing ping-pong close messages then unregister_handler() isn't really needed.

  if (itr->second.is_closed_read()) {
    m_handlers.erase(itr);
    return;
  }

  itr->second.on_error = nullptr;

  static const char* close_msg = "CLOSE";

  if (m_write_channel->write(id | Router::flag_close, 6, (void*)close_msg) != 6) {
    // TODO: Add to a pending close queue to retry later, for now just shut down everything.
    throw torrent::internal_error("Router::close(): failed to write close event to channel");
  }
}

int
Router::write(uint32_t id, uint32_t size, void* data) {
  assert(m_handlers.find(id) != m_handlers.end());

  int result = m_write_channel->write(id, size, data);

  if (result < 0)
    return result;

  // This doesn't check if we're already interrupted.

  if (m_write_channel->should_wake_consumer())
    m_wakeup_fd->send_interrupt();

  return result;
}

// The write_fn should return 0 if it has no more data to write, or -1 with errno set to EAGAIN if
// nothing was written.
//
// It must return the number of bytes written if it wrote any data to indicate it should be called
// again.
void
Router::write_async(async_write_func write_fn) {
  int result = write_fn(m_write_channel);

  if (result == -1 && errno != EAGAIN)
    throw torrent::internal_error("Router::write_async(): write_fn failed with error: " + std::string(strerror(errno)));

  if (result == 0)
    return;

  m_async_writes.push_back(std::move(write_fn));

  if (result > 0 && m_write_channel->should_wake_consumer())
    m_wakeup_fd->send_interrupt();
}

void
Router::send_graceful_shutdown() {
  m_control_fd->send_graceful_shutdown();
}

void
Router::send_forceful_shutdown() {
  m_control_fd->send_forceful_shutdown();
}

void
Router::send_fatal_error(const char* msg, uint32_t size) {
  // if (m_fd == -1)
  //   throw torrent::internal_error("Router::send_fatal_error(): no file descriptor to send error message on");

  // // Clear non-block to ensure the error message is sent.
  // // if (::fcntl(m_fd, F_SETFL, 0) == -1)
  // //   throw internal_error("RouterFactory::initialize(): fcntl() failed: " + std::string(strerror(errno)));

  // if (::send(m_fd, msg, size, 0) == -1)
  //   throw torrent::internal_error("Router::send_fatal_error(): failed to send error message");

  // ::close(m_fd);
  // m_fd = -1;

  m_control_fd->send_fatal_error(msg, size);

  torrent::this_thread::poll()->remove_and_close(m_control_fd.get());
  m_control_fd->close();
}

// TODO: This should be a static function that takes a vector of routers to process.

void
Router::process_reads_pre_polling() {
  process_reads();
  process_async_writes();
  m_read_channel->consumer_state().store(Channel::flag_polling, std::memory_order_release);
  m_write_channel->producer_state().store(Channel::flag_polling, std::memory_order_release);
  process_reads();
  process_async_writes();
}

void
Router::process_reads_post_polling() {
  process_reads();
  process_async_writes();
  m_read_channel->consumer_state().store(0, std::memory_order_release);
  m_write_channel->producer_state().store(0, std::memory_order_release);
  process_reads();
  process_async_writes();
}

void
Router::process_reads() {
  // TODO: Limit number of reads per call to avoid starvation of other tasks. (based on length, not messages?)

  // TODO: Need to also wake up writer if blocked...

  while (true) {
    auto header = m_read_channel->read_header();

    if (header == nullptr)
      break;

    // TODO: Add a special handler for id=0?

    auto itr = m_handlers.find(header->id & ~Router::flag_mask);

    if (itr == m_handlers.end()) {
      // This really shouldn't happen.
      throw torrent::internal_error("Router::process_reads(): received data for unknown handler id");

      // m_read_channel->consume_header(header);
      // continue;
    }

    if (header->size != 0 && !itr->second.is_closed_read())
      itr->second.on_read(header->data, header->size);

    // TODO: Error on size == 0 and not close?

    if (header->id & Router::flag_close) {
      if (itr->second.is_closed_read()) {
        m_handlers.erase(itr);

        m_read_channel->consume_header(header);
        continue;
      }

      if (header->size != 0)
        throw torrent::internal_error("Router::process_reads(): close message with non-zero size");

      itr->second.on_read = nullptr;

      m_read_channel->consume_header(header);
      continue;
    }

    m_read_channel->consume_header(header);

    if (m_read_channel->should_wake_producer())
      m_wakeup_fd->send_interrupt();
  }

  // TODO: Replace zero-length close messages with a id=0 special message that is buffered and
  // packed.
  //
  // By adding a (free space) buffer for special messages we avoid writes of closes failing.
  //
  // The process_reads() function can then send these special messages after reading normal
  // messages, and do reads while the buffer is insufficient to send them.
}

// TODO: This should restart from where it hit EAGAIN.

void
Router::process_async_writes() {
  if (m_async_writes.empty())
    return;

  auto head_itr    = m_async_writes.begin();
  auto current_itr = m_async_writes.begin();

  while (true) {
    int result = (*current_itr)(m_write_channel);

    head_itr++;

    if (result != 0)
      current_itr++;

    if (result == -1) {
      if (errno != EAGAIN)
        throw torrent::internal_error("Router::process_async_writes(): write_fn failed with error: " + std::string(strerror(errno)));

      break;
    }

    if (head_itr == m_async_writes.end())
      break;

    if (current_itr != head_itr)
      *current_itr = std::move(*head_itr);
  }

  if (current_itr != head_itr)
    m_async_writes.erase(current_itr, head_itr);

  if (m_write_channel->should_wake_consumer())
    m_wakeup_fd->send_interrupt();
}

} // namespace torrent::system::ipc
