#include <stdio.h>

        int main() {
            int named = thread_spawn_named("delay", "clike_worker", 5);
            int named_wait = WaitForThread(named);
            int named_ok = (named_wait == 0) ? 1 : 0;

            int pooled = thread_pool_submit("delay", "clike_pool", 5);
            int pooled_wait = WaitForThread(pooled);
            int lookup = thread_lookup("clike_pool");
            int pooled_ok = (pooled_wait == 0) ? 1 : 0;
            int lookup_match = (lookup == pooled) ? 1 : 0;
            int stats_len = length(thread_stats());

            printf("named_status=%d
", named_ok);
            printf("pooled_status=%d lookup_match=%d stats=%d
", pooled_ok, lookup_match, stats_len);
            printf("stats_json=%s
", ThreadStatsJson());
            return 0;
        }