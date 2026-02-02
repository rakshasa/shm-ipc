#include "config.h"

#include "torrent/exceptions.h"

namespace torrent {

const char*
internal_error::what() const noexcept {
  return m_msg.c_str();
}

void
internal_error::initialize(const std::string& msg) {
  m_msg = msg;

#ifdef HAVE_BACKTRACE
  void* stack_ptrs[20];
  int stack_size = ::backtrace(stack_ptrs, 20);
  char** stack_symbol_names = ::backtrace_symbols(stack_ptrs, stack_size);

  if (stack_symbol_names == nullptr) {
    m_backtrace = "backtrace_symbols failed";
    return;
  }

  std::stringstream output;

  for (int i = 0; i < stack_size; ++i) {
    if (stack_symbol_names[i] != nullptr && stack_symbol_names[i] > (void*)0x1000)
      output << stack_symbol_names[i] << '\n';
    else
      output << "stack_symbol: nullptr" << '\n';
  }

  m_backtrace = output.str();
  ::free(stack_symbol_names);

#else
  m_backtrace = "stack dump not enabled";
#endif
}


} // namespace torrent
