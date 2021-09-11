/*
 * file_export.c - low level interface for saving to various file formats
 *
 * Copyright (C) 1995-1998 David Firth
 * Copyright (C) 1998-2005 Atari800 development team (see DOC/CREDITS)
 *
 * This file is part of the Atari800 emulator project which emulates
 * the Atari 400, 800, 800XL, 130XE, and 5200 8-bit computers.
 *
 * Atari800 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Atari800 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Atari800; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <stdio.h>
#include <stdlib.h>
#include "file_export.h"
#include "video_codec_mrle.h"
#include "screen.h"
#include "colours.h"
#include "cfg.h"
#include "util.h"
#include "log.h"
#ifdef SOUND
#include "sound.h"
#include "pokeysnd.h"
#endif
#ifdef SUPPORTS_CHANGE_VIDEOMODE
#include "videomode.h"
#endif

#ifdef HAVE_LIBPNG
#include <png.h>
#ifdef VIDEO_CODEC_PNG
#include "video_codec_mpng.h"
#endif
#endif

#ifdef VIDEO_CODEC_ZMBV
#include "video_codec_zmbv.h"
#endif

#if defined(SOUND) || defined(AVI_VIDEO_RECORDING)
/* RIFF files (WAV, AVI) are limited to 4GB in size, so define a reasonable max
   that's lower than 4GB */
#define MAX_RECORDING_SIZE (0xfff00000)

/* number of bytes written to the currently open multimedia file */
static ULONG byteswritten;

/* These variables are needed for statistics and on-screen information display. */
static ULONG frames_written;
static float fps;
static char description[16];
#endif

#ifdef SOUND
/* sample size in bytes; will not change during a recording */
static int sample_size;
#endif

#if !defined(BASIC) && !defined(CURSES_BASIC)

/* image size will be determined in a call to set_video_margins() below */
static int video_left_margin;
static int video_top_margin;
static int video_width;
static int video_height;

#endif

#ifdef AVI_VIDEO_RECORDING

/* AVI requires the header at the beginning of the file contains sizes of each
   chunk, so the header will be rewritten upon the close of the file to update
   the header values with the final totals. *;
*/
static ULONG size_riff;
static ULONG size_movi;

/* AVI files using the version 1.0 indexes ('idx1') have a 32 bit limit, which
   limits file size to 4GB. Some media players may fail to play videos greater
   than 2GB because of their incorrect use of signed rather than unsigned 32 bit
   values.

   The maximum recording duration depends mostly on the complexity of the video.
   The audio is saved as raw PCM samples which doesn't vary per frame. On NTSC
   at 60Hz using 16 bit samples, this will be just under 1500 bytes per frame.

   The size of each encoded video frame depends on the complexity the screen
   image. The RLE compression is based on scan lines, and performs best when
   neighboring pixels on the scan line are the same color. Due to overhead in
   the compression sceme itself, the best it can do is about 1500 bytes on a
   completely black screen. Complex screens where many neighboring pixels have
   different colors result in video frames of around 30k. This is still a
   significant savings over an uncompressed frame which would be 80k.

   For complex scenes, therefore, this results in about 8 minutes of video
   recording per GB, or about 36 minutes when using the full 4GB file size. Less
   complex video will provide more recording time. For example, recording the
   unchanging BASIC prompt screen would result in about 6 hours of video.

   The video will automatically be stopped should the recording length approach
   the file size limit. */
static ULONG total_video_size;
static ULONG smallest_video_frame;
static ULONG largest_video_frame;

/* Video/audio frame data is stored in a packed ULONG array. Each video frame is
   limited to 0x3ffff bytes (256k) and each audio frame is limited to 0x1fff
   bytes (8k). The maximum audio sample rate is 65kHz (common are 44.1kHz and
   48kHz), so the largest possible audio frame size will be with stereo POKEYs
   at 65kHz with 16 bit samples at PAL frequency, so 2 * 65kHz * 2 / 50 frames
   per second = 5243 samples/frame. It fits. */
#define FRAME_INDEX_ALLOC_SIZE 1000
static int num_frames_allocated;
static ULONG *frame_indexes;
#define VIDEO_BITMASK    0x0003ffff
#define AUDIO_BITSHIFT   0x00040000
#define AUDIO_BITMASK    0x7ffc0000
#define KEYFRAME_BITMASK 0x80000000

/* dynamically allocated workspace for image compression */
static int video_buffer_size = 0;
static UBYTE *video_buffer = NULL;

/* current_screen_size is used as a flag before the frame is generated. -1 means
   awaiting frame generation, -2 means error, and 0 or greater means the video
   frame has been encoded. Note that some encoders can create inter-frames with
   zero length */
static int current_screen_size = -1;

static VIDEO_CODEC_t *video_codec = NULL;
static VIDEO_CODEC_t *requested_video_codec = NULL;
static VIDEO_CODEC_t *known_video_codecs[] = {
	&Video_Codec_MRLE,
#ifdef VIDEO_CODEC_PNG
	&Video_Codec_MPNG,
#endif
#ifdef VIDEO_CODEC_ZMBV
	&Video_Codec_ZMBV,
#endif
	NULL,
};

/* Some codecs allow for keyframes (full frame compression) and inter-frames
   (only the differences from the previous frame) */
static int keyframe_interval = 1000;
static float keyframe_residual;
static int current_is_keyframe = 0;

#ifdef SOUND
static ULONG samples_written;
static int audio_buffer_size = 0;
static UBYTE *audio_buffer = NULL;
static int current_audio_samples = -1;
#endif

static int num_streams;

#endif /* AVI_VIDEO_RECORDING */

#if defined(HAVE_LIBPNG) || defined(HAVE_LIBZ)
int FILE_EXPORT_compression_level = 6;
#endif


static VIDEO_CODEC_t *match_video_codec(char *id)
{
	VIDEO_CODEC_t **v = known_video_codecs;
	VIDEO_CODEC_t *found = NULL;

	while (*v) {
		if (Util_stricmp(id, (*v)->codec_id) == 0) {
			found = *v;
			break;
		}
		v++;
	}
	return found;
}

