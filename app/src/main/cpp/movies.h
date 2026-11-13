/* Hardware-decodable copies of the game's VP9 movies, for devices whose only VP9 decoder is a software one (the
 * Galaxy A17 / Exynos 1330: c2.android.vp9.decoder manages ~21 of 30 fps next to the game).
 *
 * - movies_init() indexes the .webm files of romfs.bin (their byte ranges in the image) and, on such devices, starts a
 *   background converter: each movie is decoded once with Android's VP9 decoder straight into the hardware H.264
 *   encoder's input surface and written to <data>/movies/<name>.mp4 (same timestamps, keyframes where the source has
 *   them, no B-frames). <data>/no_movie_convert turns it off.
 * - the RomFS IStorage reports every read (movies_note_read), so the movie decoder knows which movie is playing.
 * - hooks_movie.c then plays the copy on the hardware decoder in place of the VP9 stream (movies_sidecar_recent).
 * Devices with a hardware VP9 decoder (the Adreno ones) use none of this. */
#pragma once
#include <stddef.h>
#include <stdint.h>

void movies_init(void);
void movies_note_read(uint64_t off, uint64_t size);

/* 1 when Android's VP9 decoder on this device is a software one (cached) */
int movies_vp9_is_software(void);

/* The movies the game read in the last 20 s that have a complete converted copy, most recent first (the game also reads
 * the headers of other movies around a playback, so the caller picks the one whose duration matches): up to max
 * entries of paths[k] / names[k]; returns the count. */
#define MOVIES_PATH_N 640
#define MOVIES_NAME_N 96
int movies_recent_copies(char paths[][MOVIES_PATH_N], char names[][MOVIES_NAME_N], int max);

/* a video packet reached the movie decoder: the converter pauses while one arrived in the last 2 s */
void movies_playback_tick(void);
