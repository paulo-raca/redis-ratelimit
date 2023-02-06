#include <time.h>
#include <cinttypes>
#include <limits>
#include <algorithm>

extern "C" {
#include "redismodule.h"
}

using namespace std;

typedef long long int ll_t;

typedef struct {
    // Inputs from redis call
    RedisModuleKey* key_handle;
    ll_t cost;
    ll_t capacity;

    // Inputs from Redis state
    ll_t available;

    // Outputs
    ll_t out_remaining;
    ll_t out_ready_after;
    ll_t out_reset_after;
} LimitRequest;

typedef struct {
    bool allowed;
    ll_t remaining;
    ll_t ready_after;
    ll_t reset_after;
} LimitResponse;

int RateLimitImpl(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
    // Discard 1st argument (cmd name)
    argv++;
    argc--;
    // Extract timestamp
    ll_t now;
    if (argc % 3 == 0) {
        struct timespec tv;
        clock_gettime(CLOCK_REALTIME, &tv);
        now = ((ll_t)tv.tv_sec) * 1000000000 + tv.tv_nsec;
    } else if (argc % 3 == 1) {
        if (RedisModule_StringToLongLong(argv[argc-1], &now) != REDISMODULE_OK) {
            return RedisModule_ReplyWithError(ctx, "Invalid arguments: cannot parse timestamp");
        }
    } else {
      return RedisModule_WrongArity(ctx);
    }

    LimitResponse response = {
        .allowed = true,
        .remaining = numeric_limits<ll_t>::max(),
        .ready_after = 0,
        .reset_after = 0,
    };

    int n = argc / 3;
    LimitRequest* limits = (LimitRequest*)RedisModule_Calloc(n, sizeof(LimitRequest));

    for (int i=0; i<n; i++) {
        LimitRequest& limit = limits[i];
        if (RedisModule_StringToLongLong(argv[3*i+1], &limit.cost) != REDISMODULE_OK) {
            return RedisModule_ReplyWithError(ctx, "Invalid arguments: cannot parse cost");
        }
        if (RedisModule_StringToLongLong(argv[3*i+2], &limit.capacity) != REDISMODULE_OK) {
            return RedisModule_ReplyWithError(ctx, "Invalid arguments: cannot capacity");
        }
        if (limit.cost < 0) {
            return RedisModule_ReplyWithError(ctx, "Invalid arguments: negative cost");
        }
        if (limit.capacity < 0) {
            return RedisModule_ReplyWithError(ctx, "Invalid arguments: negative capacity");
        }
        if (limit.cost > limit.capacity) {
            return RedisModule_ReplyWithError(ctx, "Invalid arguments: capacity is smaller than cost");
        }

        limit.key_handle = (RedisModuleKey*)RedisModule_OpenKey(ctx, argv[3*i+0], REDISMODULE_READ | REDISMODULE_WRITE);
        if (limit.key_handle == nullptr) {
            return RedisModule_ReplyWithError(ctx, "error accessing key");
        }
        switch (RedisModule_KeyType(limit.key_handle)) {
            case REDISMODULE_KEYTYPE_EMPTY: {
                limit.available = limit.capacity;  // this is a new rate limiter, burst ready
                break;
            }
            case REDISMODULE_KEYTYPE_STRING: {
                size_t reset_at_len;
                char* reset_at_str = RedisModule_StringDMA(limit.key_handle, &reset_at_len, REDISMODULE_READ);
                if (reset_at_str == nullptr) {
                    return RedisModule_ReplyWithError(ctx, "error accessing key");
                }
                ll_t reset_at;
                if (sscanf(reset_at_str, "%lld", &reset_at) != 1) {
                    return RedisModule_ReplyWithError(ctx, "error parsing value");
                }
                limit.available = max(ll_t(0), min(limit.capacity, now + limit.capacity - reset_at));
                break;
            }
            default: {
                return RedisModule_ReplyWithError(ctx, REDISMODULE_ERRORMSG_WRONGTYPE);
            }
        }

        if (limit.available < limit.cost) {
            response.allowed = false;
        }
    }

    for (int i=0; i<n; i++) {
        LimitRequest& limit = limits[i];
        if (response.allowed) {
            limit.available -= limit.cost;
        }
        ll_t remaining = limit.available / limit.cost;
        ll_t ready_after = limit.cost > limit.available ? limit.cost - limit.available : 0;
        ll_t reset_after = limit.capacity - limit.available;
        response.remaining = min(response.remaining, remaining);
        response.ready_after = max(response.ready_after, ready_after);
        response.reset_after = max(response.reset_after, reset_after);

        if (RedisModule_StringSet(limit.key_handle, RedisModule_CreateStringFromLongLong(ctx, now + reset_after)) != REDISMODULE_OK) {
          return RedisModule_ReplyWithError(ctx, "error updating value");
        }
        if (RedisModule_SetExpire(limit.key_handle, reset_after / 1000000 + 1) != REDISMODULE_OK) {
            return RedisModule_ReplyWithError(ctx, "error setting expiration");
        }
    }

    RedisModule_ReplyWithArray(ctx, 4);
    RedisModule_ReplyWithBool(ctx, response.allowed); // TODO: Use ReplyWithBool on Redis 7
    RedisModule_ReplyWithLongLong(ctx, response.remaining);
    RedisModule_ReplyWithLongLong(ctx, response.ready_after);
    RedisModule_ReplyWithLongLong(ctx, response.reset_after);

    return REDISMODULE_OK;
}