static VIDEO_CODEC_t *get_best_video_codec(void)
{
	/* ZMBV is the default if we also have zlib because compressed ZMBV is far
	   superior to the others. If zlib is not available, RLE becomes the default
	   because it's better than uncompressed ZMBV in most cases. PNG is never
	   the default. */
#if defined(VIDEO_CODEC_ZMBV) && defined(HAVE_LIBZ)
	return &Video_Codec_ZMBV;
#else
	return &Video_Codec_MRLE;
#endif
}

static char *video_codec_args(char *buf)
{
	VIDEO_CODEC_t **v = known_video_codecs;

	strcpy(buf, "\t-videocodec auto");
	while (*v) {
		strcat(buf, "|");
		strcat(buf, (*v)->codec_id);
		v++;
	}
	return buf;
}

int File_Export_Initialise(int *argc, char *argv[])
{

	int i;
	int j;

	for (i = j = 1; i < *argc; i++) {
		int i_a = (i + 1 < *argc); /* is argument available? */
		int a_m = FALSE; /* error, argument missing! */
		int a_i = FALSE; /* error, argument invalid! */

		if (0) {}
#ifdef AVI_VIDEO_RECORDING
		else if (strcmp(argv[i], "-videocodec") == 0) {
			if (i_a) {
				char *mode = argv[++i];
				if (strcmp(mode, "auto") == 0) {
					requested_video_codec = NULL; /* want best available */
				}
				else {
					requested_video_codec = match_video_codec(mode);
					if (!requested_video_codec) {
						a_i = TRUE;
					}
				}
			}
			else a_m = TRUE;
		}
		else if (strcmp(argv[i], "-keyframe-interval") == 0) {
			if (i_a) {
				keyframe_interval = Util_sscandec(argv[++i]);
				if (keyframe_interval < 1) {
					Log_print("Invalid keyframe interval time, must be 1 millisecond or greater.");
					return FALSE;
				}
			}
			else a_m = TRUE;
		}
#endif
#if defined(HAVE_LIBPNG) || defined(HAVE_LIBZ)
		else if (strcmp(argv[i], "-compression-level") == 0) {
			if (i_a) {
				FILE_EXPORT_compression_level = Util_sscandec(argv[++i]);
				if (FILE_EXPORT_compression_level < 0 || FILE_EXPORT_compression_level > 9) {
					Log_print("Invalid png/zlib compression level - must be between 0 and 9");
					return FALSE;
				}
			}
			else a_m = TRUE;
		}
#endif
		else {
			if (strcmp(argv[i], "-help") == 0) {
#ifdef AVI_VIDEO_RECORDING
				char buf[256];
				Log_print(video_codec_args(buf));
				Log_print("\t                 Select video codec (default: auto)");
				Log_print("\t-keyframe-interval <ms>");
				Log_print("\t                 Select interval between video keyframes in milliseconds");
#endif
#if defined(HAVE_LIBPNG) || defined(HAVE_LIBZ)
				Log_print("\t-compression-level <n>");
				Log_print("\t                 Set zlib/PNG compression level 0-9 (default 6)");
#endif
			}
			argv[j++] = argv[i];
		}

		if (a_m) {
			Log_print("Missing argument for '%s'", argv[i]);
			return FALSE;
		} else if (a_i) {
			Log_print("Invalid argument for '%s'", argv[--i]);
			return FALSE;
		}
	}
	*argc = j;

	return TRUE;
}

int File_Export_ReadConfig(char *string, char *ptr)
{
	if (0) {}
#ifdef AVI_VIDEO_RECORDING
	else if (strcmp(string, "VIDEO_CODEC") == 0) {
		if (Util_stricmp(ptr, "auto") == 0) {
			requested_video_codec = NULL; /* want best available */
		}
		else {
			requested_video_codec = match_video_codec(ptr);
			if (!requested_video_codec) {
				return FALSE;
			}
		}
	}
	else if (strcmp(string, "VIDEO_CODEC_KEYFRAME_INTERVAL") == 0) {
		int num = Util_sscandec(ptr);
		if (num > 0)
			keyframe_interval = num;
		else return FALSE;
	}
#endif
#if defined(HAVE_LIBPNG) || defined(HAVE_LIBZ)
	else if (strcmp(string, "COMPRESSION_LEVEL") == 0) {
		int num = Util_sscandec(ptr);
		if (num >= 0 && num <= 9)
			FILE_EXPORT_compression_level = num;
		else return FALSE;
	}
#endif
	else return FALSE;
	return TRUE;
}

void File_Export_WriteConfig(FILE *fp)
{
#ifdef AVI_VIDEO_RECORDING
	if (!requested_video_codec) {
		fprintf(fp, "VIDEO_CODEC=AUTO\n");
	}
	else {
		fprintf(fp, "VIDEO_CODEC=%s\n", requested_video_codec->codec_id);
	}
	fprintf(fp, "VIDEO_CODEC_KEYFRAME_INTERVAL=%d\n", keyframe_interval);
#endif
#if defined(HAVE_LIBPNG) || defined(HAVE_LIBZ)
	fprintf(fp, "COMPRESSION_LEVEL=%d\n", FILE_EXPORT_compression_level);
#endif
}

#if defined(SOUND) || defined(AVI_VIDEO_RECORDING)
/* File_Export_ElapsedTime returns the current duration of the multimedia file.
   */
int File_Export_ElapsedTime(void)
{
	return (int)(frames_written / fps);
}

/* File_Export_CurrentSize returns the current size of the multimedia file in
   bytes. This should be considered approximate and not used in calculations
   related to the actual position in the written file.
   */
int File_Export_CurrentSize(void)
{
	return byteswritten;
}

/* File_Export_CurrentSize returns the current size of the multimedia file in
   bytes. This should be considered approximate and not used in calculations
   related to the actual position in the written file.
   */
char *File_Export_Description(void)
{
	return description;
}
#endif

/* fputw, fputl, and fwritele are utility functions to write values as
   little-endian format regardless of the endianness of the platform. */

/* write 16-bit word as little endian */
void fputw(UWORD x, FILE *fp)
{
	fputc(x & 0xff, fp);
	fputc((x >> 8) & 0xff, fp);
}

/* write 32-bit long as little endian */
void fputl(ULONG x, FILE *fp)
{
	fputc(x & 0xff, fp);
	fputc((x >> 8) & 0xff, fp);
	fputc((x >> 16) & 0xff, fp);
	fputc((x >> 24) & 0xff, fp);
}

/* fwritele mimics fwrite but writes data in little endian format if operating
   on a big endian platform.

   On a little endian platform, this function calls fwrite with no alteration to
   the data.

   On big endian platforms, the only valid size parameters are 1 and 2; size of
   1 indicates byte data, which has no endianness and will be written unaltered.
   If the size parameter is 2 (indicating WORD or UWORD 16-bit data), this
   function will reverse bytes in the file output.

   Note that size parameters greater than 2 (e.g. 4 which would indicate LONG &
   ULONG data) are not currently supported on big endian platforms because
   nothing currently use arrays of that size.

   RETURNS: number of elements written, or zero if error on big endial platforms */
size_t fwritele(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
#ifdef WORDS_BIGENDIAN
	size_t count;
	UBYTE *source;
	UBYTE c;

	if (size == 2) {
		/* fputc doesn't return useful error info as a return value, so simulate
		   the fwrite error condition by checking ferror before and after, and
		   if no error, return the number of elements written. */
		if (ferror(fp)) {
			return 0;
		}
		source = (UBYTE *)ptr;

		/* Instead of using this simple loop over fputc, a faster algorithm
		   could be written by copying the data into a temporary array, swapping
		   bytes there, and using fwrite. However, fputc may be cached behind
		   the scenes and this is likely to be fast enough for most platforms. */
		for (count = 0; count < nmemb; count++) {
			c = *source++;
			fputc(*source++, fp);
			fputc(c, fp);
		}
		if (ferror(fp)) {
			return 0;
		}
		return count;
	}
#endif
	return fwrite(ptr, size, nmemb, fp);
}

#if !defined(BASIC) && !defined(CURSES_BASIC)

static void set_video_margins(void)
{
#ifdef SUPPORTS_CHANGE_VIDEOMODE
	video_left_margin = VIDEOMODE_src_offset_left;
	video_width = VIDEOMODE_src_offset_left + VIDEOMODE_src_width - video_left_margin;
#else
	video_left_margin = Screen_visible_x1;
	video_width = Screen_visible_x2 - video_left_margin;
#endif
	video_top_margin = Screen_visible_y1;
	video_height = Screen_visible_y2 - video_top_margin;
}

/* PCX_SaveScreen saves the screen data to the file in PCX format, optionally
   using interlace if ptr2 is not NULL.

   PCX format is a lossless image file format derived from PC Paintbrush, a
   DOS-era paint program, and is widely supported by image viewers. The
   compression method is run-length encoding, which is simple to implement but
   only compresses well when groups of neighboring pixels on a scan line have
   the same color.

   The PNG format (see PNG_SaveScreen below) compresses much better, but depends
   on the external libpng library. No external dependencies are needed for PCX
   format.

   fp:          file pointer of file open for writing
   ptr1:        pointer to Screen_atari
   ptr2:        (optional) pointer to another array of size Screen_atari containing
                the interlaced scan lines to blend with ptr1. Set to NULL if no
				interlacing.
*/
void PCX_SaveScreen(FILE *fp, UBYTE *ptr1, UBYTE *ptr2)
{
	int i;
	int x;
	int y;
	UBYTE plane = 16;	/* 16 = Red, 8 = Green, 0 = Blue */
	UBYTE last;
	UBYTE count;

	set_video_margins();

	fputc(0xa, fp);   /* pcx signature */
	fputc(0x5, fp);   /* version 5 */
	fputc(0x1, fp);   /* RLE encoding */
	fputc(0x8, fp);   /* bits per pixel */
	fputw(0, fp);     /* XMin */
	fputw(0, fp);     /* YMin */
	fputw(video_width - 1, fp); /* XMax */
	fputw(video_height - 1, fp);        /* YMax */
	fputw(0, fp);     /* HRes */
	fputw(0, fp);     /* VRes */
	for (i = 0; i < 48; i++)
		fputc(0, fp); /* EGA color palette */
	fputc(0, fp);     /* reserved */
	fputc(ptr2 != NULL ? 3 : 1, fp); /* number of bit planes */
	fputw(video_width, fp);  /* number of bytes per scan line per color plane */
	fputw(1, fp);     /* palette info */
	fputw(video_width, fp); /* screen resolution */
	fputw(video_height, fp);
	for (i = 0; i < 54; i++)
		fputc(0, fp);  /* unused */

	ptr1 += (Screen_WIDTH * video_top_margin) + video_left_margin;
	if (ptr2 != NULL) {
		ptr2 += (Screen_WIDTH * video_top_margin) + video_left_margin;
	}
	for (y = 0; y < video_height; ) {
		x = 0;
		do {
			last = ptr2 != NULL ? (((Colours_table[*ptr1] >> plane) & 0xff) + ((Colours_table[*ptr2] >> plane) & 0xff)) >> 1 : *ptr1;
			count = 0xc0;
			do {
				ptr1++;
				if (ptr2 != NULL)
					ptr2++;
				count++;
				x++;
			} while (last == (ptr2 != NULL ? (((Colours_table[*ptr1] >> plane) & 0xff) + ((Colours_table[*ptr2] >> plane) & 0xff)) >> 1 : *ptr1)
						&& count < 0xff && x < video_width);
			if (count > 0xc1 || last >= 0xc0)
				fputc(count, fp);
			fputc(last, fp);
		} while (x < video_width);

		if (ptr2 != NULL && plane) {
			ptr1 -= video_width;
			ptr2 -= video_width;
			plane -= 8;
		}
		else {
			ptr1 += Screen_WIDTH - video_width;
			if (ptr2 != NULL) {
				ptr2 += Screen_WIDTH - video_width;
				plane = 16;
			}
			y++;
		}
	}

	if (ptr2 == NULL) {
		/* write palette */
		fputc(0xc, fp);
		for (i = 0; i < 256; i++) {
			fputc(Colours_GetR(i), fp);
			fputc(Colours_GetG(i), fp);
			fputc(Colours_GetB(i), fp);
		}
	}
}

