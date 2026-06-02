#ifndef LIBTORRENT_TORRENT_SHM_CONTROL_FD_H
#define LIBTORRENT_TORRENT_SHM_CONTROL_FD_H

#include <atomic>
#include <torrent/event.h>

namespace torrent::shm {

class LIBTORRENT_EXPORT ControlFd : public Event {
public:
  ControlFd() = default;
  ~ControlFd() = default;

  const char*         type_name() const override { return "ipc-channel"; }

  void                open(int fd);
  void                close();

  void                register_closed_handler(std::function<void(int)>&& fn);
  void                register_message_handler(std::function<void(std::string)>&& fn);

  void                send_fatal_error(const char* msg, uint32_t size);

  void                event_read() override;
  void                event_write() override;
  void                event_error() override;

private:
  std::function<void(int)>         m_slot_closed;
  std::function<void(std::string)> m_slot_message;
};

} // namespace torrent::shm

#endif
