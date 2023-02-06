# Rate Limiting for Redis

Rate-limiting is often necessary to prevent a server from being overloaded, an user from abusing a service, etc.

This can be easily implemented in a single server, but gets tricky when you have multiple servers or use a serverless backend.
In these case, your throttling state must be persisted in a separate storage, and Redis works perfectly fine for this.

# Usage

Rate limiters are usually defined by 2 properties:
- How frequently can operations be executed (e.g., 10 ops/s)
- Burst capacity (e.g., up to 100 ops can be executed in a burst).

However, for simplicity, this modules only works with periods (in nanoseconds):
- Period to wait between operations. This can be calculated as `1e9 / ops_per_second`.
- Time necessary to recover the burst capacity. This can be calculated as `1e9 * burst_capacity / ops_per_second`

To use the plugin, call:
```
RATELIMIT KEY1 PERIOD1 CAPACITY1 [ [KEY2] [PERIOD2] [CAPACITY2] ... ]
```

e.g., In order to limit the login of the user `john@example.com` to 1 per minute, with bursts of 5 attempts:

```
RATELIMIT login/john@example.com 60000000000 300000000000
```

The command will respond with an array of integers:
1. *Allowed*: Whether the action was allowed (1) or blocked (0)
  - 0 for blocked
  - 1 for allowed
2. *Remaining*: How many more operations can be executed immediately
  - Always zero if the operation was blocked.
3. *ready_after*: How long to wait (in nanoseconds) before executing the next operations. Always zero if `Remaining > 0`
4. *reset_after*: How long to wait (in nanoseconds) before the full burst capacity is available again.

## Multiple keys

This plugins allows applying multiple rate limits simultaneously:

```
RATELIMIT KEY1 PERIOD1 CAPACITY1 [ KEY2 PERIOD2 CAPACITY2 [ KEY3 PERIOD3 CAPACITY3 [...] ] ]
```

E.g., assume my service should be throttled by:
- User: 1 operations/s, 10 ops burst
- User's organization: 10 ops/s, 100 ops burst
- All requests: 1000/s, 10000 ops burst

```
RATELIMIT user/john@example.com 1000000000 10000000000 org/bigcorp 100000000 1000000000 all 1000000 100000
```

## Testing

For testing purposes, it is possible to override the wall clock used internally with a provided timestamp (in nanoseconds)

```
RATELIMIT KEY1 PERIOD1 CAPACITY1 [ KEY2 PERIOD2 CAPACITY2 [ ... ] ][TIMESTAMP]

RATELIMIT mykey 1000000000 10000000000 60000000000
```


# Isn't there a solution for it already?

Yes, I'm not the first one to implement it in Redis:
- https://github.com/brandur/redis-cell
- https://blog.callr.tech/rate-limiting-for-distributed-systems-with-redis-and-lua/
- https://github.com/rwz/redis-gcra
- https://github.com/wyattjoh/rate-limit-redis

The advantage of this implementation if being able to check multiple limits efficiently, simultaneously and atomically -- Which I haven't seen in any other implementations.

# Why a module?

This could be implemented with a lua script. I've done it and it works fine.

However the lua script is slower (2.4x speedup in benchmark) and suffers with numeric precision issues (There is no int64, only double).
