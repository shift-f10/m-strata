#pragma once
// Include this instead of <windows.h>/<winsock2.h> directly. It fixes the
// macro hygiene that otherwise breaks std::min/std::max and pulls in the
// socket headers in the order Winsock2 requires.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif
