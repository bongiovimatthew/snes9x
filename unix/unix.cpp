/*****************************************************************************\
     Snes9x - Portable Super Nintendo Entertainment System (TM) emulator.
                This file is licensed under the Snes9x License.
   For further information, consult the LICENSE file in the root directory.
\*****************************************************************************/

#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <algorithm>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/soundcard.h>
#include <sys/mman.h>
#include "snes9x.h"
#include "memmap.h"
#include "apu/apu.h"
#include "gfx.h"
#include "snapshot.h"
#include "controls.h"
#include "cheats.h"
#include "movie.h"
#include "logger.h"
#include "display.h"
#include "conffile.h"
#include "statemanager.h"

typedef std::pair<std::string, std::string>	strpair_t;

ConfigFile::secvec_t	keymaps;

StateManager stateMan;

#define FIXED_POINT				0x10000
#define FIXED_POINT_SHIFT		16
#define FIXED_POINT_REMAINDER	0xffff
#define SOUND_BUFFER_SIZE		(1024 * 16)
#define SOUND_BUFFER_SIZE_MASK	(SOUND_BUFFER_SIZE - 1)

static volatile bool8	block_signal         = FALSE;
static volatile bool8	block_generate_sound = FALSE;

static const char	*sound_device = NULL;

static const char	*s9x_base_dir        = NULL,
					*rom_filename        = NULL,
					*snapshot_filename   = NULL,
					*play_smv_filename   = NULL,
					*record_smv_filename = NULL;

static char		default_dir[PATH_MAX + 1];

static const char	dirNames[13][32] =
{
	"",				// DEFAULT_DIR
	"",				// HOME_DIR
	"",				// ROMFILENAME_DIR
	"rom",			// ROM_DIR
	"sram",			// SRAM_DIR
	"savestate",	// SNAPSHOT_DIR
	"screenshot",	// SCREENSHOT_DIR
	"spc",			// SPC_DIR
	"cheat",		// CHEAT_DIR
	"patch",		// PATCH_DIR
	"bios",			// BIOS_DIR
	"log",			// LOG_DIR
	""
};

struct SUnixSettings
{
	bool8	JoystickEnabled;
	bool8	ThreadSound;
	uint32	SoundBufferSize;
	uint32	SoundFragmentSize;
	uint32	rewindBufferSize;
	uint32	rewindGranularity;
};

struct SoundStatus
{
	int		sound_fd;
	uint32	fragment_size;
};


static int frame_advance = 0;
static SUnixSettings	unixSettings;
static SoundStatus		so;

static bool8	rewinding;

bool8 S9xMapDisplayInput (const char *, s9xcommand_t *);
s9xcommand_t S9xGetDisplayCommandT (const char *);
char * S9xGetDisplayCommandName (s9xcommand_t);
void S9xHandleDisplayCommand (s9xcommand_t, int16, int16);
bool S9xDisplayPollButton (uint32, bool *);
bool S9xDisplayPollAxis (uint32, int16 *);
bool S9xDisplayPollPointer (uint32, int16 *, int16 *);

static void LogSpecialMemory (void);
static void NSRTControllerSetup (void);
static int make_snes9x_dirs (void);

static long log2 (long num)
{
	long	n = 0;

	while (num >>= 1)
		n++;

	return (n);
}

namespace {

	class S9xAudioOutput
	{
	public:
		S9xAudioOutput(int fd, uint32 sampleRateHz, bool isThreaded)
		{
			m_FD = fd;
			uint32 bufferSizeMS = unixSettings.SoundBufferSize; // milliseconds
			// 4 = sizeof(uint16) * STEREO
			m_BufferSize = int(uint64(sampleRateHz) * bufferSizeMS / 1000 * 4);
		}

		~S9xAudioOutput()
		{
		}

		void Write(void* data, int size)
		{
			{
				WriteImpl(data, size);
			}
		}

		int GetFreeBufferSize()
		{
			{
				audio_buf_info info;
				// ioctl(m_FD, SNDCTL_DSP_GETOSPACE, &info);
				int writtenSize = info.fragsize * info.fragstotal - info.bytes;
				return std::max(0, m_BufferSize - writtenSize);
			}
		}

	private:
		void WriteImpl(const void* data, int size)
		{
			const char* p = reinterpret_cast<const char*>(data);
			while (size > 0)
			{
				int result = write(m_FD, p, size);
				if (result < 0)
				{
					return;
				}
				p += result;
				size -= result;
			}
		}

		int m_FD;
		int m_BufferSize;
	};

	S9xAudioOutput* s_AudioOutput = NULL;

}

