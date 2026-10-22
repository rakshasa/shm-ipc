#include "config.h"

#include "event.h"

#include <cassert>

#include "torrent/exceptions.h"
// #include "torrent/net/fd.h"
// #include "torrent/net/socket_address.h"

namespace torrent::system {

const char*
errno_enum(int status) {
  // The Open Group Base Specifications Issue 6
  switch (status) {
  case 0:               return "0";
  case E2BIG:           return "E2BIG";
  case EACCES:          return "EACCES";
  case EADDRINUSE:      return "EADDRINUSE";
  case EADDRNOTAVAIL:   return "EADDRNOTAVAIL";
  case EAFNOSUPPORT:    return "EAFNOSUPPORT";
  case EAGAIN:          return "EAGAIN";
  case EALREADY:        return "EALREADY";
  case EBADF:           return "EBADF";
  case EBADMSG:         return "EBADMSG";
  case EBUSY:           return "EBUSY";
  case ECANCELED:       return "ECANCELED";
  case ECHILD:          return "ECHILD";
  case ECONNABORTED:    return "ECONNABORTED";
  case ECONNREFUSED:    return "ECONNREFUSED";
  case ECONNRESET:      return "ECONNRESET";
  case EDEADLK:         return "EDEADLK";
  case EDESTADDRREQ:    return "EDESTADDRREQ";
  case EDOM:            return "EDOM";
  case EDQUOT:          return "EDQUOT";
  case EEXIST:          return "EEXIST";
  case EFAULT:          return "EFAULT";
  case EFBIG:           return "EFBIG";
  case EHOSTUNREACH:    return "EHOSTUNREACH";
  case EIDRM:           return "EIDRM";
  case EILSEQ:          return "EILSEQ";
  case EINPROGRESS:     return "EINPROGRESS";
  case EINTR:           return "EINTR";
  case EINVAL:          return "EINVAL";
  case EIO:             return "EIO";
  case EISCONN:         return "EISCONN";
  case EISDIR:          return "EISDIR";
  case ELOOP:           return "ELOOP";
  case EMFILE:          return "EMFILE";
  case EMLINK:          return "EMLINK";
  case EMSGSIZE:        return "EMSGSIZE";
#if defined(EMULTIHOP)
  case EMULTIHOP:       return "EMULTIHOP";
#endif
  case ENAMETOOLONG:    return "ENAMETOOLONG";
  case ENETDOWN:        return "ENETDOWN";
  case ENETRESET:       return "ENETRESET";
  case ENETUNREACH:     return "ENETUNREACH";
  case ENFILE:          return "ENFILE";
  case ENOBUFS:         return "ENOBUFS";
  case ENODATA:         return "ENODATA";
  case ENODEV:          return "ENODEV";
  case ENOENT:          return "ENOENT";
  case ENOEXEC:         return "ENOEXEC";
  case ENOLCK:          return "ENOLCK";
  case ENOLINK:         return "ENOLINK";
  case ENOMEM:          return "ENOMEM";
  case ENOMSG:          return "ENOMSG";
  case ENOPROTOOPT:     return "ENOPROTOOPT";
  case ENOSPC:          return "ENOSPC";
  case ENOSR:           return "ENOSR";
  case ENOSTR:          return "ENOSTR";
  case ENOSYS:          return "ENOSYS";
  case ENOTCONN:        return "ENOTCONN";
  case ENOTDIR:         return "ENOTDIR";
  case ENOTEMPTY:       return "ENOTEMPTY";
  case ENOTSOCK:        return "ENOTSOCK";
  case ENOTTY:          return "ENOTTY";
  case ENXIO:           return "ENXIO";
  case EOPNOTSUPP:      return "EOPNOTSUPP";
  case EOVERFLOW:       return "EOVERFLOW";
  case EPERM:           return "EPERM";
  case EPIPE:           return "EPIPE";
  case EPROTO:          return "EPROTO";
  case EPROTONOSUPPORT: return "EPROTONOSUPPORT";
  case EPROTOTYPE:      return "EPROTOTYPE";
  case ERANGE:          return "ERANGE";
  case EROFS:           return "EROFS";
  case ESPIPE:          return "ESPIPE";
  case ESRCH:           return "ESRCH";
  case ESTALE:          return "ESTALE";
  case ETIME:           return "ETIME";
  case ETIMEDOUT:       return "ETIMEDOUT";
  case ETXTBSY:         return "ETXTBSY";
  case EXDEV:           return "EXDEV";

  default:
    // Handle potentially duplicate error numbers here.
    switch (status) {
    case ENOTSUP:       return "ENOTSUP";
    case EWOULDBLOCK:   return "EWOULDBLOCK";
    default:
      static thread_local char buffer[16];
      std::snprintf(buffer, sizeof(buffer), "E%d", status);

      return buffer;
    };
  };
}

Event::~Event() {
  assert(m_poll_event == nullptr && "Event::~Event() called with m_poll_event != nullptr.");
}

// int
// Event::socket_type_or_zero() const {
//   if (!is_open())
//     throw internal_error("Event::socket_type_or_zero() called on closed event.");

//   int socktype{};

//   if (!fd_get_type(m_fileDesc, &socktype)) {
//     if (errno != EBADF)
//       throw internal_error("Event::socket_type_or_zero() error getting socket type");

//     return 0;
//   }

//   return socktype;
// }

const char*
Event::type_name() const {
  throw internal_error("Event::type_name() must be overridden in derived class.");
}

std::string
Event::print_name_fd_str() const {
 return "name:" + std::string(type_name()) + " fd:" + std::to_string(file_descriptor());
}

// // TODO: Add "check_socket_valid_and_same".

// bool
// Event::update_socket_address() {
//   if (!is_open())
//     throw internal_error("Event::update_socket_address() called on closed event.");

//   int socktype = socket_type_or_zero();

//   if (socktype == 0 || (socktype != SOCK_STREAM && socktype != SOCK_DGRAM))
//     return false;

//   auto address = fd_get_socket_name(m_fileDesc);

//   if (address == nullptr) {
//     if (errno != EBADF && errno != ENOTCONN && errno != EINVAL)
//       throw internal_error("Event::update_socket_address() error getting socket address");

//     return false;
//   }

//   m_socket_address = std::move(address);
//   return true;
// }

// bool
// Event::update_peer_address() {
//   if (!is_open())
//     throw internal_error("Event::update_peer_address() called on closed event.");

//   int socktype = socket_type_or_zero();

//   if (socktype == 0 || (socktype != SOCK_STREAM && socktype != SOCK_DGRAM))
//     return false;

//   auto address = fd_get_peer_name(m_fileDesc);

//   if (address == nullptr) {
//     if (errno != EBADF && errno != ENOTCONN && errno != EINVAL)
//       throw internal_error("Event::update_peer_address() error getting peer address");

//     return false;
//   }

//   m_peer_address = std::move(address);
//   return true;
// }

// bool
// Event::update_and_verify_socket_address() {
//   auto old_address = std::move(m_socket_address);

//   if (!update_socket_address()) {
//     m_socket_address = std::move(old_address);

//     return m_socket_address == nullptr;
//   }

//   if (old_address == nullptr)
//     return true;

//   if (!sap_equal(old_address, m_socket_address)) {
//     m_socket_address = std::move(old_address);
//     return false;
//   }

//   return true;
// }

// bool
// Event::update_and_verify_peer_address() {
//   auto old_address = std::move(m_peer_address);

//   if (!update_peer_address()) {
//     m_peer_address = std::move(old_address);

//     return m_peer_address == nullptr;
//   }

//   if (old_address == nullptr)
//     return true;

//   if (!sap_equal(old_address, m_peer_address)) {
//     m_peer_address = std::move(old_address);
//     return false;
//   }

//   return true;
// }

} // namespace torrent
