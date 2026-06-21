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

// This should only need to be set when compiling libtorrent.
#ifdef SUPPORT_ATTRIBUTE_VISIBILITY
  #define LIBTORRENT_NO_EXPORT __attribute__ ((visibility("hidden")))
  #define LIBTORRENT_EXPORT __attribute__ ((visibility("default")))
#else
  #define LIBTORRENT_NO_EXPORT
  #define LIBTORRENT_EXPORT
#endif

#define align_cacheline alignas(LT_SMP_CACHE_BYTES)

struct sockaddr;
struct sockaddr_in;
struct sockaddr_in6;
struct sockaddr_un;

using namespace std::chrono_literals;

namespace torrent {

class Event;
class HashString;

namespace runtime {

class SocketManager;

} // namespace runtime

namespace system {

using callback_id = std::shared_ptr<std::atomic<uint32_t>>;

class Poll;

} // namespace system

} // namespace torrent


extern std::unique_ptr<torrent::system::Poll> g_poll;

namespace torrent::this_thread {

inline auto* poll() { return g_poll.get(); }

} // namespace torrent::this_thread

#endif