void S9xExtraUsage (void)
{
	/*                               12345678901234567890123456789012345678901234567890123456789012345678901234567890 */

	S9xMessage(S9X_INFO, S9X_USAGE, "-multi                          Enable multi cartridge system");
	S9xMessage(S9X_INFO, S9X_USAGE, "-carta <filename>               ROM in slot A (use with -multi)");
	S9xMessage(S9X_INFO, S9X_USAGE, "-cartb <filename>               ROM in slot B (use with -multi)");
	S9xMessage(S9X_INFO, S9X_USAGE, "");

	S9xMessage(S9X_INFO, S9X_USAGE, "-buffersize                     Sound generating buffer size in millisecond");
	S9xMessage(S9X_INFO, S9X_USAGE, "-fragmentsize                   Sound playback buffer fragment size in bytes");
	S9xMessage(S9X_INFO, S9X_USAGE, "-sounddev <string>              Specify sound device");
	S9xMessage(S9X_INFO, S9X_USAGE, "");

	S9xMessage(S9X_INFO, S9X_USAGE, "-loadsnapshot                   Load snapshot file at start");
	S9xMessage(S9X_INFO, S9X_USAGE, "-playmovie <filename>           Start emulator playing the .smv file");
	S9xMessage(S9X_INFO, S9X_USAGE, "-recordmovie <filename>         Start emulator recording the .smv file");
	S9xMessage(S9X_INFO, S9X_USAGE, "-dumpstreams                    Save audio/video data to disk");
	S9xMessage(S9X_INFO, S9X_USAGE, "-dumpmaxframes <num>            Stop emulator after saving specified number of");
	S9xMessage(S9X_INFO, S9X_USAGE, "                                frames (use with -dumpstreams)");
	S9xMessage(S9X_INFO, S9X_USAGE, "");

	S9xMessage(S9X_INFO, S9X_USAGE, "-rwbuffersize                   Rewind buffer size in MB");
	S9xMessage(S9X_INFO, S9X_USAGE, "-rwgranularity                  Rewind granularity in frames");
	S9xMessage(S9X_INFO, S9X_USAGE, "");

	S9xExtraDisplayUsage();
}

void S9xParseArg (char **argv, int &i, int argc)
{
	if (!strcasecmp(argv[i], "-multi"))
		Settings.Multi = TRUE;
	else
	if (!strcasecmp(argv[i], "-carta"))
	{
		if (i + 1 < argc)
			strncpy(Settings.CartAName, argv[++i], _MAX_PATH);
		else
			S9xUsage();
	}
	else
	if (!strcasecmp(argv[i], "-cartb"))
	{
		if (i + 1 < argc)
			strncpy(Settings.CartBName, argv[++i], _MAX_PATH);
		else
			S9xUsage();
	}
	else
	if (!strcasecmp(argv[i], "-buffersize"))
	{
		if (i + 1 < argc)
			unixSettings.SoundBufferSize = atoi(argv[++i]);
		else
			S9xUsage();
	}
	else
	if (!strcasecmp(argv[i], "-fragmentsize"))
	{
		if (i + 1 < argc)
			unixSettings.SoundFragmentSize = atoi(argv[++i]);
		else
			S9xUsage();
	}
	else
	if (!strcasecmp(argv[i], "-sounddev"))
	{
		if (i + 1 < argc)
			sound_device = argv[++i];
		else
			S9xUsage();
	}
	else
	if (!strcasecmp(argv[i], "-loadsnapshot"))
	{
		if (i + 1 < argc)
			snapshot_filename = argv[++i];
		else
			S9xUsage();
	}
	else
	if (!strcasecmp(argv[i], "-playmovie"))
	{
		if (i + 1 < argc)
			play_smv_filename = argv[++i];
		else
			S9xUsage();
	}
	else
	if (!strcasecmp(argv[i], "-recordmovie"))
	{
		if (i + 1 < argc)
			record_smv_filename = argv[++i];
		else
			S9xUsage();
	}
	else
	if (!strcasecmp(argv[i], "-dumpstreams"))
		Settings.DumpStreams = TRUE;
	else
	if (!strcasecmp(argv[i], "-dumpmaxframes"))
		Settings.DumpStreamsMaxFrames = atoi(argv[++i]);
	else
	if (!strcasecmp(argv[i], "-rwbuffersize"))
	{
		if (i + 1 < argc)
			unixSettings.rewindBufferSize = atoi(argv[++i]);
		else
			S9xUsage();
	}
	else
	if (!strcasecmp(argv[i], "-rwgranularity"))
	{
		if (i + 1 < argc)
			unixSettings.rewindGranularity = atoi(argv[++i]);
		else
			S9xUsage();
	}
	else
		S9xParseDisplayArg(argv, i, argc);
}

