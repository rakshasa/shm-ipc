#!/bin/bash

clang++ -std=c++17 -I.  -I/opt/local/include -I/opt/local/include  -pthread -g -O2 -DDEBUG -Wall -o test main.cc

chmod +x test
