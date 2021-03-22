/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "controls.h"
#include "movie.h"
#include "logger.h"
#include "conffile.h"
#include "blit.h"
#include "display.h"

struct Image
{
	// TODO: replace XImage* with something?
	// XImage*	ximage;
	char *data;

	uint32 height;
	uint32 data_size;
	uint32 bits_per_pixel;
	uint32 bytes_per_line;
};

struct GUIData
{
	int depth;
	int pixel_format;
	int bytes_per_pixel;
	uint32 red_shift;
	uint32 blue_shift;
	uint32 green_shift;
	uint32 red_size;
	uint32 green_size;
	uint32 blue_size;
	Image *image;
	uint8 *snes_buffer;
	uint8 *filter_buffer;
	uint8 *blit_screen;
	uint32 blit_screen_pitch;
	bool8 need_convert;
	int x_offset;
	int y_offset;
};

static struct GUIData GUI;

typedef std::pair<std::string, std::string> strpair_t;
extern std::vector<strpair_t> keymaps;

typedef void (*Blitter)(uint8 *, int, uint8 *, int, int, int);

static void SetupImage(void);
static void TakedownImage(void);
static void SetupXImage(void);
static void Convert16To24(int, int);
static void Convert16To24Packed(int, int);

void S9xExtraDisplayUsage(void) {}

void S9xParseDisplayArg(char **argv, int &i, int argc) {}

const char *S9xParseDisplayConfig(ConfigFile &conf, int pass)
{
	return ("Unix/X11");
}

static void FatalError(const char *str)
{
	fprintf(stderr, "%s\n", str);
	S9xExit();
}

void S9xInitDisplay(int argc, char **argv)
{
	// Init various scale-filters
	S9xBlitFilterInit();
	S9xBlit2xSaIFilterInit();
	S9xBlitHQ2xFilterInit();

	SetupImage();
}

void S9xDeinitDisplay(void)
{
	TakedownImage();

	S9xBlitFilterDeinit();
	S9xBlit2xSaIFilterDeinit();
	S9xBlitHQ2xFilterDeinit();
}

static void SetupImage(void)
{
	TakedownImage();

	// Create new image struct
	GUI.image = (Image *)calloc(sizeof(Image), 1);

	SetupXImage();

	// Setup SNES buffers
	GFX.Pitch = SNES_WIDTH * 2 * 2;
	GUI.snes_buffer = (uint8 *)calloc(GFX.Pitch * ((SNES_HEIGHT_EXTENDED + 4) * 2), 1);
	if (!GUI.snes_buffer)
		FatalError("Failed to allocate GUI.snes_buffer.");

	GFX.Screen = (uint16 *)(GUI.snes_buffer + (GFX.Pitch * 2 * 2));

	GUI.filter_buffer = (uint8 *)calloc((SNES_WIDTH * 2) * 2 * (SNES_HEIGHT_EXTENDED * 2), 1);
	if (!GUI.filter_buffer)
		FatalError("Failed to allocate GUI.filter_buffer.");

	if (GUI.depth == 15 || GUI.depth == 16)
	{
		GUI.blit_screen_pitch = GUI.image->bytes_per_line;
		GUI.blit_screen = (uint8 *)GUI.image->data;
		GUI.need_convert = FALSE;
	}
	else
	{
		GUI.blit_screen_pitch = (SNES_WIDTH * 2) * 2;
		GUI.blit_screen = GUI.filter_buffer;
		GUI.need_convert = TRUE;
	}
	if (GUI.need_convert)
	{
		printf("\tImage conversion needed before blit.\n");
	}

	S9xGraphicsInit();
}

static void TakedownImage(void)
{
	if (GUI.snes_buffer)
	{
		free(GUI.snes_buffer);
		GUI.snes_buffer = NULL;
	}

	if (GUI.filter_buffer)
	{
		free(GUI.filter_buffer);
		GUI.filter_buffer = NULL;
	}

	if (GUI.image)
	{
		free(GUI.image);
		GUI.image = NULL;
	}

	S9xGraphicsDeinit();
}