static void NSRTControllerSetup (void)
{
	if (!strncmp((const char *) Memory.NSRTHeader + 24, "NSRT", 4))
	{
		// First plug in both, they'll change later as needed
		S9xSetController(0, CTL_JOYPAD, 0, 0, 0, 0);
		S9xSetController(1, CTL_JOYPAD, 1, 0, 0, 0);

		switch (Memory.NSRTHeader[29])
		{
			case 0x00:	// Everything goes
				break;

			case 0x10:	// Mouse in Port 0
				S9xSetController(0, CTL_MOUSE,      0, 0, 0, 0);
				break;

			case 0x01:	// Mouse in Port 1
				S9xSetController(1, CTL_MOUSE,      1, 0, 0, 0);
				break;

			case 0x03:	// Super Scope in Port 1
				S9xSetController(1, CTL_SUPERSCOPE, 0, 0, 0, 0);
				break;

			case 0x06:	// Multitap in Port 1
				S9xSetController(1, CTL_MP5,        1, 2, 3, 4);
				break;

			case 0x66:	// Multitap in Ports 0 and 1
				S9xSetController(0, CTL_MP5,        0, 1, 2, 3);
				S9xSetController(1, CTL_MP5,        4, 5, 6, 7);
				break;

			case 0x08:	// Multitap in Port 1, Mouse in new Port 1
				S9xSetController(1, CTL_MOUSE,      1, 0, 0, 0);
				// There should be a toggle here for putting in Multitap instead
				break;

			case 0x04:	// Pad or Super Scope in Port 1
				S9xSetController(1, CTL_SUPERSCOPE, 0, 0, 0, 0);
				// There should be a toggle here for putting in a pad instead
				break;

			case 0x05:	// Justifier - Must ask user...
				S9xSetController(1, CTL_JUSTIFIER,  1, 0, 0, 0);
				// There should be a toggle here for how many justifiers
				break;

			case 0x20:	// Pad or Mouse in Port 0
				S9xSetController(0, CTL_MOUSE,      0, 0, 0, 0);
				// There should be a toggle here for putting in a pad instead
				break;

			case 0x22:	// Pad or Mouse in Port 0 & 1
				S9xSetController(0, CTL_MOUSE,      0, 0, 0, 0);
				S9xSetController(1, CTL_MOUSE,      1, 0, 0, 0);
				// There should be a toggles here for putting in pads instead
				break;

			case 0x24:	// Pad or Mouse in Port 0, Pad or Super Scope in Port 1
				// There should be a toggles here for what to put in, I'm leaving it at gamepad for now
				break;

			case 0x27:	// Pad or Mouse in Port 0, Pad or Mouse or Super Scope in Port 1
				// There should be a toggles here for what to put in, I'm leaving it at gamepad for now
				break;

			// Not Supported yet
			case 0x99:	// Lasabirdie
				break;

			case 0x0A:	// Barcode Battler
				break;
		}
	}
}

void S9xParsePortConfig (ConfigFile &conf, int pass)
{
	s9x_base_dir                   = conf.GetStringDup("Unix::BaseDir",             default_dir);
	snapshot_filename              = conf.GetStringDup("Unix::SnapshotFilename",    NULL);
	play_smv_filename              = conf.GetStringDup("Unix::PlayMovieFilename",   NULL);
	record_smv_filename            = conf.GetStringDup("Unix::RecordMovieFilename", NULL);
	unixSettings.SoundBufferSize   = conf.GetUInt     ("Unix::SoundBufferSize",     100);
	unixSettings.SoundFragmentSize = conf.GetUInt     ("Unix::SoundFragmentSize",   2048);
	sound_device                   = conf.GetStringDup("Unix::SoundDevice",         "/dev/dsp");

	keymaps.clear();
	if (!conf.GetBool("Unix::ClearAllControls", false))
	{
	#if 0
		// Using an axis to control Pseudo-pointer #1
		keymaps.push_back(strpair_t("J00:Axis0",      "AxisToPointer 1h Var"));
		keymaps.push_back(strpair_t("J00:Axis1",      "AxisToPointer 1v Var"));
		keymaps.push_back(strpair_t("PseudoPointer1", "Pointer C=2 White/Black Superscope"));
	#elif 0
		// Using an Axis for Pseudo-buttons
		keymaps.push_back(strpair_t("J00:Axis0",      "AxisToButtons 1/0 T=50%"));
		keymaps.push_back(strpair_t("J00:Axis1",      "AxisToButtons 3/2 T=50%"));
		keymaps.push_back(strpair_t("PseudoButton0",  "Joypad1 Right"));
		keymaps.push_back(strpair_t("PseudoButton1",  "Joypad1 Left"));
		keymaps.push_back(strpair_t("PseudoButton2",  "Joypad1 Down"));
		keymaps.push_back(strpair_t("PseudoButton3",  "Joypad1 Up"));
	#else
		// Using 'Joypad# Axis'
		keymaps.push_back(strpair_t("J00:Axis0",      "Joypad1 Axis Left/Right T=50%"));
		keymaps.push_back(strpair_t("J00:Axis1",      "Joypad1 Axis Up/Down T=50%"));
	#endif
		keymaps.push_back(strpair_t("J00:B0",         "Joypad1 X"));
		keymaps.push_back(strpair_t("J00:B1",         "Joypad1 A"));
		keymaps.push_back(strpair_t("J00:B2",         "Joypad1 B"));
		keymaps.push_back(strpair_t("J00:B3",         "Joypad1 Y"));
	#if 1
		keymaps.push_back(strpair_t("J00:B6",         "Joypad1 L"));
	#else
		// Show off joypad-meta
		keymaps.push_back(strpair_t("J00:X+B6",       "JS1 Meta1"));
		keymaps.push_back(strpair_t("J00:M1+B1",      "Joypad1 Turbo A"));
	#endif
		keymaps.push_back(strpair_t("J00:B7",         "Joypad1 R"));
		keymaps.push_back(strpair_t("J00:B8",         "Joypad1 Select"));
		keymaps.push_back(strpair_t("J00:B11",        "Joypad1 Start"));
	}

	std::string section = S9xParseDisplayConfig(conf, 1);

	ConfigFile::secvec_t	sec = conf.GetSection((section + " Controls").c_str());
	for (ConfigFile::secvec_t::iterator c = sec.begin(); c != sec.end(); c++)
		keymaps.push_back(*c);
}

