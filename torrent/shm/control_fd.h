#ifndef LIBTORRENT_TORRENT_SHM_CONTROL_FD_H
#define LIBTORRENT_TORRENT_SHM_CONTROL_FD_H

#include <atomic>
#include <torrent/event.h>

namespace torrent::shm {

class LIBTORRENT_EXPORT ControlFd : public Event {
public:
  static constexpr unsigned int max_message_size = 1024;

  ControlFd() = default;
  ~ControlFd() = default;

  const char*         type_name() const override { return "ipc-channel"; }

  void                open(int fd);
  void                close();

  void                register_closed_handler(std::function<void(int)>&& fn);
  void                register_shutdown_handler(std::function<void(bool)>&& fn);
  void                register_message_handler(std::function<void(std::string)>&& fn);

  void                send_shutdown_message(bool graceful);
  void                send_fatal_error(const char* msg, uint32_t size);

private:
  void                send_message_internal(const char* msg, uint32_t size);

  void                event_read() override;
  void                event_write() override;
  void                event_error() override;

  std::function<void(int)>         m_slot_closed;
  std::function<void(bool)>        m_slot_shutdown;
  std::function<void(std::string)> m_slot_message;
};

inline void ControlFd::register_closed_handler(std::function<void(int)>&& fn)          { m_slot_closed = std::move(fn); }
inline void ControlFd::register_shutdown_handler(std::function<void(bool)>&& fn)       { m_slot_shutdown = std::move(fn); }
inline void ControlFd::register_message_handler(std::function<void(std::string)>&& fn) { m_slot_message = std::move(fn); }

} // namespace torrent::shm

#endif
