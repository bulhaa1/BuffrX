#pragma once
#include "windows.h"
#include <sys/random.h>
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
inline long BCryptGenRandom(void*, unsigned char* buffer, unsigned long count, unsigned long) {
  size_t done = 0;
  while (done < count) {
    const ssize_t got = getrandom(buffer + done, count - done, 0);
    if (got <= 0) return -1;
    done += static_cast<size_t>(got);
  }
  return 0;
}
