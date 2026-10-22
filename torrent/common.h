#ifndef TORRENT_COMMON_H
#define TORRENT_COMMON_H

#include <new>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstring>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <torrent/system/common.h>

namespace torrent {

class HashString;

namespace runtime {

class SocketManager;

} // namespace runtime

} // namespace torrent


extern std::unique_ptr<torrent::system::Poll> g_poll;

namespace torrent::this_thread {

const char*               thread_name() LIBTORRENT_EXPORT;
std::string               thread_name_str() LIBTORRENT_EXPORT;

inline auto* poll() { return g_poll.get(); }

} // namespace torrent::this_thread

#endif
