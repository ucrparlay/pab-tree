#!/bin/bash
set -o xtrace

./../build/data_generator -p /ssd1/zmen002/kv_pair -n 1000000 -d 2 -file_num 10 -varden 0
./../build/data_generator -p /ssd1/zmen002/kv_pair -n 5000000 -d 2 -file_num 10 -varden 0
./../build/data_generator -p /ssd1/zmen002/kv_pair -n 8000000 -d 2 -file_num 10 -varden 0
./../build/data_generator -p /ssd1/zmen002/kv_pair -n 10000000 -d 2 -file_num 10 -varden 0
./../build/data_generator -p /ssd1/zmen002/kv_pair -n 50000000 -d 2 -file_num 10 -varden 0
