// TinyWG: a portable WireGuard implementation for Arduino, initiator and responder.
//
//   TinyWG     wg0 as an lwIP network interface (ESP32, CH32V with the Network library)
//   TinyWGUdp  over any Arduino UDP, raw IPv4 packets in and out (any core)
//   TinyWGTimeServer  an optional SNTP server for clients with no Internet
//
// The protocol engine on its own is tinywg::Device (tinywg/wg_core.h).
#pragma once

#include "tinywg_version.h"
#include "TinyWGBase.h"
#include "tinywg/wg_cookie.h"
#include "TinyWGLwip.h"
#include "TinyWGUdp.h"
#include "TinyWGTimeServer.h"