#ifdef HAVE_LIBPNG
#ifdef VIDEO_CODEC_PNG
static void PNG_SaveToBuffer(png_structp png_ptr, png_bytep data, png_size_t length)
{
	if (current_screen_size >= 0) {
		if (current_screen_size + length < video_buffer_size) {
			memcpy(video_buffer + current_screen_size, data, length);
			current_screen_size += length;
		}
		else {
			Log_print("AVI write error: video compression buffer size too small.");
			current_screen_size = -2;
		}
	}
}
#endif /* VIDEO_CODEC_PNG */

/* PNG_SaveScreen saves the screen data to the file in PNG format, optionally
   using interlace if ptr2 is not NULL.

   PNG format is a lossless image file format that compresses much better than
   PCX. Because it depends on the external libpng library, it is only compiled
   in atari800 if requested and libpng is found on the system.

   fp:          file pointer of file open for writing
   ptr1:        pointer to Screen_atari
   ptr2:        (optional) pointer to another array of size Screen_atari containing
                the interlaced scan lines to blend with ptr1. Set to NULL if no
				interlacing.
*/
int PNG_SaveScreen(FILE *fp, UBYTE *ptr1, UBYTE *ptr2)
{
	png_structp png_ptr;
	png_infop info_ptr;
	png_bytep rows[Screen_HEIGHT];

	png_ptr = png_create_write_struct(
		PNG_LIBPNG_VER_STRING,
		NULL, NULL, NULL
	);
	if (png_ptr == NULL)
		return 0;
	info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == NULL) {
		png_destroy_write_struct(&png_ptr, NULL);
		return 0;
	}
#ifdef VIDEO_CODEC_PNG
	if (fp == NULL) {
		current_screen_size = 0;
		png_set_write_fn(png_ptr, NULL, PNG_SaveToBuffer, NULL);
	}
	else
#endif
	{
		set_video_margins();
		png_init_io(png_ptr, fp);
	}

	png_set_compression_level(png_ptr, FILE_EXPORT_compression_level);
	png_set_IHDR(
		png_ptr, info_ptr, video_width, video_height,
		8, ptr2 == NULL ? PNG_COLOR_TYPE_PALETTE : PNG_COLOR_TYPE_RGB,
		PNG_INTERLACE_NONE,
		PNG_COMPRESSION_TYPE_DEFAULT,
		PNG_FILTER_TYPE_DEFAULT
	);
	if (ptr2 == NULL) {
		int i;
		png_color palette[256];
		for (i = 0; i < 256; i++) {
			palette[i].red = Colours_GetR(i);
			palette[i].green = Colours_GetG(i);
			palette[i].blue = Colours_GetB(i);
		}
		png_set_PLTE(png_ptr, info_ptr, palette, 256);
		ptr1 += (Screen_WIDTH * video_top_margin) + video_left_margin;
		for (i = 0; i < video_height; i++) {
			rows[i] = ptr1;
			ptr1 += Screen_WIDTH;
		}
	}
	else {
		png_bytep ptr3;
		int x;
		int y;
		ptr1 += (Screen_WIDTH * video_top_margin) + video_left_margin;
		ptr2 += (Screen_WIDTH * video_top_margin) + video_left_margin;
		ptr3 = (png_bytep) Util_malloc(3 * video_width * video_height);
		for (y = 0; y < video_height; y++) {
			rows[y] = ptr3;
			for (x = 0; x < video_width; x++) {
				*ptr3++ = (png_byte) ((Colours_GetR(*ptr1) + Colours_GetR(*ptr2)) >> 1);
				*ptr3++ = (png_byte) ((Colours_GetG(*ptr1) + Colours_GetG(*ptr2)) >> 1);
				*ptr3++ = (png_byte) ((Colours_GetB(*ptr1) + Colours_GetB(*ptr2)) >> 1);
				ptr1++;
				ptr2++;
			}
			ptr1 += Screen_WIDTH - video_width;
			ptr2 += Screen_WIDTH - video_width;
		}
	}
	png_set_rows(png_ptr, info_ptr, rows);
	png_write_png(png_ptr, info_ptr, PNG_TRANSFORM_IDENTITY, NULL);
	png_destroy_write_struct(&png_ptr, &info_ptr);
	if (ptr2 != NULL)
		free(rows[0]);

#ifdef VIDEO_CODEC_PNG
	return current_screen_size;
#else
	return 0;
#endif
}
#endif /* HAVE_LIBPNG */
#endif /* !defined(BASIC) && !defined(CURSES_BASIC) */


#ifdef SOUND
/* WAV_OpenFile will start a new sound file and write out the header. Note that
   the file will not be valid until the it is closed with WAV_CloseFile because
   the length information contained in the header must be updated with the
   number of samples in the file.

   RETURNS: TRUE if file opened with no problems, FALSE if failure during open
   */
