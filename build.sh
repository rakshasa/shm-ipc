#!/bin/bash

set -eux

source_files=(
  exceptions.cc
  main.cc
  parent.cc
  child.cc
  torrent/net/fd.cc
  torrent/net/socket_address.cc
  torrent/system/event.cc
  torrent/system/poll_kqueue.cc
  torrent/system/ipc/channel.cc
  torrent/system/ipc/control_fd.cc
  torrent/system/ipc/factory.cc
  torrent/system/ipc/router.cc
  torrent/system/ipc/segment.cc
  torrent/system/ipc/wakeup_fd.cc
)

compile_args=(
  -I.
  -I/opt/local/include

  -DUSE_KQUEUE
  -DLT_SMP_CACHE_BYTES=64
  -DDEBUG

  -pthread

  -Wall

  #-Og
  -O0
)

clang++ -std=c++20 -g "${compile_args[@]}" -o test "${source_files[@]}"

chmod +x test
