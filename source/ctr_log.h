#pragma once

// Debug logger, writing sdmc:/3ds/mii-transfer/debug.log - pull the SD card
// (or use a file browser homebrew) and read it after a run that didn't
// behave as expected. Disabled outright by default (kLoggingEnabled in
// ctr_log.cpp) - see that file's own comment: this app's whole hang-hunting
// history turned out to be self-inflicted by this logging code's I/O
// latency once per-draw-call diagnostic logging pushed call volume high
// enough, so flip that flag on only for active, temporary debugging.
//
// A UDP broadcast side-channel (the 3DS equivalent of the Wii U build's
// WHBLogUdp) used to exist alongside the file for convenient live viewing
// without touching the SD card, but has been removed - it turned out to
// contribute to that same self-inflicted latency (soc:u sockets go through
// IPC to the soc: system module, so even a "fire and forget" UDP send has a
// real per-call cost), and was always secondary to debug.log anyway (UDP
// packets can silently go missing; the file was always the source of truth).
namespace CtrLog {

// Safe to call even if the file can't be opened - Printf() is then a
// silent no-op, since logging must never be able to crash or block the app
// it exists to help debug.
void Init();

void Shutdown();

// printf-style (no trailing newline needed - one is always appended).
void Printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace CtrLog
