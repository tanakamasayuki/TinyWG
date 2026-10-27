#include "wg_port.h"

#include <sys/time.h>

#if defined(ESP_PLATFORM)
#include <esp_random.h>
#elif defined(ARDUINO_ARCH_RP2040)
#include <pico/rand.h>
#elif defined(ARDUINO_ARCH_CH32RV)
#include <ch32rv_rng.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <stdio.h>
#endif

namespace tinywg {
namespace port {

#if defined(ESP_PLATFORM)
// The hardware RNG; esp_fill_random() is cryptographically secure while the
// radio (or the bootloader's entropy source) is running.
static bool randomBytes(uint8_t *out, size_t len)
{
    esp_fill_random(out, len);
    return true;
}
#elif defined(ARDUINO_ARCH_RP2040)
static bool randomBytes(uint8_t *out, size_t len)
{
    while (len > 0) {
        uint64_t r = get_rand_64();
        for (int i = 0; i < 8 && len > 0; i++, len--) *out++ = (uint8_t)(r >> (8 * i));
    }
    return true;
}
#elif defined(ARDUINO_ARCH_CH32RV)
// The chip's TRNG. Series without one (V208 among them) return false, and a
// sketch has to supply a source with Device::setRandom().
static bool randomBytes(uint8_t *out, size_t len) { return ch32rv_rng_read(out, len); }
#elif defined(__linux__) || defined(__APPLE__)
static bool randomBytes(uint8_t *out, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t n = fread(out, 1, len, f);
    fclose(f);
    return n == len;
}
#else
#define TINYWG_NO_DEFAULT_RANDOM 1
#endif

RandomFn defaultRandom()
{
#ifdef TINYWG_NO_DEFAULT_RANDOM
    return nullptr;
#else
    return randomBytes;
#endif
}

void defaultWallClock(uint64_t *unixSeconds, uint32_t *nanoseconds)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    *unixSeconds = (uint64_t)tv.tv_sec;
    *nanoseconds = (uint32_t)tv.tv_usec * 1000u;
}

}  // namespace port
}  // namespace tinywg