FILE *WAV_OpenFile(const char *szFileName)
{
	FILE *fp;

	if (!(fp = fopen(szFileName, "wb")))
		return NULL;
	/*
	The RIFF header:

	  Offset  Length   Contents
	  0       4 bytes  'RIFF'
	  4       4 bytes  <file length - 8>
	  8       4 bytes  'WAVE'

	The fmt chunk:

	  12      4 bytes  'fmt '
	  16      4 bytes  0x00000010     // Length of the fmt data (16 bytes)
	  20      2 bytes  0x0001         // Format tag: 1 = PCM
	  22      2 bytes  <channels>     // Channels: 1 = mono, 2 = stereo
	  24      4 bytes  <sample rate>  // Samples per second: e.g., 44100
	  28      4 bytes  <bytes/second> // sample rate * block align
	  32      2 bytes  <block align>  // channels * bits/sample / 8
	  34      2 bytes  <bits/sample>  // 8 or 16

	The data chunk:

	  36      4 bytes  'data'
	  40      4 bytes  <length of the data block>
	  44        bytes  <sample data>

	All chunks must be word-aligned.

	Good description of WAVE format: http://www.sonicspot.com/guide/wavefiles.html
	*/
	sample_size = POKEYSND_snd_flags & POKEYSND_BIT16? 2 : 1;
	frames_written = 0;
	fps = Atari800_tv_mode == Atari800_TV_PAL ? Atari800_FPS_PAL : Atari800_FPS_NTSC;
	strcpy(description, "WAV");

	fputs("RIFF", fp);
	fputl(0, fp); /* length to be filled in upon file close */
	fputs("WAVE", fp);

	fputs("fmt ", fp);
	fputl(16, fp);
	fputw(1, fp);
	fputw(POKEYSND_num_pokeys, fp);
	fputl(POKEYSND_playback_freq, fp);
	fputl(POKEYSND_playback_freq * sample_size, fp);
	fputw(POKEYSND_num_pokeys * sample_size, fp);
	fputw(sample_size * 8, fp);

	fputs("data", fp);
	fputl(0, fp); /* length to be filled in upon file close */

	if (ftell(fp) != 44) {
		fclose(fp);
		return NULL;
	}

	byteswritten = 0;
	return fp;
}

/* WAV_WriteSamples will dump PCM data to the WAV file. The best way
   to do this for Atari800 is probably to call it directly after
   POKEYSND_Process(buffer, size) with the same values (buffer, size)

   RETURNS: the number of bytes written to the file (should be equivalent to the
   input num_samples * sample size) */
int WAV_WriteSamples(const unsigned char *buf, unsigned int num_samples, FILE *fp)
{
	if (fp && buf && num_samples) {
		int result;

		result = fwritele(buf, sample_size, num_samples, fp);
		if (result != num_samples) {
			result = 0;
		}
		else {
			result *= sample_size;
		}

		byteswritten += result;
		frames_written++;
		if (byteswritten > MAX_RECORDING_SIZE) {
			return 0;
		}
		return result;
	}

	return 0;
}


/* WAV_CloseFile must be called to create a valid WAV file, because the header
   at the beginning of the file must be modified to indicate the number of
   samples in the file.

   RETURNS: TRUE if file closed with no problems, FALSE if failure during close
   */
int WAV_CloseFile(FILE *fp)
{
	int bSuccess = TRUE;
	char aligned = 0;

	if (fp != NULL) {
		/* A RIFF file's chunks must be word-aligned. So let's align. */
		if (byteswritten & 1) {
			if (putc(0, fp) == EOF)
				bSuccess = FALSE;
			else
				aligned = 1;
		}

		if (bSuccess) {
			/* Sound file is finished, so modify header and close it. */
			if (fseek(fp, 4, SEEK_SET) != 0)	/* Seek past RIFF */
				bSuccess = FALSE;
			else {
				/* RIFF header's size field must equal the size of all chunks
				 * with alignment, so the alignment byte is added.
				 */
				fputl(byteswritten + 36 + aligned, fp);
				if (fseek(fp, 40, SEEK_SET) != 0)
					bSuccess = FALSE;
				else {
					/* But in the "data" chunk size field, the alignment byte
					 * should be ignored. */
					fputl(byteswritten, fp);
				}
			}
		}
		fclose(fp);
	}

	return bSuccess;
}
#endif /* SOUND */

#ifdef AVI_VIDEO_RECORDING

/* AVI_WriteHeader creates and writes out the file header. Note that this
   function will have to be called again just prior to closing the file in order
   to re-write the header with updated size values that are only known after all
   data has been written.

   RETURNS: TRUE if header was written successfully, FALSE if not
   */
