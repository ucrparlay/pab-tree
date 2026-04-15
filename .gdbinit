set history filename ~/.gdb_history
set history save on
set history size 100000
set history expansion on

define rebuild
  shell make b_test
end

define go
  run -p /ssd1/zmen002/kv_pair/exp/1000000_1/ins/100/1.base.in -d 1 -t 8 -r 2 -q 0 -s 0 -b 1.0 -T 0 -D exp
end
