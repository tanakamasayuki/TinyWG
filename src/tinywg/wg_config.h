// Build-time limits. Every value can be overridden with a -D build flag
// (arduino-cli: --build-property "compiler.cpp.extra_flags=-DTINYWG_MAX_PEERS=8").
// The defaults are provisional until measured on the targets (SPEC.ja.md §5).
#pragma once

// Peers per device.
#ifndef TINYWG_MAX_PEERS
#define TINYWG_MAX_PEERS 4
#endif

// AllowedIPs entries per peer.
#ifndef TINYWG_MAX_ALLOWED_IPS
#define TINYWG_MAX_ALLOWED_IPS 4
#endif

// Largest inner MTU: sizes the packet buffers. The MTU in use can be lowered at
// run time with setMtu(); lowering this as well saves RAM.
#ifndef TINYWG_MTU
#define TINYWG_MTU 1420
#endif

// Replay window in bits, a multiple of 32 and at least 64. Out-of-order packets
// older than (TINYWG_REPLAY_WINDOW - 32) are dropped.
#ifndef TINYWG_REPLAY_WINDOW
#define TINYWG_REPLAY_WINDOW 128
#endif

// 1: hold the last packet sent while a handshake is in progress and send it when
// the session comes up (costs TINYWG_MTU bytes of RAM). 0: drop it.
#ifndef TINYWG_STAGED_PACKET
#define TINYWG_STAGED_PACKET 1
#endif

#if (TINYWG_REPLAY_WINDOW % 32) != 0 || TINYWG_REPLAY_WINDOW < 64
#error "TINYWG_REPLAY_WINDOW must be a multiple of 32 and at least 64"
#endif
#if TINYWG_MTU < 576 || TINYWG_MTU > 1500
#error "TINYWG_MTU must be between 576 and 1500"
#endif
