// Frame-time spikes and what loading did in them (NOMA_LOAD_STATS=ms): every game frame's time,
// and for each frame longer than ms, what happened during it: the guest threads' CPU time, the
// content reads (bytes, files, how long the guest waited), CX decompression, NAND reads, heap
// allocations and the GPU thread's texture uploads. A summary every NOMA_LOAD_STATS_EVERY seconds
// (default 10) gives the frame-time distribution, and a line per loading burst (reads with less
// than a second between them) its time, bytes and decompression. NOMA_LOAD_STATS_READS=1 also logs
// every content read.
#pragma once

#include "guest.h"

namespace guest {

extern bool g_lstats;
void lstats_init();
void lstats_frame();                      // vi: the game showed a new frame
void lstats_thread(u32 thread, u64 us);   // irq.cpp: CPU time charged to a guest thread
void lstats_idle(u64 us);                 // irq.cpp: every guest thread waited
// a content read: index, offset, size; queued (CPU thread), the I/O thread's start and end, and the
// reply to the guest (all host_time_us)
void lstats_read(u32 content, u64 off, u32 n, u64 queued, u64 start, u64 end, u64 replied);
// a content's archive: its files' names and places, read from the file (any thread; null if none),
// then given to the stats with the content's index (the CPU's thread)
void* lstats_parse_names(const char* host_path);
void lstats_content_open(u32 index, void* names);
// a CX call (on the CPU's thread): its time leaves out other threads' time inside its poll points
u64 lstats_cx_begin();
void lstats_cx_end(u64 t0, bool huffman, u32 in, u32 out);
// any other I/O job (ios.cpp: the file system's requests, contents opened and closed)
void lstats_job(u64 queued, u64 start, u64 end, u64 replied);
void lstats_alloc(u32 n);
// the GPU's thread (any thread): a texture decoded and uploaded
void lstats_texture(u32 bytes, u64 us);

}  // namespace guest