static void SetupXImage(void)
{
	// TODO: Replace with image creation not using X11
	// GUI.image->ximage = XCreateImage(GUI.display, GUI.visual, GUI.depth, ZPixmap, 0, NULL, SNES_WIDTH * 2, SNES_HEIGHT_EXTENDED * 2, BitmapUnit(GUI.display), 0);
	// set main Image struct vars
	
	// GUI.image->height = GUI.image->ximage->height;
	// GUI.image->bytes_per_line = GUI.image->ximage->bytes_per_line;
	// GUI.image->data_size = GUI.image->bytes_per_line * GUI.image->height;

// 	GUI.image->ximage->data = (char *)malloc(GUI.image->data_size);
// 	if (!GUI.image->ximage || !GUI.image->ximage->data)
// 		FatalError("XCreateImage failed.");
// 	printf("Created XImage, size %d\n", GUI.image->data_size);

// 	// Set final values
// 	GUI.image->bits_per_pixel = GUI.image->ximage->bits_per_pixel;
// 	GUI.image->data = GUI.image->ximage->data;

// #ifdef LSB_FIRST
// 	GUI.image->ximage->byte_order = LSBFirst;
// #else
// 	GUI.image->ximage->byte_order = MSBFirst;
// #endif
}

void S9xPutImage(int width, int height)
{
	static int prevHeight = 0;
	int copyWidth, copyHeight;
	Blitter blitFn = NULL;

	if (width <= SNES_WIDTH)
	{
		if (height > SNES_HEIGHT_EXTENDED)
		{
			copyWidth = width * 2;
			copyHeight = height;
			blitFn = S9xBlitPixSimple2x1;
		}
		else
		{
			copyWidth = width * 2;
			copyHeight = height * 2;
		}
	}
	else if (height <= SNES_HEIGHT_EXTENDED)
	{
		copyWidth = width;
		copyHeight = height * 2;
	}
	else
	{
		copyWidth = width;
		copyHeight = height;
		blitFn = S9xBlitPixSimple1x1;
	}
	blitFn((uint8 *)GFX.Screen, GFX.Pitch, GUI.blit_screen, GUI.blit_screen_pitch, width, height);

	if (height < prevHeight)
	{
		int p = GUI.blit_screen_pitch >> 2;
		for (int y = SNES_HEIGHT * 2; y < SNES_HEIGHT_EXTENDED * 2; y++)
		{
			uint32 *d = (uint32 *)(GUI.blit_screen + y * GUI.blit_screen_pitch);
			for (int x = 0; x < p; x++)
				*d++ = 0;
		}
	}

	if (GUI.need_convert)
	{
		if (GUI.bytes_per_pixel == 3)
			Convert16To24Packed(copyWidth, copyHeight);
		else
			Convert16To24(copyWidth, copyHeight);
	}

	// TODO: Replace with some kind of image flushing logic
	//Repaint(TRUE);
	prevHeight = height;
}

static void Convert16To24(int width, int height)
{
	if (GUI.pixel_format == 565)
	{
		for (int y = 0; y < height; y++)
		{
			uint16 *s = (uint16 *)(GUI.blit_screen + y * GUI.blit_screen_pitch);
			uint32 *d = (uint32 *)(GUI.image->data + y * GUI.image->bytes_per_line);

			for (int x = 0; x < width; x++)
			{
				uint32 pixel = *s++;
				*d++ = (((pixel >> 11) & 0x1f) << (GUI.red_shift + 3)) | (((pixel >> 6) & 0x1f) << (GUI.green_shift + 3)) | ((pixel & 0x1f) << (GUI.blue_shift + 3));
			}
		}
	}
	else
	{
		for (int y = 0; y < height; y++)
		{
			uint16 *s = (uint16 *)(GUI.blit_screen + y * GUI.blit_screen_pitch);
			uint32 *d = (uint32 *)(GUI.image->data + y * GUI.image->bytes_per_line);

			for (int x = 0; x < width; x++)
			{
				uint32 pixel = *s++;
				*d++ = (((pixel >> 10) & 0x1f) << (GUI.red_shift + 3)) | (((pixel >> 5) & 0x1f) << (GUI.green_shift + 3)) | ((pixel & 0x1f) << (GUI.blue_shift + 3));
			}
		}
	}
}