static int AVI_WriteHeader(FILE *fp) {
	int i;
	int list_size;

	fseek(fp, 0, SEEK_SET);

	/* RIFF AVI header */
	fputs("RIFF", fp);
	fputl(size_riff, fp); /* length of entire file minus 8 bytes */
	fputs("AVI ", fp);

	/* hdrl LIST. Payload size includes the 4 bytes of the 'hdrl' identifier. */
	fputs("LIST", fp);

	/* total header size includes hdrl identifier plus avih size PLUS the video stream
	   header which is (strl header LIST + (strh + strf + strn)) */
	list_size = 4 + 8 + 56 + (12 + (8 + 56 + 8 + 40 + 256*4 + 8 + 16));

	/* if audio is included, add size of audio stream strl header LIST + (strh + strf + strn) */
	if (num_streams == 2) list_size += 12 + (8 + 56 + 8 + 18 + 8 + 12);

	fputl(list_size, fp); /* length of header payload */
	fputs("hdrl", fp);

	/* Main header is documented at https://docs.microsoft.com/en-us/previous-versions/windows/desktop/api/Aviriff/ns-aviriff-avimainheader */

	/* 8 bytes */
	fputs("avih", fp);
	fputl(56, fp); /* length of avih payload: 14 x 4 byte words */

	/* 56 bytes */
	fputl((ULONG)(1000000 / fps), fp); /* microseconds per frame */
	fputl(video_width * video_height * 3, fp); /* approximate bytes per second of video + audio FIXME: should likely be (width * height * 3 + audio) * fps */
	fputl(0, fp); /* reserved */
	fputl(0x10, fp); /* flags; 0x10 indicates the index at the end of the file */
	fputl(frames_written, fp); /* number of frames in the video */
	fputl(0, fp); /* initial frames, always zero for us */
	fputl(num_streams, fp); /* 2 = video and audio, 1 = video only */
	fputl(video_width * video_height * 3, fp); /* suggested buffer size */
	fputl(video_width, fp); /* video width */
	fputl(video_height, fp); /* video height */
	fputl(0, fp); /* reserved */
	fputl(0, fp);
	fputl(0, fp);
	fputl(0, fp);

	/* video stream format */

	/* 12 bytes for video stream strl LIST chuck header; LIST payload size includes the
	   4 bytes of the 'strl' identifier plus the strh + strf + strn sizes */
	fputs("LIST", fp);
	fputl(4 + 8 + 56 + 8 + 40 + 256*4 + 8 + 16, fp);
	fputs("strl", fp);

	/* Stream header format is document at https://docs.microsoft.com/en-us/previous-versions/windows/desktop/api/avifmt/ns-avifmt-avistreamheader */

	/* 8 bytes for stream header indicator */
	fputs("strh", fp);
	fputl(56, fp); /* length of strh payload: 14 x 4 byte words */

	/* 56 bytes for stream header data */
	fputs("vids", fp); /* video stream */
	fwrite(video_codec->fourcc, 4, 1, fp);
	fputl(0, fp); /* flags */
	fputw(0, fp); /* priority */
	fputw(0, fp); /* language */
	fputl(0, fp); /* initial_frames */
	fputl(1000000, fp); /* scale */
	fputl((ULONG)(fps * 1000000), fp); /* rate = frames per second / scale */
	fputl(0, fp); /* start */
	fputl(frames_written, fp); /* length (for video is number of frames) */
	fputl(video_width * video_height * 3, fp); /* suggested buffer size */
	fputl(0, fp); /* quality */
	fputl(0, fp); /* sample size (0 = variable sample size) */
	fputl(0, fp); /* rcRect, ignored */
	fputl(0, fp);

	/* 8 bytes for stream format indicator */
	fputs("strf", fp);
	fputl(40 + 256*4, fp); /* length of header + palette info */

	/* 40 bytes for stream format data */
	fputl(40, fp); /* header_size */
	fputl(video_width, fp); /* width */
	fputl(video_height, fp); /* height */
	fputw(1, fp); /* number of bitplanes */
	fputw(8, fp); /* bits per pixel: 8 = paletted */
	fwrite(video_codec->avi_compression, 4, 1, fp);
	fputl(video_width * video_height * 3, fp); /* image_size */
	fputl(0, fp); /* x pixels per meter (!) */
	fputl(0, fp); /* y pikels per meter */
	fputl(256, fp); /* colors_used */
	fputl(0, fp); /* colors_important (0 = all are important) */

	/* 256 * 4 = 1024 bytes of palette in ARGB little-endian order */
	for (i = 0; i < 256; i++) {
		fputc(Colours_GetB(i), fp);
		fputc(Colours_GetG(i), fp);
		fputc(Colours_GetR(i), fp);
		fputc(0, fp);
	}

	/* 8 bytes for stream name indicator */
	fputs("strn", fp);
	fputl(16, fp); /* length of name */

	/* 16 bytes for name, zero terminated and padded with a zero */

	/* Note: everything in RIFF files must be word-aligned, so padding with a
	   zero is necessary if the length of the name plus the null terminator is
	   an odd value */
	fputs("atari800 video", fp);
	fputc(0, fp); /* null terminator */
	fputc(0, fp); /* padding to get to 16 bytes */

#ifdef SOUND
	if (num_streams == 2) {
		/* audio stream format */

		/* 12 bytes for audio stream strl LIST chuck header; LIST payload size includes the
		4 bytes of the 'strl' identifier plus the strh + strf + strn sizes */
		fputs("LIST", fp);
		fputl(4 + 8 + 56 + 8 + 18 + 8 + 12, fp);
		fputs("strl", fp);

		/* stream header format is same as video above even when used for audio */

		/* 8 bytes for stream header indicator */
		fputs("strh", fp);
		fputl(56, fp); /* length of strh payload: 14 x 4 byte words */

		/* 56 bytes for stream header data */
		fputs("auds", fp); /* video stream */
		fputl(1, fp); /* 1 = uncompressed audio */
		fputl(0, fp); /* flags */
		fputw(0, fp); /* priority */
		fputw(0, fp); /* language */
		fputl(0, fp); /* initial_frames */
		fputl(1, fp); /* scale */
		fputl(POKEYSND_playback_freq, fp); /* rate, i.e. samples per second */
		fputl(0, fp); /* start time; zero = no delay */
		fputl(samples_written, fp); /* length (for audio is number of samples) */
		fputl(POKEYSND_playback_freq * POKEYSND_num_pokeys * sample_size, fp); /* suggested buffer size */
		fputl(0, fp); /* quality (-1 = default quality?) */
		fputl(POKEYSND_num_pokeys * sample_size, fp); /* sample size */
		fputl(0, fp); /* rcRect, ignored */
		fputl(0, fp);

		/* 8 bytes for stream format indicator */
		fputs("strf", fp);
		fputl(18, fp); /* length of header */

		/* 18 bytes for stream format data */
		fputw(1, fp); /* format_type */
		fputw(POKEYSND_num_pokeys, fp); /* channels */
		fputl(POKEYSND_playback_freq, fp); /* sample_rate */
		fputl(POKEYSND_playback_freq * POKEYSND_num_pokeys * sample_size, fp); /* bytes_per_second */
		fputw(POKEYSND_num_pokeys * sample_size, fp); /* bytes per frame */
		fputw(sample_size * 8, fp); /* bits_per_sample */
		fputw(0, fp); /* size */

		/* 8 bytes for stream name indicator */
		fputs("strn", fp);
		fputl(12, fp); /* length of name */

		/* 12 bytes for name, zero terminated */
		fputs("POKEY audio", fp);
		fputc(0, fp); /* null terminator */
	}
#endif /* SOUND */

	/* audia/video data */

	/* 8 bytes for audio/video stream LIST chuck header; LIST payload is the
	  'movi' chunk which in turn contains 00dc and 01wb chunks representing a
	  frame of video and the corresponding audio. */
	fputs("LIST", fp);
	fputl(size_movi, fp); /* length of all video and audio chunks */
	size_movi = ftell(fp); /* start of movi payload, will finalize after all chunks written */
	fputs("movi", fp);

	return (ftell(fp) == 12 + 8 + list_size + 12);
}

