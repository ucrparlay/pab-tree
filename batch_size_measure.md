Add a test function in the @tests/test_framework.h, which measures the total time to insert/delete a single batch with
  different batch sizes, using uniform distribution.
- We want to measure the total time to delete the same batch using batch_insert/batch_delete versus the single entry insert/delete.
- the batch size is 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000
- The dataset is from the /data/zmen002/kv_pair/uni/1000000000_1/ins/1/1.base.in and 1.batch.in,
- the measure first build a tree using 1.base.in, then for a batch size k, it measures the time to insert/delete first k entries from 1.batch.in using batch_insert/serial_insert. Note both operation should happen in the tree build from 1.base.in, meaning that if you measure them in one run, you need to rebuild the tree before measuring the second operation. 
- The delete path is /data/zmen002/kv_pair/uni/1000000000_1/del/1/1.base.in and 1.batch.in
- Save the results in a CSV file with columns: batch_size, insert_time_batch, insert_time_serial, delete_time_batch, delete_time_serial.
- use the default settings for the batch operations, such as leaf sort predict.