RedisModuleCommandInfo info = {
      .version = REDISMODULE_COMMAND_INFO_VERSION,
      .summary = "Verifies if an operation should be rate-limited",
      .arity = -3,
      .key_specs = (RedisModuleCommandKeySpec[]){
          {
              .flags = REDISMODULE_CMD_KEY_RW | REDISMODULE_CMD_KEY_UPDATE,
              .begin_search_type = REDISMODULE_KSPEC_BS_INDEX,
              .bs = {
                  .index = {
                      .pos = 1,
                  }
              },
              .find_keys_type = REDISMODULE_KSPEC_FK_RANGE,
              .fk = {
                .range = {
                  .lastkey = -1,
                  .keystep = 3,
                  .limit = 0,
                }
              }
          },
          {0},
      },
      .args = (RedisModuleCommandArg[]){
          {
              .name = "key",
              .type = REDISMODULE_ARG_TYPE_KEY,
              .key_spec_index = 0,
              .summary = "Identifier of a rate-limiter. e.g., 'ratelimit/login/user/johnsmith', 'ratelimit/login/ip/1.2.3.4'",
              .flags = REDISMODULE_CMD_ARG_MULTIPLE,
          },
          {
              .name = "period",
              .type = REDISMODULE_ARG_TYPE_INTEGER,
              .key_spec_index = 0,
              .summary = "Minimum period between executions, in nanoseconds.",
              .flags = REDISMODULE_CMD_ARG_MULTIPLE,
          },
          {
              .name = "capacity",
              .type = REDISMODULE_ARG_TYPE_INTEGER,
              .key_spec_index = 0,
              .summary = "Maximum idle capacity to retain for bursts, in nanoseconds.",
              .flags = REDISMODULE_CMD_ARG_MULTIPLE,
          },
          {0},
      },
  };

extern "C" int RedisModule_OnLoad(RedisModuleCtx *ctx) {

  // Register the module itself
  if (RedisModule_Init(ctx, "ratelimit", 1, REDISMODULE_APIVER_1) != REDISMODULE_OK) {
    return REDISMODULE_ERR;
  }

  // register example.parse - the default registration syntax
  if (RedisModule_CreateCommand(ctx, "ratelimit", RateLimitImpl, "write fast", 1, -1, 3) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  RedisModuleCommand *command = RedisModule_GetCommand(ctx, "ratelimit");

  if (RedisModule_SetCommandInfo(command, &info) != REDISMODULE_OK) {
      return REDISMODULE_ERR;
  }

  return REDISMODULE_OK;
}