/* AVI_OpenFile will start a new video file and write out an initial copy of the
   header. Note that the file will not be valid until the it is closed with
   AVI_CloseFile because the length information contained in the header must be
   updated with the number of samples in the file.

   RETURNS: file pointer if successful, NULL if failure during open
   */
FILE *AVI_OpenFile(const char *szFileName)
{
	FILE *fp;

	if (!(fp = fopen(szFileName, "wb")))
		return NULL;

	size_riff = 0;
	size_movi = 0;
	frames_written = 0;
	keyframe_residual = 0.0;
	current_is_keyframe = 1; /* first frame always a keyframe */
	current_screen_size = -1; /* screen not generated yet */

	fps = Atari800_tv_mode == Atari800_TV_PAL ? Atari800_FPS_PAL : Atari800_FPS_NTSC;
	set_video_margins();

	num_frames_allocated = FRAME_INDEX_ALLOC_SIZE;
	frame_indexes = (ULONG *)Util_malloc(num_frames_allocated * sizeof(ULONG));
	memset(frame_indexes, 0, num_frames_allocated * sizeof(ULONG));

	if (!video_codec) {
		if (!requested_video_codec) {
			video_codec = get_best_video_codec();
		}
		else {
			video_codec = requested_video_codec;
		}
	}
	strcpy(description, "AVI ");
	strcat(description, video_codec->codec_id);

	video_buffer_size = video_codec->init(video_width, video_height, video_left_margin, video_top_margin);
	if (video_buffer_size < 0) {
		Log_print("Failed to initialize video codec");
		fclose(fp);
		return NULL;
	}
	video_buffer = (UBYTE *)Util_malloc(video_buffer_size);
#ifdef SOUND
	current_audio_samples = -1;
	samples_written = 0;

	if (Sound_enabled) {
		num_streams = 2;
		sample_size = POKEYSND_snd_flags & POKEYSND_BIT16? 2 : 1;
		audio_buffer_size = (int)(POKEYSND_playback_freq * POKEYSND_num_pokeys * sample_size / fps) + 1024;
		audio_buffer = (UBYTE *)Util_malloc(audio_buffer_size);
	}
	else {
		num_streams = 1;
		sample_size = 0;
		audio_buffer_size = 0;
		audio_buffer = NULL;
	}
#else
	num_streams = 1;
#endif

	if (!AVI_WriteHeader(fp)) {
		video_codec->end();
		fclose(fp);
		return NULL;
	}

	/* set up video statistics */
	byteswritten = ftell(fp) + 8; /* current size + index header */
	total_video_size = 0;
	smallest_video_frame = 0xffffffff;
	largest_video_frame = 0;

	return fp;
}

/* AVI_WriteFrame writes out a single frame of video and audio, and saves the
   index data for the end-of-file index chunk */
static int AVI_WriteFrame(FILE *fp) {
	int video_padding;
	int frame_size;
	int expected_frame_size;
	int result;
#ifdef SOUND
	int audio_size;
	int audio_padding;
#endif

	frame_size = ftell(fp);

	/* AVI chunks must be word-aligned, i.e. lengths must be multiples of 2 bytes.
	   If the size is an odd number, the data is padded with a zero but the length
	   value still reports the actual length, not the padded length */
	video_padding = current_screen_size % 2;
	fputs("00dc", fp);
	fputl(current_screen_size, fp);
	fwrite(video_buffer, 1, current_screen_size, fp);
	if (video_padding) {
		fputc(0, fp);
	}
	expected_frame_size = 8 + current_screen_size + video_padding;

#ifdef SOUND
	if (num_streams == 2) {
		audio_size = current_audio_samples * sample_size;
		audio_padding = audio_size % 2;
		fputs("01wb", fp);
		fputl(audio_size, fp);
		fwritele(audio_buffer, sample_size, current_audio_samples, fp);
		if (audio_padding) {
			fputc(0, fp);
		}
		samples_written += current_audio_samples;
		expected_frame_size += 8 + audio_size + audio_padding;
	}
	else {
		audio_size = 0;
	}
#endif

	frame_indexes[frames_written] = current_screen_size
#ifdef SOUND
		+ AUDIO_BITSHIFT * audio_size
#endif
		;
	if (current_is_keyframe) {
		frame_indexes[frames_written] |= KEYFRAME_BITMASK;
	}
	frames_written++;
	if (frames_written >= num_frames_allocated) {
		num_frames_allocated += FRAME_INDEX_ALLOC_SIZE;
		frame_indexes = (ULONG *)Util_realloc(frame_indexes, num_frames_allocated * sizeof(ULONG));
	}

	/* check expected file data written equals the calculated size */
	frame_size = ftell(fp) - frame_size;
	result = (frame_size == expected_frame_size);

	/* update size limit calculation including the 32 bytes needed for each index entry */
	byteswritten += frame_size + 32;

	/* update statistics */
	total_video_size += current_screen_size;
	if (current_screen_size < smallest_video_frame) {
		smallest_video_frame = current_screen_size;
	}
	if (current_screen_size > largest_video_frame) {
		largest_video_frame = current_screen_size;
	}

	/* A keyframe is requested when the interval has passed, unless Motion-PNG
	   is used. Motion-PNG doesn't have the concept of deltas from the previous
	   frame; instead, everything is a keyframe. */
	if (video_codec->uses_interframes) {
		keyframe_residual += 1000.0 / fps;
		if (keyframe_residual > keyframe_interval) {
			current_is_keyframe = TRUE;
			keyframe_residual = keyframe_residual - ((int)(keyframe_residual / keyframe_interval) * keyframe_interval);
		}
		else {
			current_is_keyframe = FALSE;
		}
	}
	else {
		current_is_keyframe = TRUE;
	}

	/* reset size indicators for next frame */
	current_screen_size = -1;
#ifdef SOUND
	current_audio_samples = -1;
#endif

	if (byteswritten > MAX_RECORDING_SIZE) {
		/* force file close when at the limit */
		return 0;
	}

	return result;
}