static int make_snes9x_dirs (void)
{
	if (strlen(s9x_base_dir) + 1 + sizeof(dirNames[0]) > PATH_MAX + 1)
		return (-1);

	mkdir(s9x_base_dir, 0755);

	for (int i = 0; i < LAST_DIR; i++)
	{
		if (dirNames[i][0])
		{
			char	s[PATH_MAX + 1];
			snprintf(s, PATH_MAX + 1, "%s%s%s", s9x_base_dir, SLASH_STR, dirNames[i]);
			mkdir(s, 0755);
		}
	}

	return (0);
}

const char * S9xGetDirectory (enum s9x_getdirtype dirtype)
{
	static char	s[PATH_MAX + 1];

	if (dirNames[dirtype][0])
		snprintf(s, PATH_MAX + 1, "%s%s%s", s9x_base_dir, SLASH_STR, dirNames[dirtype]);
	else
	{
		switch (dirtype)
		{
			case DEFAULT_DIR:
				strncpy(s, s9x_base_dir, PATH_MAX + 1);
				s[PATH_MAX] = 0;
				break;

			case HOME_DIR:
				strncpy(s, getenv("HOME"), PATH_MAX + 1);
				s[PATH_MAX] = 0;
				break;

			case ROMFILENAME_DIR:
				strncpy(s, Memory.ROMFilename, PATH_MAX + 1);
				s[PATH_MAX] = 0;

				for (int i = strlen(s); i >= 0; i--)
				{
					if (s[i] == SLASH_CHAR)
					{
						s[i] = 0;
						break;
					}
				}

				break;

			default:
				s[0] = 0;
				break;
		}
	}

	return (s);
}

const char * S9xGetFilename (const char *ex, enum s9x_getdirtype dirtype)
{
	static char	s[PATH_MAX + 1];
	char		drive[_MAX_DRIVE + 1], dir[_MAX_DIR + 1], fname[_MAX_FNAME + 1], ext[_MAX_EXT + 1];

	_splitpath(Memory.ROMFilename, drive, dir, fname, ext);
	snprintf(s, PATH_MAX + 1, "%s%s%s%s", S9xGetDirectory(dirtype), SLASH_STR, fname, ex);

	return (s);
}

const char * S9xGetFilenameInc (const char *ex, enum s9x_getdirtype dirtype)
{
	static char	s[PATH_MAX + 1];
	char		drive[_MAX_DRIVE + 1], dir[_MAX_DIR + 1], fname[_MAX_FNAME + 1], ext[_MAX_EXT + 1];

	unsigned int	i = 0;
	const char		*d;
	struct stat		buf;

	_splitpath(Memory.ROMFilename, drive, dir, fname, ext);
	d = S9xGetDirectory(dirtype);

	do
		snprintf(s, PATH_MAX + 1, "%s%s%s.%03d%s", d, SLASH_STR, fname, i++, ex);
	while (stat(s, &buf) == 0 && i < 1000);

	return (s);
}

const char * S9xBasename (const char *f)
{
	const char	*p;

	if ((p = strrchr(f, '/')) != NULL || (p = strrchr(f, '\\')) != NULL)
		return (p + 1);

	return (f);
}

bool8 S9xOpenSnapshotFile (const char *filename, bool8 read_only, STREAM *file)
{
	char	s[PATH_MAX + 1];
	char	drive[_MAX_DRIVE + 1], dir[_MAX_DIR + 1], fname[_MAX_FNAME + 1], ext[_MAX_EXT + 1];

	_splitpath(filename, drive, dir, fname, ext);

	if (*drive || *dir == SLASH_CHAR || (strlen(dir) > 1 && *dir == '.' && *(dir + 1) == SLASH_CHAR))
	{
		strncpy(s, filename, PATH_MAX + 1);
		s[PATH_MAX] = 0;
	}
	else
		snprintf(s, PATH_MAX + 1, "%s%s%s", S9xGetDirectory(SNAPSHOT_DIR), SLASH_STR, fname);

	if (!*ext && strlen(s) <= PATH_MAX - 4)
		strcat(s, ".frz");

	if ((*file = OPEN_STREAM(s, read_only ? "rb" : "wb")))
		return (TRUE);

	return (FALSE);
}

void S9xCloseSnapshotFile (STREAM file)
{
	CLOSE_STREAM(file);
}

bool8 S9xInitUpdate (void)
{
	return (TRUE);
}

bool8 S9xDeinitUpdate (int width, int height)
{
	S9xPutImage(width, height);
	return (TRUE);
}

