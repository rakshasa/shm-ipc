#include "config.h"

#include "torrent/system/ipc/channel.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <new>

#include "torrent/exceptions.h"

namespace torrent::system::ipc {

namespace {

constexpr uint32_t
align_size_to_cacheline(uint32_t size) {
  uint32_t cache_line_size = LT_SMP_CACHE_BYTES;

  return (size + (cache_line_size - 1)) & ~(cache_line_size - 1);
}

inline char* as_c_ptr(Channel::header_type* header) { return reinterpret_cast<char*>(header); }

} // namespace anonymous

inline Channel::header_type*
Channel::header_at_offset(uint32_t offset) {
  return reinterpret_cast<header_type*>(static_cast<char*>(m_addr) + offset);
}

bool
Channel::verify_read_header(header_type* header) {
  auto start_offset = m_read_offset.load(std::memory_order_acquire);

  if (header != header_at_offset(start_offset) && header != header_at_offset(0))
    return false;

  if (header->size > m_size - (as_c_ptr(header) - as_c_ptr(header_at_offset(0))) - header_size)
    return false;

  return true;
}

uint32_t
Channel::write_to_header(uint32_t id, uint32_t offset, uint32_t size, void* data) {
  auto* header = header_at_offset(offset);
  header->id   = id;
  header->size = size;

  if (size > 0)
    std::memcpy(header->data, data, size);

  return align_size_to_cacheline(offset + header_size + size);
}

void
Channel::write_padding_header(uint32_t offset) {
  auto* header = header_at_offset(offset);
  header->id   = 0;
  header->size = ~uint32_t{};
}

void
Channel::initialize(uint32_t size) {
  if (reinterpret_cast<uintptr_t>(this) % LT_SMP_CACHE_BYTES != 0)
    throw torrent::internal_error("Channel::initialize() this must be aligned to cache line size");

  if (reinterpret_cast<uintptr_t>(m_addr) % LT_SMP_CACHE_BYTES != 0)
    throw torrent::internal_error("Channel::initialize() m_addr must be aligned to cache line size");

  if (size == 0 || (size % LT_SMP_CACHE_BYTES) != 0)
    throw torrent::internal_error("Channel::initialize() size must be non-zero and a multiple of cache line size");

  m_size = size - (reinterpret_cast<char*>(m_addr) - reinterpret_cast<char*>(this));

  m_write_threshold = align_size_to_cacheline(m_size / 8);

  m_read_offset    = 0;
  m_write_offset   = 0;
  m_consumer_state = 0;
}

// Returns the number of payload bytes written, which may be less than 'size'.
//
// Or -1 with errno set to EAGAIN if there is no room to write the data.
int
Channel::write(uint32_t id, uint32_t size, void* data) {
  if (id == 0)
    throw torrent::internal_error("Channel::write() invalid id");

  // if (data == nullptr)
  //   throw torrent::internal_error("Channel::write() data is null");

  // if (size == 0)
  //   throw torrent::internal_error("Channel::write() size is zero");

  if (size == 0 && data != nullptr)
    throw torrent::internal_error("Channel::write() size is zero but data is not null");

  uint32_t start_offset = m_read_offset.load(std::memory_order_acquire);
  uint32_t old_offset   = m_write_offset.load(std::memory_order_acquire);
  uint32_t end_offset   = old_offset;

  uint32_t first_size  = 0;
  uint32_t second_size = 0;

  auto size_with_padding = [](uint32_t offset) {
      return offset >= cache_line_size ? offset - cache_line_size : 0;
    };

  auto data_size = [](uint32_t size) {
      return size >= cache_line_size ? size - header_size : 0;
    };

  if (end_offset == m_size) {
    // Resetting end_offset would mark this as an empty buffer.
    if (start_offset == 0) {
      errno = EAGAIN;
      return -1;
    }

    end_offset = 0;

  } else {
    if (start_offset - end_offset == cache_line_size) {
      errno = EAGAIN;
      return -1;
    }
  }

  // Check if we should write to the end of the buffer.
  if (end_offset >= start_offset) {
    first_size  = std::min(size,              data_size(m_size - end_offset));
    second_size = std::min(size - first_size, data_size(size_with_padding(start_offset)));

    // TODO: Reconsider write_threshold, we're going to wake up the reader anyway so just push it through?
    //
    // TODO: Although if the reader needs to do e.g. disk I/O it may be better to wait. So make the
    // write_treshold configurable, and maybe even per-id. (or rather as an argument to write())

    if (first_size + second_size < std::min(size, m_write_threshold)) {
      if (start_offset == end_offset)
        throw torrent::internal_error("Channel::write() start_offset == end_offset but can't write");

      errno = EAGAIN;
      return -1;
    }

    end_offset = write_to_header(id, end_offset, first_size, data);

    if (second_size > 0) {
      if (end_offset != m_size)
        throw torrent::internal_error("Channel::write() end_offset not at end of buffer");

      end_offset = 0;
    }

  } else {
    second_size = std::min(size, data_size(size_with_padding(start_offset - end_offset)));

    if (second_size < std::min(size, m_write_threshold)) {
      errno = EAGAIN;
      return -1;
    }
  }

  // Write the second part of the data at the start of the buffer.
  if (second_size > 0)
    end_offset = write_to_header(id, end_offset, second_size, static_cast<char*>(data) + first_size);

  if (end_offset % cache_line_size != 0)
    throw torrent::internal_error("Channel::write() end_offset not aligned to cache line size");

  if (end_offset > m_size)
    throw torrent::internal_error("Channel::write() end_offset exceeds buffer size");

  if (first_size + second_size == 0)
    throw torrent::internal_error("Channel::write() wrote zero bytes");

  if (first_size + second_size > size)
    throw torrent::internal_error("Channel::write() wrote more bytes than requested");

  if (first_size == 0 && end_offset >= start_offset)
    throw torrent::internal_error("Channel::write() first_size is zero but end_offset >= start_offset");

  std::atomic_thread_fence(std::memory_order_release);
  m_write_offset.store(end_offset, std::memory_order_release);

  return first_size + second_size;
}

Channel::header_type*
Channel::read_header() {
  uint32_t start_offset = m_read_offset.load(std::memory_order_acquire);
  uint32_t end_offset   = m_write_offset.load(std::memory_order_acquire);

  if (start_offset == end_offset)
    return nullptr;

  if (start_offset == m_size) {
    if (end_offset == 0)
      return nullptr;

    start_offset = 0;
  }

  std::atomic_thread_fence(std::memory_order_acquire);

  auto header = header_at_offset(start_offset);

  if (!verify_read_header(header))
    throw torrent::internal_error("Channel::read_header() header size exceeds buffer size : " +
                                  std::to_string(header->size) + " : " + std::to_string(start_offset));

  return header;
}

void
Channel::consume_header(header_type* header) {
  uint32_t header_offset    = as_c_ptr(header) - as_c_ptr(header_at_offset(0));
  uint32_t new_start_offset = header_offset + align_size_to_cacheline(header_size + header->size);

  if (!verify_read_header(header))
    throw torrent::internal_error("Channel::consume_header() header size exceeds buffer size");

  if (new_start_offset > m_size)
    throw torrent::internal_error("Channel::consume_header() new_start_offset exceeds buffer size");

  m_read_offset.store(new_start_offset, std::memory_order_release);
}

} // namespace torrent::system::ipc
