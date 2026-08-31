#include "i_interface.h"
#include "st_start.h"
#include "gamestate.h"
#include "startupinfo.h"
#include "c_cvars.h"
#include "gstrings.h"
#include "version.h"

static_assert(sizeof(void*) == 8,
	"Only LP64/LLP64 builds are officially supported. "
	"Please do not attempt to build for other platforms; "
	"even if the program succeeds in a MAP01 smoke test, "
	"there are e.g. known visual artifacts "
	"<https://forum.zdoom.org/viewtopic.php?f=7&t=75673> "
	"that lead to a bad user experience.");

// Some global engine variables taken out of the backend code.
FStartupScreen* StartWindow;
SystemCallbacks sysCallbacks;
FString endoomName;
bool batchrun;
float menuBlurAmount;

bool AppActive = true;
int chatmodeon;
gamestate_t 	gamestate = GS_STARTUP;
bool ToggleFullscreen;
int 			paused;
bool			pauseext;

FStartupInfo GameStartupInfo;

CVAR(Bool, queryiwad, QUERYIWADDEFAULT, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(String, defaultiwad, "", CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, vid_fps, false, 0)

EXTERN_CVAR(Bool, ui_generic)

static const char* NormalizeNakaraLanguageCVar(const char* value)
{
	if (value == nullptr || value[0] == 0)
	{
		return "eng";
	}

	if (stricmp(value, "eng") == 0 || stricmp(value, "jp") == 0 || stricmp(value, "zh") == 0)
	{
		return nullptr;
	}

	// Preserve compatibility with older configs and common language aliases.
	if (stricmp(value, "auto") == 0 || stricmp(value, "default") == 0 ||
		stricmp(value, "en") == 0 || stricmp(value, "enu") == 0)
	{
		return "eng";
	}
	if (stricmp(value, "ja") == 0 || stricmp(value, "jpn") == 0)
	{
		return "jp";
	}
	if (stricmp(value, "chs") == 0 || stricmp(value, "zhs") == 0 ||
		stricmp(value, "cn") == 0 || stricmp(value, "zh-cn") == 0)
	{
		return "zh";
	}

	// Nakara exposes only English, Japanese, and Simplified Chinese.
	return "eng";
}

CUSTOM_CVAR(String, language, "eng", CVAR_ARCHIVE | CVAR_NOINITCALL | CVAR_GLOBALCONFIG)
{
	if (const char* normalized = NormalizeNakaraLanguageCVar(self))
	{
		self = normalized;
		return;
	}

	GStrings.UpdateLanguage(self);
	UpdateGenericUI(ui_generic);
	if (sysCallbacks.LanguageChanged) sysCallbacks.LanguageChanged(self);
}