bool8 S9xContinueUpdate (int width, int height)
{
	return (TRUE);
}

void S9xToggleSoundChannel (int c)
{
	static uint8	sound_switch = 255;

	if (c == 8)
		sound_switch = 255;
	else
		sound_switch ^= 1 << c;

	S9xSetSoundControl(sound_switch);
}

void S9xAutoSaveSRAM (void)
{
	Memory.SaveSRAM(S9xGetFilename(".srm", SRAM_DIR));
}

void S9xSyncSpeed (void)
{
	if (Settings.SoundSync)
	{
		return;
	}

	if (Settings.DumpStreams)
		return;

	if (Settings.HighSpeedSeek > 0)
		Settings.HighSpeedSeek--;

	if (Settings.TurboMode)
	{
		if ((++IPPU.FrameSkip >= Settings.TurboSkipFrames) && !Settings.HighSpeedSeek)
		{
			IPPU.FrameSkip = 0;
			IPPU.SkippedFrames = 0;
			IPPU.RenderThisFrame = TRUE;
		}
		else
		{
			IPPU.SkippedFrames++;
			IPPU.RenderThisFrame = FALSE;
		}

		return;
	}

	static struct timeval	next1 = { 0, 0 };
	struct timeval			now;

	while (gettimeofday(&now, NULL) == -1) ;

	// If there is no known "next" frame, initialize it now.
	if (next1.tv_sec == 0)
	{
		next1 = now;
		next1.tv_usec++;
	}

	// If we're on AUTO_FRAMERATE, we'll display frames always only if there's excess time.
	// Otherwise we'll display the defined amount of frames.
	unsigned	limit = (Settings.SkipFrames == AUTO_FRAMERATE) ? (timercmp(&next1, &now, <) ? 10 : 1) : Settings.SkipFrames;

	IPPU.RenderThisFrame = (++IPPU.SkippedFrames >= limit) ? TRUE : FALSE;

	if (IPPU.RenderThisFrame)
		IPPU.SkippedFrames = 0;
	else
	{
		// If we were behind the schedule, check how much it is.
		if (timercmp(&next1, &now, <))
		{
			unsigned	lag = (now.tv_sec - next1.tv_sec) * 1000000 + now.tv_usec - next1.tv_usec;
			if (lag >= 500000)
			{
				// More than a half-second behind means probably pause.
				// The next line prevents the magic fast-forward effect.
				next1 = now;
			}
		}
	}

	// Delay until we're completed this frame.
	// Can't use setitimer because the sound code already could be using it. We don't actually need it either.
	while (timercmp(&next1, &now, >))
	{
		// If we're ahead of time, sleep a while.
		unsigned	timeleft = (next1.tv_sec - now.tv_sec) * 1000000 + next1.tv_usec - now.tv_usec;
		usleep(timeleft);

		while (gettimeofday(&now, NULL) == -1) ;
		// Continue with a while-loop because usleep() could be interrupted by a signal.
	}

	// Calculate the timestamp of the next frame.
	next1.tv_usec += Settings.FrameTime;
	if (next1.tv_usec >= 1000000)
	{
		next1.tv_sec += next1.tv_usec / 1000000;
		next1.tv_usec %= 1000000;
	}
}

bool8 S9xMapInput (const char *n, s9xcommand_t *cmd)
{
	int		i, j, d;
	char	*c;
	char	buf[4] = "M1+";

	if (!strncmp(n, "PseudoPointer", 13) && n[13] >= '1' && n[13] <= '8' && n[14] == '\0')
		return (S9xMapPointer(PseudoPointerBase + (n[13] - '1'), *cmd, false));

	if (!strncmp(n, "PseudoButton", 12) && isdigit(n[12]) && (j = strtol(n + 12, &c, 10)) < 256 && (c == NULL || *c == '\0'))
		return (S9xMapButton(PseudoButtonBase + j, *cmd, false));

	if (n[0] != 'J' || !isdigit(n[1]) || !isdigit(n[2]) || n[3] != ':')
		goto unrecog;

	d = ((n[1] - '0') * 10 + (n[2] - '0')) << 24;
	d |= 0x80000000;
	i = 4;
	if (!strncmp(n + i, "X+", 2))
	{
		d |= 0x4000;
		i += 2;
	}
	else
	{
		for (buf[1] = '1'; buf[1] <= '8'; buf[1]++)
		{
			if (!strncmp(n + i, buf, 3))
			{
				d |= 1 << (buf[1] - '1' + 16);
				i += 3;
			}
		}
	}

	if (!strncmp(n + i, "Axis", 4))
	{
		d |= 0x8000;
		i += 4;
	}
	else
	if (n[i] == 'B')
		i++;
	else
		goto unrecog;

	d |= j = strtol(n + i, &c, 10);
	if ((c != NULL && *c != '\0') || j > 0x3fff)
		goto unrecog;

	if (d & 0x8000)
		return (S9xMapAxis(d, *cmd, false));

	return (S9xMapButton(d, *cmd, false));

unrecog:
	return (S9xMapDisplayInput(n, cmd));
}

