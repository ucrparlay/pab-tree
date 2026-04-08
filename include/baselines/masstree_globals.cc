// Global variables required by Masstree's threadinfo system
#include "kvthread.hh"

relaxed_atomic<mrcu_epoch_type> active_epoch(1);
relaxed_atomic<mrcu_epoch_type> globalepoch(1);
