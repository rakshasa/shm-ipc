#include "config.h"

#include "torrent/system/ipc/channel.h"

#include <algorithm>
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

void
Channel::initialize(size_t size) {
  if (reinterpret_cast<uintptr_t>(this) % LT_SMP_CACHE_BYTES != 0)
    throw torrent::internal_error("Channel::initialize() this must be aligned to cache line size");

  if (reinterpret_cast<uintptr_t>(m_addr) % LT_SMP_CACHE_BYTES != 0)
    throw torrent::internal_error("Channel::initialize() m_addr must be aligned to cache line size");

  if (size == 0 || (size % LT_SMP_CACHE_BYTES) != 0)
    throw torrent::internal_error("Channel::initialize() size must be non-zero and a multiple of cache line size");

  m_size = size - (reinterpret_cast<char*>(m_addr) - reinterpret_cast<char*>(this));

  // TODO: This was supposed to be a 10% buffer of the channel size to be used for closing channel
  // connections.
  m_write_threshold = align_size_to_cacheline(m_size / 10) + 2 * cache_line_size;

  m_read_offset    = 0;
  m_write_offset   = 0;
  m_consumer_state = 0;
}

// TODO: Make this return both sides of the wrap, so we can do 2x writes?

uint32_t
Channel::available_write() {
  uint32_t end_offset   = m_write_offset.load(std::memory_order_acquire);
  uint32_t start_offset = m_read_offset.load(std::memory_order_acquire);

  if (end_offset >= start_offset)
    return std::max(m_size - end_offset, start_offset);

  return start_offset - end_offset;
}

bool
Channel::can_write(uint32_t size) {
  return available_write() >= align_size_to_cacheline(header_size + size) + cache_line_size;
}

// TODO: Need to align writes?

// TODO: When returning false, we need to also save the start_offset to indicate when we need to wake up producer.
// TODO: To handle the cases when a spurious write happens later, clear out the wakeup_offset.
// TODO: We set the wakeup_offset, then compare again to the start_offset, if changed we call write again.

// TODO: Reconsider above, we should never fail on write() if we check available_write() first.
// TODO: Perhaps add a strict available_write_or_wakeup function that strictly handles this.

// If the data does not fit at the end of the buffer, then:
//
// - If at least 'split_threshold' bytes remain at the end, the data is split into two records with
//   the same id, the first filling the end of the buffer and the second starting at offset 0. Both
//   become visible to the reader with a single update of the write offset.
// - Otherwise a padding header is written at the end and the whole data is written after the wrap.

bool
Channel::write(uint32_t id, uint32_t size, void* data) {
  if (id == 0)
    throw torrent::internal_error("Channel::write() invalid id");

  if (size > m_size - header_size)
    throw torrent::internal_error("Channel::write() size exceeds buffer size");

  if (data == nullptr && size != 0)
    throw torrent::internal_error("Channel::write() data is null but size is non-zero");

  size_t total_size = align_size_to_cacheline(header_size + size);

  size_t start_offset = m_read_offset.load(std::memory_order_acquire);
  size_t end_offset   = m_write_offset.load(std::memory_order_acquire);

  // Set when the write is split; the size of the payload written at the end of the buffer.
  size_t first_size = size;
  bool   split      = false;

  // We keep a cache line free to distinguish full/empty.

  if (end_offset < start_offset) {
    // We're in wrapped state.
    if (start_offset - end_offset < total_size + cache_line_size)
      return false;

  } else if (end_offset == m_size) {
    // At end, need to wrap.
    if (start_offset < total_size + cache_line_size)
      return false;

    end_offset = 0;

  } else if (m_size - end_offset < total_size) {
    // Not enough space at end, need to wrap.
    size_t remaining = m_size - end_offset;

    if (remaining >= split_threshold) {
      // Split the write in two; the first record fills the end of the buffer exactly, as
      // 'remaining' is a multiple of the cache line size.
      first_size = remaining - header_size;

      size_t second_total = align_size_to_cacheline(header_size + (size - first_size));

      if (start_offset < second_total + cache_line_size)
        return false;

      split = true;

    } else {
      // Too little left at the end, pad and write everything after the wrap.
      if (start_offset < total_size + cache_line_size)
        return false;

      auto padding_header = header_at_offset(end_offset);
      padding_header->size = ~uint32_t{};
      padding_header->id   = 0;

      end_offset = 0;
    }

  } else {
    // Sufficient space at end.
  }

  auto header = header_at_offset(end_offset);
  header->size = first_size;
  header->id   = id;

  std::memcpy(header->data, data, first_size);

  size_t new_end_offset;

  if (split) {
    size_t second_size = size - first_size;

    auto second_header = header_at_offset(0);
    second_header->size = second_size;
    second_header->id   = id;

    std::memcpy(second_header->data, static_cast<char*>(data) + first_size, second_size);

    new_end_offset = align_size_to_cacheline(header_size + second_size);

  } else {
    new_end_offset = end_offset + total_size;
  }

  if (new_end_offset % cache_line_size != 0)
    throw torrent::internal_error("Channel::write() new_end_offset not aligned to cache line size");

  if (new_end_offset > m_size)
    throw torrent::internal_error("Channel::write() new_end_offset exceeds buffer size");

  if (new_end_offset == m_size)
    new_end_offset = 0;

  std::atomic_thread_fence(std::memory_order_release);
  m_write_offset.store(new_end_offset, std::memory_order_release);

  return true;
}

Channel::header_type*
Channel::read_header() {
  size_t start_offset = m_read_offset.load(std::memory_order_acquire);
  size_t end_offset   = m_write_offset.load(std::memory_order_acquire);

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
  size_t header_offset    = as_c_ptr(header) - as_c_ptr(header_at_offset(0));
  size_t new_start_offset = header_offset + align_size_to_cacheline(header_size + header->size);

  if (!verify_read_header(header))
    throw torrent::internal_error("Channel::consume_header() header size exceeds buffer size");

  if (new_start_offset > m_size)
    throw torrent::internal_error("Channel::consume_header() new_start_offset exceeds buffer size");

  if (new_start_offset == m_size)
    new_start_offset = 0;

  m_read_offset.store(new_start_offset, std::memory_order_release);
}

} // namespace torrent::system::ipc