bool S9xPollButton (uint32 id, bool *pressed)
{
	return (S9xDisplayPollButton(id, pressed));
}

bool S9xPollAxis (uint32 id, int16 *value)
{
	return (S9xDisplayPollAxis(id, value));
}

bool S9xPollPointer (uint32 id, int16 *x, int16 *y)
{
	return (S9xDisplayPollPointer(id, x, y));
}

s9xcommand_t S9xGetPortCommandT (const char *n)
{
	s9xcommand_t	cmd;

	cmd.type         = S9xBadMapping;
	cmd.multi_press  = 0;
	cmd.button_norpt = 0;
	cmd.port[0]      = 0;
	cmd.port[1]      = 0;
	cmd.port[2]      = 0;
	cmd.port[3]      = 0;

	if (!strncmp(n, "JS", 2) && n[2] >= '1' && n[2] <= '8')
	{
		if (!strncmp(n + 3, " Meta", 5) && n[8] >= '1' && n[8] <= '8' && n[9] == '\0')
		{
			cmd.type    = S9xButtonPort;
			cmd.port[1] = 0;
			cmd.port[2] = n[2] - '1';
			cmd.port[3] = 1 << (n[8] - '1');

			return (cmd);
		}
		else
		if (!strncmp(n + 3, " ToggleMeta", 11) && n[14] >= '1' && n[14] <= '8' && n[15] == '\0')
		{
			cmd.type    = S9xButtonPort;
			cmd.port[1] = 1;
			cmd.port[2] = n[2] - '1';
			cmd.port[3] = 1 << (n[13] - '1');

			return (cmd);
		}
	} else
	if (!strcmp(n,"Rewind"))
	{
		cmd.type = S9xButtonPort;
		cmd.port[1] = 2;

		return (cmd);
	}
        else if (!strcmp(n, "Advance"))
        {
                cmd.type = S9xButtonPort;
                cmd.port[1] = 3;
                return (cmd);
        }

	return (S9xGetDisplayCommandT(n));
}

char * S9xGetPortCommandName (s9xcommand_t cmd)
{
	std::string	x;

	switch (cmd.type)
	{
		case S9xButtonPort:
			if (cmd.port[0] != 0)
				break;

			switch (cmd.port[1])
			{
				case 0:
					x = "JS";
					x += (int) cmd.port[2];
					x += " Meta";
					x += (int) cmd.port[3];
					return (strdup(x.c_str()));

				case 1:
					x = "JS";
					x += (int) cmd.port[2];
					x += " ToggleMeta";
					x += (int) cmd.port[3];
					return (strdup(x.c_str()));

				case 2:
					return (strdup("Rewind"));

                                case 3:
                                        return (strdup("Advance"));
			}

			break;

		case S9xAxisPort:
			break;

		case S9xPointerPort:
			break;
	}

	return (S9xGetDisplayCommandName(cmd));
}

void S9xHandlePortCommand (s9xcommand_t cmd, int16 data1, int16 data2)
{
}

void S9xSetupDefaultKeymap (void)
{
	s9xcommand_t	cmd;

	S9xUnmapAllControls();

	for (ConfigFile::secvec_t::iterator i = keymaps.begin(); i != keymaps.end(); i++)
	{
		cmd = S9xGetPortCommandT(i->second.c_str());

		if (cmd.type == S9xBadMapping)
		{
			cmd = S9xGetCommandT(i->second.c_str());
			if (cmd.type == S9xBadMapping)
			{
				std::string	s("Unrecognized command '");
				s += i->second + "'";
				perror(s.c_str());
				continue;
			}
		}

		if (!S9xMapInput(i->first.c_str(), &cmd))
		{
			std::string	s("Could not map '");
			s += i->second + "' to '" + i->first + "'";
			perror(s.c_str());
			continue;
		}
	}

	keymaps.clear();
}

void S9xInitInputDevices (void)
{
}


void S9xSamplesAvailable(void *data)
{
    int samples_to_write;
	static uint8 *sound_buffer = NULL;
	static int sound_buffer_size = 0;

    if (Settings.DynamicRateControl)
    {
        S9xUpdateDynamicRate(s_AudioOutput->GetFreeBufferSize(), so.fragment_size * 4);
    }

    samples_to_write = S9xGetSampleCount();

    if (samples_to_write < 0)
        return;

    if (sound_buffer_size < samples_to_write * 2)
    {
        sound_buffer = (uint8 *)realloc(sound_buffer, samples_to_write * 2);
        sound_buffer_size = samples_to_write * 2;
    }

    S9xMixSamples(sound_buffer, samples_to_write);

    s_AudioOutput->Write(sound_buffer, samples_to_write * 2);

}

