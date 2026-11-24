/*
 * chd_reader.h - CHD file reader callbacks for rcheevos hashing
 * Adapted from LoveRetro/NextUI (workspace/all/minarch/chd_reader.h).
 */

#ifndef CHD_READER_H
#define CHD_READER_H

#include <stddef.h>
#include <stdint.h>

void *chd_open_track(const char *path, uint32_t track);
void *chd_open_track_iterator(const char *path, uint32_t track, const void *iterator);
size_t chd_read_sector(void *track_handle, uint32_t sector, void *buffer, size_t requested_bytes);
void chd_close_track(void *track_handle);
uint32_t chd_first_track_sector(void *track_handle);
int chd_reader_is_chd(const char *path);

#endif /* CHD_READER_H */