static void Convert16To24Packed(int width, int height)
{
	if (GUI.pixel_format == 565)
	{
		for (int y = 0; y < height; y++)
		{
			uint16 *s = (uint16 *)(GUI.blit_screen + y * GUI.blit_screen_pitch);
			uint8 *d = (uint8 *)(GUI.image->data + y * GUI.image->bytes_per_line);

#ifdef LSB_FIRST
			if (GUI.red_shift < GUI.blue_shift)
#else
			if (GUI.red_shift > GUI.blue_shift)
#endif
			{
				for (int x = 0; x < width; x++)
				{
					uint32 pixel = *s++;
					*d++ = (pixel >> (11 - 3)) & 0xf8;
					*d++ = (pixel >> (6 - 3)) & 0xf8;
					*d++ = (pixel & 0x1f) << 3;
				}
			}
			else
			{
				for (int x = 0; x < width; x++)
				{
					uint32 pixel = *s++;
					*d++ = (pixel & 0x1f) << 3;
					*d++ = (pixel >> (6 - 3)) & 0xf8;
					*d++ = (pixel >> (11 - 3)) & 0xf8;
				}
			}
		}
	}
	else
	{
		for (int y = 0; y < height; y++)
		{
			uint16 *s = (uint16 *)(GUI.blit_screen + y * GUI.blit_screen_pitch);
			uint8 *d = (uint8 *)(GUI.image->data + y * GUI.image->bytes_per_line);

#ifdef LSB_FIRST
			if (GUI.red_shift < GUI.blue_shift)
#else
			if (GUI.red_shift > GUI.blue_shift)
#endif
			{
				for (int x = 0; x < width; x++)
				{
					uint32 pixel = *s++;
					*d++ = (pixel >> (10 - 3)) & 0xf8;
					*d++ = (pixel >> (5 - 3)) & 0xf8;
					*d++ = (pixel & 0x1f) << 3;
				}
			}
			else
			{
				for (int x = 0; x < width; x++)
				{
					uint32 pixel = *s++;
					*d++ = (pixel & 0x1f) << 3;
					*d++ = (pixel >> (5 - 3)) & 0xf8;
					*d++ = (pixel >> (10 - 3)) & 0xf8;
				}
			}
		}
	}
}

void S9xTextMode(void) {}

void S9xGraphicsMode(void) {}

void S9xLatchJSEvent()
{
	// record that a JS event happened and was reported to the engine
	// GUI.js_event_latch = TRUE;
}

void S9xProcessEvents(bool8 block) {}

const char *S9xSelectFilename(const char *def, const char *dir1, const char *ext1, const char *title)
{
	static char s[PATH_MAX + 1];
	char buffer[PATH_MAX + 1];

	printf("\n%s (default: %s): ", title, def);
	fflush(stdout);

	if (fgets(buffer, PATH_MAX + 1, stdin))
	{
		char drive[_MAX_DRIVE + 1], dir[_MAX_DIR + 1], fname[_MAX_FNAME + 1], ext[_MAX_EXT + 1];

		char *p = buffer;
		while (isspace(*p))
			p++;
		if (!*p)
		{
			strncpy(buffer, def, PATH_MAX + 1);
			buffer[PATH_MAX] = 0;
			p = buffer;
		}

		char *q = strrchr(p, '\n');
		if (q)
			*q = 0;

		_splitpath(p, drive, dir, fname, ext);
		_makepath(s, drive, *dir ? dir : dir1, fname, *ext ? ext : ext1);

		return (s);
	}

	return (NULL);
}

void S9xMessage(int type, int number, const char *message)
{
	const int max = 36 * 3;
	static char buffer[max + 1];

	fprintf(stdout, "%s\n", message);
	strncpy(buffer, message, max + 1);
	buffer[max] = 0;
	S9xSetInfoString(buffer);
}

const char *S9xStringInput(const char *message)
{
	static char buffer[256];

	printf("%s: ", message);
	fflush(stdout);

	if (fgets(buffer, sizeof(buffer) - 2, stdin))
		return (buffer);

	return (NULL);
}

void S9xSetTitle(const char *string)
{
	// TODO: Replace with something
	// XStoreName(GUI.display, GUI.window, string);
	// XFlush(GUI.display);
}

s9xcommand_t S9xGetDisplayCommandT(const char *n)
{
	s9xcommand_t cmd;

	cmd.type = S9xBadMapping;
	cmd.multi_press = 0;
	cmd.button_norpt = 0;
	cmd.port[0] = 0xff;
	cmd.port[1] = 0;
	cmd.port[2] = 0;
	cmd.port[3] = 0;

	return (cmd);
}

char *S9xGetDisplayCommandName(s9xcommand_t cmd)
{
	return (strdup("None"));
}

void S9xHandleDisplayCommand(s9xcommand_t cmd, int16 data1, int16 data2)
{
	return;
}

bool8 S9xMapDisplayInput(const char *n, s9xcommand_t *cmd)
{
	return (false);
}

bool S9xDisplayPollButton(uint32 id, bool *pressed)
{
	return (false);
}

bool S9xDisplayPollAxis(uint32 id, int16 *value)
{
	return (false);
}

bool S9xDisplayPollPointer(uint32 id, int16 *x, int16 *y)
{
	if ((id & 0xc0008000) != 0x40008000)
		return (false);

	int d = (id >> 24) & 0x3f,
		n = id & 0x7fff;

	if (d != 0 || n != 0)
		return (false);

	return (true);
}