bool8 S9xOpenSoundDevice (void)
{
	int	J, K;

	so.sound_fd = open(sound_device, O_WRONLY | O_NONBLOCK);
	if (so.sound_fd == -1)
	{
		fprintf(stderr, "ERROR: Failed to open sound device %s for writing.\n\t(Try loading snd-pcm-oss module?)\n", sound_device);
		return (FALSE);
	}

	s_AudioOutput = new S9xAudioOutput(
		so.sound_fd,
		Settings.SoundPlaybackRate,
		bool(unixSettings.ThreadSound)
	);

	J = log2(unixSettings.SoundFragmentSize) | (4 << 16);
	// if (ioctl(so.sound_fd, SNDCTL_DSP_SETFRAGMENT, &J) == -1)
	// 	return (FALSE);

	J = K = AFMT_S16_NE;
	// if (ioctl(so.sound_fd, SNDCTL_DSP_SETFMT,      &J) == -1 || J != K)
	// 	return (FALSE);

	J = K = 1;
	// if (ioctl(so.sound_fd, SNDCTL_DSP_STEREO,      &J) == -1 || J != K)
	// 	return (FALSE);

	J = K = Settings.SoundPlaybackRate;
	// if (ioctl(so.sound_fd, SNDCTL_DSP_SPEED,       &J) == -1 || J != K)
	// 	return (FALSE);

	J = 0;
	// if (ioctl(so.sound_fd, SNDCTL_DSP_GETBLKSIZE,  &J) == -1)
	// 	return (FALSE);

	so.fragment_size = J;
	printf("fragment size: %d\n", J);

	S9xSetSamplesAvailableCallback(S9xSamplesAvailable, NULL);

	return (TRUE);
}


void S9xExit (void)
{
	S9xMovieShutdown();

	S9xSetSoundMute(TRUE);
	Settings.StopEmulation = TRUE;

	delete s_AudioOutput;

	Memory.SaveSRAM(S9xGetFilename(".srm", SRAM_DIR));
	S9xResetSaveTimer(FALSE);
	S9xSaveCheatFile(S9xGetFilename(".cht", CHEAT_DIR));
	S9xUnmapAllControls();
	S9xDeinitDisplay();
	Memory.Deinit();
	S9xDeinitAPU();

	exit(0);
}

