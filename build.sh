#!/bin/bash

set -eux

source_files=(
  exceptions.cc
  main.cc
  parent.cc
  child.cc
  torrent/event.cc
  torrent/shm/channel.cc
  torrent/shm/control_fd.cc
  torrent/shm/factory.cc
  torrent/shm/router.cc
  torrent/shm/segment.cc
  torrent/system/poll_kqueue.cc
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
