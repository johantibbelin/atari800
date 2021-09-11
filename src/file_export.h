#ifndef FILE_EXPORT_H_
#define FILE_EXPORT_H_

#include "atari.h"

/* Video codec initialization function. It must set up any internal
   configuration needed by the codec. Return the maximum size of the buffer
   needed to store the compressed video frame, or -1 on error. */
typedef int (*VIDEO_CODEC_Init)(int width, int height, int left_margin, int top_margin);

/* Video codec frame creation function. Given the pointer to the screen data and
   whether to produce a keyframe or interframe, store the compressed frame into
   buf. Return the size of the compressed frame in bytes, or -1 on error. */
typedef int (*VIDEO_CODEC_CreateFrame)(UBYTE *source, int keyframe, UBYTE *buf, int bufsize);

/* Video codec cleanup function. Free any data allocated in the init function. Return 1 on
   success, or zero on error. */
typedef int (*VIDEO_CODEC_End)(void);

typedef struct {
    char *codec_id;
    char *description;
    char fourcc[4];
    char avi_compression[4];
    int uses_interframes;
    VIDEO_CODEC_Init init;
    VIDEO_CODEC_CreateFrame frame;
    VIDEO_CODEC_End end;
} VIDEO_CODEC_t;

#if defined(HAVE_LIBPNG) || defined(HAVE_LIBZ)
extern int FILE_EXPORT_compression_level;
#endif

int File_Export_Initialise(int *argc, char *argv[]);
int File_Export_ReadConfig(char *string, char *ptr);
void File_Export_WriteConfig(FILE *fp);

#if defined(SOUND) || defined(AVI_VIDEO_RECORDING)
int File_Export_ElapsedTime(void);
int File_Export_CurrentSize(void);
char *File_Export_Description(void);
#endif

void fputw(UWORD, FILE *fp);
void fputl(ULONG, FILE *fp);
size_t fwritele(const void *ptr, size_t size, size_t nmemb, FILE *fp);

void PCX_SaveScreen(FILE *fp, UBYTE *ptr1, UBYTE *ptr2);
#ifdef HAVE_LIBPNG
int PNG_SaveScreen(FILE *fp, UBYTE *ptr1, UBYTE *ptr2);
#endif

#ifdef SOUND
FILE *WAV_OpenFile(const char *szFileName);
int WAV_WriteSamples(const unsigned char *buf, unsigned int num_samples, FILE *fp);
int WAV_CloseFile(FILE *fp);
#endif

#ifdef AVI_VIDEO_RECORDING
FILE *AVI_OpenFile(const char *szFileName);
int AVI_CloseFile(FILE *fp);
int AVI_AddVideoFrame(FILE *fp);
#ifdef SOUND
int AVI_AddAudioSamples(const UBYTE *buf, int num_samples, FILE *fp);
#endif /* SOUND */
#endif /* AVI_VIDEO_RECORDING */

#endif /* FILE_EXPORT_H_ */