int main (int argc, char **argv)
{
	if (argc < 2)
		S9xUsage();

	printf("\n\nSnes9x " VERSION " for unix\n");

	snprintf(default_dir, PATH_MAX + 1, "%s%s%s", getenv("HOME"), SLASH_STR, ".snes9x");
	s9x_base_dir = default_dir;

	memset(&Settings, 0, sizeof(Settings));
	Settings.MouseMaster = TRUE;
	Settings.SuperScopeMaster = TRUE;
	Settings.JustifierMaster = TRUE;
	Settings.MultiPlayer5Master = TRUE;
	Settings.FrameTimePAL = 20000;
	Settings.FrameTimeNTSC = 16667;
	Settings.SixteenBitSound = TRUE;
	Settings.Stereo = TRUE;
	Settings.SoundPlaybackRate = 48000;
	Settings.SoundInputRate = 31950;
	Settings.SupportHiRes = TRUE;
	Settings.Transparency = TRUE;
	Settings.AutoDisplayMessages = TRUE;
	Settings.InitialInfoStringTimeout = 120;
	Settings.HDMATimingHack = 100;
	Settings.BlockInvalidVRAMAccessMaster = TRUE;
	Settings.StopEmulation = TRUE;
	Settings.WrongMovieStateProtection = TRUE;
	Settings.DumpStreamsMaxFrames = -1;
	Settings.StretchScreenshots = 1;
	Settings.SnapshotScreenshots = TRUE;
	Settings.SkipFrames = AUTO_FRAMERATE;
	Settings.TurboSkipFrames = 15;
	Settings.CartAName[0] = 0;
	Settings.CartBName[0] = 0;
	unixSettings.JoystickEnabled = FALSE;
	unixSettings.ThreadSound = TRUE;
	unixSettings.SoundBufferSize = 100;
	unixSettings.SoundFragmentSize = 2048;

	unixSettings.rewindBufferSize = 0;
	unixSettings.rewindGranularity = 1;

	memset(&so, 0, sizeof(so));

	rewinding = false;

	CPU.Flags = 0;

	S9xLoadConfigFiles(argv, argc);
	rom_filename = S9xParseArgs(argv, argc);
	S9xDeleteCheats();

	make_snes9x_dirs();

	if (!Memory.Init() || !S9xInitAPU())
	{
		fprintf(stderr, "Snes9x: Memory allocation failure - not enough RAM/virtual memory available.\nExiting...\n");
		Memory.Deinit();
		S9xDeinitAPU();
		exit(1);
	}

	S9xInitSound(0);
	S9xSetSoundMute(TRUE);

	S9xReportControllers();

	uint32	saved_flags = CPU.Flags;
	bool8	loaded = FALSE;

	if (Settings.Multi)
	{
		loaded = Memory.LoadMultiCart(Settings.CartAName, Settings.CartBName);

		if (!loaded)
		{
			char	s1[PATH_MAX + 1], s2[PATH_MAX + 1];
			char	drive[_MAX_DRIVE + 1], dir[_MAX_DIR + 1], fname[_MAX_FNAME + 1], ext[_MAX_EXT + 1];

			s1[0] = s2[0] = 0;

			if (Settings.CartAName[0])
			{
				_splitpath(Settings.CartAName, drive, dir, fname, ext);
				snprintf(s1, PATH_MAX + 1, "%s%s%s", S9xGetDirectory(ROM_DIR), SLASH_STR, fname);
				if (ext[0] && (strlen(s1) <= PATH_MAX - 1 - strlen(ext)))
				{
					strcat(s1, ".");
					strcat(s1, ext);
				}
			}

			if (Settings.CartBName[0])
			{
				_splitpath(Settings.CartBName, drive, dir, fname, ext);
				snprintf(s2, PATH_MAX + 1, "%s%s%s", S9xGetDirectory(ROM_DIR), SLASH_STR, fname);
				if (ext[0] && (strlen(s2) <= PATH_MAX - 1 - strlen(ext)))
				{
					strcat(s2, ".");
					strcat(s2, ext);
				}
			}

			loaded = Memory.LoadMultiCart(s1, s2);
		}
	}
	else
	if (rom_filename)
	{
		loaded = Memory.LoadROM(rom_filename);

		if (!loaded && rom_filename[0])
		{
			char	s[PATH_MAX + 1];
			char	drive[_MAX_DRIVE + 1], dir[_MAX_DIR + 1], fname[_MAX_FNAME + 1], ext[_MAX_EXT + 1];

			_splitpath(rom_filename, drive, dir, fname, ext);
			snprintf(s, PATH_MAX + 1, "%s%s%s", S9xGetDirectory(ROM_DIR), SLASH_STR, fname);
			if (ext[0] && (strlen(s) <= PATH_MAX - 1 - strlen(ext)))
			{
				strcat(s, ".");
				strcat(s, ext);
			}

			loaded = Memory.LoadROM(s);
		}
	}

	if (!loaded)
	{
		fprintf(stderr, "Error opening the ROM file.\n");
		exit(1);
	}

	S9xDeleteCheats();
	S9xCheatsEnable();
	NSRTControllerSetup();
	Memory.LoadSRAM(S9xGetFilename(".srm", SRAM_DIR));

	if (Settings.ApplyCheats)
	{
		S9xLoadCheatFile(S9xGetFilename(".cht", CHEAT_DIR));
	}

	S9xParseArgsForCheats(argv, argc);

	CPU.Flags = saved_flags;
	Settings.StopEmulation = FALSE;

	S9xInitInputDevices();
	S9xInitDisplay(argc, argv);
	S9xSetupDefaultKeymap();
	S9xTextMode();

	if (play_smv_filename)
	{
		uint32	flags = CPU.Flags & (DEBUG_MODE_FLAG | TRACE_FLAG);
		if (S9xMovieOpen(play_smv_filename, TRUE) != SUCCESS)
			exit(1);
		CPU.Flags |= flags;
	}
	else
	if (record_smv_filename)
	{
		uint32	flags = CPU.Flags & (DEBUG_MODE_FLAG | TRACE_FLAG);
		if (S9xMovieCreate(record_smv_filename, 0xFF, MOVIE_OPT_FROM_RESET, NULL, 0) != SUCCESS)
			exit(1);
		CPU.Flags |= flags;
	}
	else
	{
		if (snapshot_filename)
		{
			uint32	flags = CPU.Flags & (DEBUG_MODE_FLAG | TRACE_FLAG);
			if (!S9xUnfreezeGame(snapshot_filename))
				exit(1);
			CPU.Flags |= flags;
		}
		if (unixSettings.rewindBufferSize)
		{
			stateMan.init(unixSettings.rewindBufferSize * 1024 * 1024);
		}
	}

	S9xGraphicsMode();

	sprintf(String, "\"%s\" %s: %s", Memory.ROMName, TITLE, VERSION);
	S9xSetTitle(String);
	S9xSetSoundMute(FALSE);

	// Main unix loop
	while (1)
	{
		if (!Settings.Paused)
		{
			if(rewinding)
			{
				uint16 joypads[8];
				for (int i = 0; i < 8; i++)
					joypads[i] = MovieGetJoypad(i);

				rewinding = stateMan.pop();

				for (int i = 0; i < 8; i++)
					MovieSetJoypad (i, joypads[i]);
			}
			else if(IPPU.TotalEmulatedFrames % unixSettings.rewindGranularity == 0)
				stateMan.push();

			// Before calling the main loop, log the memory we want
			LogSpecialMemory();
			S9xMainLoop();
		}
		if (Settings.Paused && frame_advance)
		{
			// This main loop call only occurs when we are paused, but also advancing the frames
			// We probably don't care about logging the memory in this case, because it should 
			// mean that the game is in "replay".
			S9xMainLoop();
			frame_advance = 0;
		}

		if (Settings.Paused)
		{
			S9xSetSoundMute(TRUE);
		}

		if (Settings.Paused)
		{
			S9xProcessEvents(FALSE);
			usleep(100000);
		}

		S9xProcessEvents(FALSE);

		if (!Settings.Paused)
		{
			S9xSetSoundMute(FALSE);
		}
	}

	return (0);
}

static void LogSpecialMemory (void)
{
	// Memory.SaveLTBBMemory("data_output.txt");
}
