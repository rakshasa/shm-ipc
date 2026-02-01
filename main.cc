#include <cerrno>
#include <algorithm>
#include <iostream>
#include <unistd.h>
#include <sys/shm.h>

// sysctl -A | grep shm

namespace shm {

// Open using shmget() as a private segment, then fork.

// TODO: Change this to store the buffer?

class SharedSegment {
public:
  static constexpr size_t page_size = 4096;

  SharedSegment() = default;
  ~SharedSegment();

  void                create(size_t size);
  void                close();

  void                attach();
  void                detach();

  void*               address()       { return m_addr; }
  size_t              size() const    { return m_size; }

private:
  int                 m_shm_id{-1};
  size_t              m_size{};
  void*               m_addr{};
};

SharedSegment::~SharedSegment() {
  close();
}

void
SharedSegment::create(size_t size) {
  if (m_shm_id != -1)
    throw std::runtime_error("SharedSegment::create() segment already created");

  if (m_size != 0)
    throw std::runtime_error("SharedSegment::create() segment size already set");

  if (size == 0 || (size % page_size) != 0)
    throw std::invalid_argument("SharedSegment::create() size must be non-zero and a multiple of page size");

  auto fd = shmget(IPC_PRIVATE, size, IPC_CREAT | 0600);

  if (fd == -1)
    throw std::runtime_error("shmget() failed: " + std::string(strerror(errno)));

  m_shm_id = fd;
  m_size   = size;
}

void
SharedSegment::close() {
  if (m_shm_id == -1)
    return;

  if (shmctl(m_shm_id, IPC_RMID, nullptr) == -1)
    throw std::runtime_error("shmctl(IPC_RMID) failed: " + std::string(strerror(errno)));
}

void
SharedSegment::attach() {
  void* addr = shmat(m_shm_id, nullptr, 0);

  if (addr == (void*)-1)
    throw std::runtime_error("shmat() failed: " + std::string(strerror(errno)));

  m_addr = addr;
}

void
SharedSegment::detach() {
  if (m_addr == nullptr)
    return;

  if (shmdt(m_addr) == -1)
    throw std::runtime_error("shmdt() failed: " + std::string(strerror(errno)));
}

// ChannelWriter and ChannelReader
//
// Initialize with SharedSegment, take void* pointer and size_t.
//
// The class itself is cast from the void* pointer, initialized in place.
//
// Add synchronization mechanisms at the top using std::mutex, use a private ChannelBase inheritance.
//
// Each write should have a header and size to allow reading of multiple messages, then wrap around at end.
//
// When full, return false and we can use a signal fd to wait for space. This is intended to be used
// with a poll so needs to be non-blocking.
//

// This does however mean the writer side likely needs to buffer excess data until it can be
// written, as we can't allow blocking.
//
// We should perhaps throttle e.g. libcurl reads based on available space in the channel. Use
// CURL_MAX_WRITE_SIZE to limit.

// Wrap messages around, keep count of outstanding messages + size remaining?
//
// Rather keep start and end offsets in atomic values.
//
// Use atomic swap to read offsets, however need mutex to read/write data?

// When the reader checks for available data, it can read the start and end offsets atomically.
//
// The values are copied to local variables, a memory barrier is used to ensure data visibility, then the data is read.

// The writer can just write data and update the end offset atomically.

// To check for available reads, the reader reads start and end offsets atomically in that order.


// Align new messages to 8 byte boundary?


constexpr uint32_t
align_to_cacheline(uint32_t size) {
  uint32_t cache_line_size = std::hardware_destructive_interference_size;
  return (size + (cache_line_size - 1)) & ~(cache_line_size - 1);
}

// Channels are intended for one writer, one reader, of data blocks less than ~1/10th the channel size.
//
// Querying available write space selects the largest of the contiguous free spaces, so when
// wrapping the free space may be smaller than total free space.

class ChannelMetadata {
public:
  struct [[gnu::packed]] header_type {
    uint32_t    size{};
    uint32_t    id{};
    char        data[];
  };

  static constexpr size_t header_size     = sizeof(header_type);
  static constexpr size_t cache_line_size = std::hardware_destructive_interference_size;

  void                initialize(void* addr, size_t size);

  // There will always be at least one cache line free, however this cannot be used.
  uint32_t            available_write();

  bool                write(uint32_t id, uint32_t size, void* data);

  header_type*        read_header();
  void                consume_header(header_type* header);

protected:
  // These are offset by size of ChannelBase.
  void*                 m_addr{};
  uint32_t              m_size{};

  std::atomic<uint32_t> m_read_offset{};
  std::atomic<uint32_t> m_write_offset{};

