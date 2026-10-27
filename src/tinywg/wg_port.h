// What the WG core needs from the platform: cryptographic random bytes and a
// wall clock for the TAI64N handshake timestamp. Defaults are picked per core
// in wg_port.cpp; a sketch can replace either through Device::setRandom() /
// Device::setWallClock().
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tinywg {

// Fills out with len cryptographically secure random bytes. false when no
// source is available (the caller must not use out then).
typedef bool (*RandomFn)(uint8_t *out, size_t len);

// The current wall-clock time as Unix seconds and nanoseconds. Only the
// initiator uses it, and only its monotonicity across reboots matters to the
// responder (SPEC.ja.md §4.4).
typedef void (*WallClockFn)(uint64_t *unixSeconds, uint32_t *nanoseconds);

namespace port {

// The default random source of this core, or nullptr when it has none.
RandomFn defaultRandom();
void defaultWallClock(uint64_t *unixSeconds, uint32_t *nanoseconds);

}  // namespace port
}  // namespace tinywg
