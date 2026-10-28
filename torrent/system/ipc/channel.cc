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
  if (header != header_at_offset(m_read_offset.load(std::memory_order_acquire)))
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

// TODO: Make this return both sides of the wrap, so we can do 2x writes?

uint32_t
Channel::available_write() {
  uint32_t end_offset   = m_write_offset.load(std::memory_order_acquire);
  uint32_t start_offset = m_read_offset.load(std::memory_order_acquire);

  if (end_offset < start_offset)
    return start_offset - end_offset - cache_line_size;

  // Non-wrapped state.
  uint32_t tail_space = m_size - end_offset;
  uint32_t head_space = start_offset;

  // If we can split across wrap-around, both ends can be used by one write sequence.
  if (tail_space >= split_threshold) {
    uint32_t split_space = tail_space + head_space;

    if (split_space <= cache_line_size)
      return 0;

    return split_space - cache_line_size;
  }

  if (head_space <= cache_line_size)
    return tail_space;

  return std::max(tail_space, head_space - static_cast<uint32_t>(cache_line_size));
}

bool
Channel::can_write(uint32_t size) {
  return available_write() >= align_size_to_cacheline(header_size + size) + cache_line_size;
}

// Returns the number of payload bytes written, which may be less than 'size', or -1 with errno set
// to EAGAIN if there is no room for a record (a zero-sized write only succeeds with room for its
// header).
//
// If 'wake_reader' is non-null it is set to whether the reader should be woken up, i.e. if it may
// have run out of data or at least 'write_threshold' bytes are now unread.

int
Channel::write(uint32_t id, uint32_t size, void* data, bool* wake_reader) {
  if (id == 0)
    throw torrent::internal_error("Channel::write() invalid id");

  if (data == nullptr)
    throw torrent::internal_error("Channel::write() data is null");

  if (size == 0)
    throw torrent::internal_error("Channel::write() size is zero");

  if (wake_reader != nullptr)
    *wake_reader = false;

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

  // if (wake_reader != nullptr) {
  //   // Re-read the offset as the reader may have caught up since we started writing; if it has
  //   // consumed everything before this write it may already be polling.
  //   size_t read_offset = m_read_offset.load(std::memory_order_acquire);
  //   size_t unread      = (new_end_offset + m_size - read_offset) % m_size;

  //   *wake_reader = read_offset == old_offset || read_offset == first_offset || unread >= m_write_threshold;
  // }

  return first_size + second_size;
}

Channel::header_type*
Channel::read_header() {
  uint32_t start_offset = m_read_offset.load(std::memory_order_acquire);
  uint32_t end_offset   = m_write_offset.load(std::memory_order_acquire);

  if (start_offset == end_offset)
    return nullptr;

  std::atomic_thread_fence(std::memory_order_acquire);

  auto header = header_at_offset(start_offset);

  if (header->size == ~uint32_t{0}) {
    // Padding header, wrap around.
    if (start_offset < end_offset)
      throw torrent::internal_error("Channel::read_header() padding header but no wrap");

    start_offset = 0;
    header       = header_at_offset(start_offset);

    if (start_offset == end_offset)
      throw torrent::internal_error("Channel::read_header() padding header but no data after wrap");

    if (header->size == ~uint32_t{0})
      throw torrent::internal_error("Channel::read_header() consecutive padding headers");

    // Free up the padding header for the next write and ensure verify_read_header() works correctly.
    m_read_offset.store(start_offset, std::memory_order_release);
  }

  if (!verify_read_header(header))
    throw torrent::internal_error("Channel::read_header() header size exceeds buffer size");

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

  if (new_start_offset == m_size)
    new_start_offset = 0;

  m_read_offset.store(new_start_offset, std::memory_order_release);
}

} // namespace torrent::system::ipc