  // Align to cache line: mutex.
};

void
ChannelMetadata::initialize(void* addr, size_t size) {
  m_addr = static_cast<char*>(addr) + align_to_cacheline(sizeof(ChannelMetadata));
  m_size = size - align_to_cacheline(sizeof(ChannelMetadata));

  m_read_offset = 0;
  m_write_offset = 0;
}

// Wrap data write around end of buffer.

// Do we make it into two writes if it wraps?

//std::atomic_thread_fence(std::memory_order_release)
//std::atomic_thread_fence(std::memory_order_acquire)

// Does not include header size, which can be sizeof(header_type).
uint32_t
ChannelMetadata::available_write() {
  // These are cacheline aligned atomic values.
  uint32_t start_offset = m_read_offset.load(std::memory_order_acquire);
  uint32_t end_offset   = m_write_offset.load(std::memory_order_acquire);

  if (end_offset >= start_offset)
    return std::max(m_size - end_offset, start_offset);
  else
    return start_offset - end_offset;
}

// TODO: Need to align writes?

bool
ChannelMetadata::write(uint32_t id, uint32_t size, void* data) {
  if (id == 0)
    throw std::invalid_argument("ChannelMetadata::write() invalid id");

  if (size > m_size - header_size)
    throw std::invalid_argument("ChannelMetadata::write() invalid size");

  size_t total_size = align_to_cacheline(header_size + size);

  // If we're wrapping around, add a padding header. (size == 0)
  size_t start_offset = m_read_offset.load(std::memory_order_acquire);
  size_t end_offset   = m_write_offset.load(std::memory_order_acquire);

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
    if (start_offset < total_size + cache_line_size)
      return false;

    auto padding_header = reinterpret_cast<header_type*>(static_cast<char*>(m_addr) + end_offset);
    padding_header->size = ~uint32_t{0};
    padding_header->id   = 0;

    end_offset = 0;

  } else {
    // Sufficient space at end.
  }

  auto header = reinterpret_cast<header_type*>(static_cast<char*>(m_addr) + end_offset);
  header->size = size;
  header->id   = id;

  size_t new_end_offset = end_offset + total_size;

  if (new_end_offset > m_size)
    throw std::runtime_error("ChannelMetadata::write() internal error: new_end_offset exceeds buffer size");

  if (new_end_offset == m_size)
    new_end_offset = 0;

  std::memcpy(header->data, data, size);

  m_write_offset.store(new_end_offset, std::memory_order_release);

  std::atomic_thread_fence(std::memory_order_release);
  return true;
}

ChannelMetadata::header_type*
ChannelMetadata::read_header() {
  size_t start_offset = m_read_offset.load(std::memory_order_acquire);
  size_t end_offset   = m_write_offset.load(std::memory_order_acquire);

  if (start_offset == end_offset)
    return nullptr;

  std::atomic_thread_fence(std::memory_order_acquire);

  auto header = reinterpret_cast<header_type*>(static_cast<char*>(m_addr) + start_offset);

  if (header->size == ~uint32_t{0}) {
    // Padding header, wrap around.
    start_offset = 0;
    header = reinterpret_cast<header_type*>(static_cast<char*>(m_addr) + start_offset);

    if (start_offset == end_offset)
      throw std::runtime_error("ChannelMetadata::read_header() internal error: padding header but no data after wrap");

    if (header->size == ~uint32_t{0})
      throw std::runtime_error("ChannelMetadata::read_header() internal error: consecutive padding headers");
  }

  if (header->data + header->size > static_cast<char*>(m_addr) + m_size)
    throw std::runtime_error("ChannelMetadata::read_header() internal error: header size exceeds buffer size");

  return header;
}

void
ChannelMetadata::consume_header(header_type* header) {
  // size_t start_offset     = m_read_offset.load(std::memory_order_acquire);
  // size_t new_start_offset = start_offset + align_to_cacheline(header_size + header->size);

  // We can compute new_start_offset directly from header pointer.
  size_t header_offset    = reinterpret_cast<char*>(header) - static_cast<char*>(m_addr);
  size_t new_start_offset = header_offset + align_to_cacheline(header_size + header->size);

  if (new_start_offset > m_size)
    throw std::runtime_error("ChannelMetadata::consume_header() internal error: new_start_offset exceeds buffer size");

  if (new_start_offset == m_size)
    new_start_offset = 0;

  m_read_offset.store(new_start_offset, std::memory_order_release);
}

} // namespace shm

void
child_process(shm::SharedSegment* segment) {
  std::cout << "Child process started, reading messages..." << std::endl;

  auto metadata = static_cast<shm::ChannelMetadata*>(segment->address());

  while (true) {
    std::cout << "Child process checking for message..." << std::endl;

    auto header = metadata->read_header();

    if (header == nullptr) {
      std::cout << "Child process: no message available, waiting..." << std::endl;
      // sleep(1);
      // sleep shorter
      usleep(100); // 100 ms
      continue;
    }

    std::cout << "Child process received message (id=" << header->id << "): " << header->data << std::endl;

    metadata->consume_header(header);

    // sleep(1);
  }
}

void
parent_process(shm::SharedSegment* segment) {
  std::cout << "Parent process started, writing messages..." << std::endl;

  auto metadata = static_cast<shm::ChannelMetadata*>(segment->address());

  while (true) {
    std::cout << "Parent process writing message..." << std::endl;

    const char* message = "Hello from parent process!";

    while (!metadata->write(1, strlen(message) + 1, (void*)message)) {
      std::cout << "Parent process: channel full, waiting..." << std::endl;
      // sleep(2);
      usleep(10000); // 10 s
    }

    // sleep(5);
    usleep(500); // 500 ms
  }
}

int
main() {
  auto segment = std::make_unique<shm::SharedSegment>();

  // segment->create(12 * shm::SharedSegment::page_size);
  segment->create(1 * shm::SharedSegment::page_size);
  segment->attach();

  auto metadata = static_cast<shm::ChannelMetadata*>(segment->address());

  metadata->initialize(segment->address(), segment->size());

  pid_t pid = ::fork();

  if (pid == -1)
    throw std::runtime_error("fork() failed: " + std::string(strerror(errno)));

  if (pid == 0)
    child_process(segment.get());
  else
    parent_process(segment.get());

  ::wait(nullptr);

  return 0;
}
