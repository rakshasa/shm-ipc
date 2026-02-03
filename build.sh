#!/bin/bash

source_files=(
  main.cc
  exceptions.cc
  torrent/shm/channel.cc
  torrent/shm/router.cc
  torrent/shm/segment.cc
)

clang++ -std=c++17 -I. -I/opt/local/include -I/opt/local/include  -pthread -g -O2 -DDEBUG -Wall -o test "${source_files[@]}"

chmod +x test