/* AVI_AddVideoFrame adds a video frame to the stream. If an existing video
   frame & audio data exist, save it to the file before starting a new frame.
   Note that AVI_AddVideoFrame and AVI_AddAudioSamples may be called in either
   order, but you must call both video and audio functions before the same
   function again. */
int AVI_AddVideoFrame(FILE *fp) {
	if (current_screen_size >= 0) {
#ifdef SOUND
		if (num_streams == 1 || current_audio_samples > 0) {
#endif
			if (!AVI_WriteFrame(fp)) {
				return 0;
			}
#ifdef SOUND
		}
		else {
			Log_print("AVI write error: attempted to write video frame without audio data");
			return 0;
		}
#endif
	}
	else if (current_screen_size < -1
#ifdef SOUND
			 || current_audio_samples < -1
#endif
			) {
		/* error condition; force close of file */
		return 0;
	}

	current_screen_size = video_codec->frame((UBYTE *)Screen_atari, current_is_keyframe, video_buffer, video_buffer_size);
	return current_screen_size >= 0;
}

#ifdef SOUND
/* AVI_AddAudioSamples adds audio data to the stream for the current video
   frame. If an existing video frame & audio data exist, save it to the file
   before starting a new frame. Note that AVI_AddVideoFrame and
   AVI_AddAudioSamples may be called in either order, but you must call both
   video and audio functions before the same function again. */
int AVI_AddAudioSamples(const UBYTE *buf, int num_samples, FILE *fp) {
	int size;

	if (current_audio_samples >= 0) {
		if (current_screen_size >= 0) {
			if (!AVI_WriteFrame(fp)) {
				return 0;
			}
		}
		else {
			Log_print("AVI write error: attempted to write audio data without video frame");
			return 0;
		}
	}
	else if (current_screen_size < -1 || current_audio_samples < -1) {
		/* error condition; force close of file */
		return 0;
	}

	size = num_samples * sample_size;
	if (size > audio_buffer_size) {
		Log_print("AVI write error: audio buffer size too small to hold %d samples", num_samples);
		/* set error condition */
		current_audio_samples = -2;
		return 0;
	}
	current_audio_samples = num_samples;
	memcpy(audio_buffer, buf, size);

	return 1;
}
#endif

static int AVI_WriteIndex(FILE *fp) {
	int i;
	int offset;
	int size;
	int index_size;
	int chunk_size;
	ULONG index;
	int is_keyframe;

	if (frames_written == 0) return 0;

	chunk_size = ftell(fp);
	offset = 4;
	index_size = frames_written * 16
#ifdef SOUND
		* 2
#endif
		;

	/* The index format used here is tag 'idx1" (index version 1.0) & documented at
	https://docs.microsoft.com/en-us/previous-versions/windows/desktop/api/Aviriff/ns-aviriff-avioldindex
	*/

	fputs("idx1", fp);
	fputl(index_size, fp);

	for (i = 0; i < frames_written; i++) {
		index = frame_indexes[i];
		is_keyframe = index & KEYFRAME_BITMASK ? 0x10 : 0;

		fputs("00dc", fp); /* stream 0, a compressed video frame */
		fputl(is_keyframe, fp); /* flags: is a keyframe */
		fputl(offset, fp); /* offset in bytes from start of the 'movi' list */
		size = index & VIDEO_BITMASK;
		fputl(size, fp); /* size of video frame */
		offset += size + 8 + (size % 2); /* make sure to word-align next offset */

#ifdef SOUND
		fputs("01wb", fp); /* stream 1, audio data */
		fputl(0x10, fp); /* flags: PCM audio is always a keyframe */
		fputl(offset, fp); /* offset in bytes from start of the 'movi' list */
		size = (index & AUDIO_BITMASK) / AUDIO_BITSHIFT;
		fputl(size, fp); /* size of audio data */
		offset += size + 8 + (size % 2); /* make sure to word-align next offset */
#endif
	}

	chunk_size = ftell(fp) - chunk_size;
	return (chunk_size == 8 + index_size);
}

/* AVI_CloseFile must be called to create a valid AVI file, because the header
   at the beginning of the file must be modified to indicate the number of
   samples in the file.

   RETURNS: TRUE if file closed with no problems, FALSE if failure during close
   */
int AVI_CloseFile(FILE *fp)
{
	int seconds;
	int result;

	/* write out final frame if one exists */
	if (current_screen_size >= 0
#ifdef SOUND
		&& current_audio_samples >= 0
#endif
			) {
		result = AVI_WriteFrame(fp);
	}
	else {
		result = 1;
	}

	if (frames_written > 0) {
		seconds = (int)(frames_written / fps);
		Log_print("AVI stats: %d:%02d:%02d, %dMB, %d frames; video codec avg frame size %.1fkB, min=%.1fkB, max=%.1fkB", seconds / 60 / 60, (seconds / 60) % 60, seconds % 60, byteswritten / 1024 / 1024, frames_written, total_video_size / frames_written / 1024.0, smallest_video_frame / 1024.0, largest_video_frame / 1024.0);
	}

	if (result > 0) {
		size_movi = ftell(fp) - size_movi; /* movi payload ends here */
		result = AVI_WriteIndex(fp);
	}
	if (result > 0) {
		size_riff = ftell(fp) - 8;
		result = AVI_WriteHeader(fp);
	}
	fclose(fp);
#ifdef SOUND
	if (audio_buffer_size > 0) {
		free(audio_buffer);
		audio_buffer = NULL;
	}
	audio_buffer_size = 0;
	current_audio_samples = -1;
#endif
	video_codec->end();
	free(video_buffer);
	video_buffer = NULL;
	video_buffer_size = 0;
	current_screen_size = -1;
	free(frame_indexes);
	frame_indexes = NULL;
	num_frames_allocated = 0;
	return result;
}
#endif /* AVI_VIDEO_RECORDING */