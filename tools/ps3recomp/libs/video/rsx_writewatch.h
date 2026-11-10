/*
 * rsx_writewatch -- which pages of guest memory were written, and when.
 *
 * The draw engine keeps converted geometry and decoded textures and has to
 * notice when the guest rewrites them. Hashing every cached byte on every use
 * was the largest single cost of a draw (Zolar Forest: ~20 MB of textures a
 * frame plus every draw's vertices). On Windows the guest's address space is
 * reserved with MEM_WRITE_WATCH, so the OS tracks every write to it -- by the
 * PPU, SPU DMA, file reads, anything -- and this hands that out as epochs:
 *
 *   rsx_ww_poll()   once per FIFO pass, after `put` is read: every page written
 *                   since the last poll gets the new epoch. The guest writes
 *                   a draw's data before it moves put, so a draw's data is
 *                   always stamped before the draw is processed.
 *   rsx_ww_poll_heap()
 *                   the same for the title's heap range only (0x20000000), at
 *                   a FIFO semaphore ACQUIRE: SPU jobs fill vertex data there
 *                   after the commands are queued and release the label, with
 *                   no move of put. A third of the cost of a full poll.
 *   rsx_ww_written_since(p, n, e)
 *                   was any page of [p, p+n) stamped after epoch e? A reader
 *                   that validated its copy during epoch e asks this; a write
 *                   that raced the validation lands in e+1 and is caught.
 *   rsx_ww_self_begin(p, n) / rsx_ww_self_end(p, n)
 *                   around the engine's own writes into guest memory (a GPU
 *                   target read back into it): they are not the guest's, and
 *                   must not mark the other targets on the same pages as
 *                   rewritten by the game (rsx_ww_guest_written_since).
 *
 * Off (rsx_ww_on() == 0) when the reservation has no write watch, on non-
 * Windows hosts, or with RACACIT_NO_WRITE_WATCH=1: callers then hash.
 */
#ifndef PS3RECOMP_RSX_WRITEWATCH_H
#define PS3RECOMP_RSX_WRITEWATCH_H

#ifdef __cplusplus
extern "C" {
#endif

int          rsx_ww_on(void);
void         rsx_ww_poll(void);
void         rsx_ww_poll_heap(void);
unsigned int rsx_ww_epoch(void);
int          rsx_ww_written_since(const void* host, unsigned int len, unsigned int epoch);
int          rsx_ww_guest_written_since(const void* host, unsigned int len, unsigned int epoch);
int          rsx_ww_watched(const void* host, unsigned int len);
void         rsx_ww_self_begin(const void* host, unsigned int len);
void         rsx_ww_self_end(const void* host, unsigned int len);

#ifdef __cplusplus
}
#endif
#endif
