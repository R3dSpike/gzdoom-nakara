/*
**
**
**---------------------------------------------------------------------------
** Copyright 2005-2016 Randy Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

// HEADER FILES ------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbt.h>
#include <xinput.h>
#include <limits.h>
#include <cmath>
#include "c_cvars.h"

#include "i_input.h"
#include "d_eventbase.h"

#include "gameconfigfile.h"
#include "m_argv.h"
#include "cmdlib.h"
#include "keydef.h"


// MACROS ------------------------------------------------------------------

// This macro is defined by newer versions of xinput.h. In case we are
// compiling with an older version, define it here.
#ifndef XUSER_MAX_COUNT
#define XUSER_MAX_COUNT                 4
#endif

// MinGW
#ifndef XINPUT_DLL
#define XINPUT_DLL_A  "xinput1_3.dll"
#define XINPUT_DLL_W L"xinput1_3.dll"
#ifdef UNICODE
#define XINPUT_DLL XINPUT_DLL_W
#else
#define XINPUT_DLL XINPUT_DLL_A
#endif
#endif

extern bool AppActive;
CVAR(Bool, in_gamepadconnected, false, CVAR_NOSAVE);
EXTERN_CVAR(Bool, use_joystick);
static int g_xinputConnectedCount = 0;
static int g_pendingUseJoystick = -1;   // -1 = no change, 0/1 = apply
// Auto-toggle use_joystick based on gamepad connection state.
// Once the player manually changes use_joystick, we stop auto-toggling for the rest of the session.
static bool g_autoJoyLocked = false;
static bool g_inAutoJoyChange = false;
static int  g_lastUseJoystick = -1;


// ---------------------------------------------------------------------------
// XInput rumble (custom fork)
//
// ZScript/game logic should pass duration in tics (35 tics = 1 second).
// We convert tics -> milliseconds internally because XInput is time-based.
// ---------------------------------------------------------------------------

CVAR(Bool, joy_rumble, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, joy_rumble_strength, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

static inline WORD ClampRumble(int v)
{
	if (v <= 0) return 0;
	if (v >= 65535) return 65535;
	return (WORD)v;
}

static inline int TicsToMs(int tics)
{
	if (tics <= 0) return 0;
	// 35 tics == 1 second
	return (int)round(tics * (1000.0 / 35.0));
}


// TYPES -------------------------------------------------------------------

typedef DWORD(WINAPI* XInputGetStateType)(DWORD index, XINPUT_STATE* state);
typedef DWORD(WINAPI* XInputSetStateType)(DWORD index, XINPUT_VIBRATION* vibration);
typedef DWORD(WINAPI* XInputGetCapabilitiesType)(DWORD index, DWORD flags, XINPUT_CAPABILITIES* caps);
typedef void  (WINAPI* XInputEnableType)(BOOL enable);

class FXInputController : public IJoystickConfig
{
public:
	FXInputController(int index);
	~FXInputController();

	void ProcessInput();
	void AddAxes(float axes[NUM_JOYAXIS]);
	bool IsConnected() { return Connected; }

	// IJoystickConfig interface
	FString GetName();
	float GetSensitivity();
	virtual void SetSensitivity(float scale);

	int GetNumAxes();
	float GetAxisDeadZone(int axis);
	EJoyAxis GetAxisMap(int axis);
	const char* GetAxisName(int axis);
	float GetAxisScale(int axis);

	void SetAxisDeadZone(int axis, float deadzone);
	void SetAxisMap(int axis, EJoyAxis gameaxis);
	void SetAxisScale(int axis, float scale);

	bool IsSensitivityDefault();
	bool IsAxisDeadZoneDefault(int axis);
	bool IsAxisMapDefault(int axis);
	bool IsAxisScaleDefault(int axis);

	bool GetEnabled();
	void SetEnabled(bool enabled);

	bool AllowsEnabledInBackground() { return true; }
	bool GetEnabledInBackground() { return EnabledInBackground; }
	void SetEnabledInBackground(bool enabled) { EnabledInBackground = enabled; }

	void SetDefaultConfig();
	FString GetIdentifier();

protected:
	struct AxisInfo
	{
		float Value;
		float DeadZone;
		float Multiplier;
		EJoyAxis GameAxis;
		uint8_t ButtonValue;
	};
	struct DefaultAxisConfig
	{
		float DeadZone;
		EJoyAxis GameAxis;
		float Multiplier;
	};
	enum
	{
		AXIS_ThumbLX,
		AXIS_ThumbLY,
		AXIS_ThumbRX,
		AXIS_ThumbRY,
		AXIS_LeftTrigger,
		AXIS_RightTrigger,
		NUM_AXES
	};

	int Index;
	float Multiplier;
	AxisInfo Axes[NUM_AXES];
	static DefaultAxisConfig DefaultAxes[NUM_AXES];
	DWORD LastPacketNumber;
	int LastButtons;
	bool Connected;
	bool Enabled;
	bool EnabledInBackground;

	void Attached();
	void Detached();

	static void ProcessThumbstick(int value1, AxisInfo* axis1, int value2, AxisInfo* axis2, int base);
	static void ProcessTrigger(int value, AxisInfo* axis, int base);
};

class FXInputManager : public FJoystickCollection
{
public:
	FXInputManager();
	~FXInputManager();

	bool GetDevice();
	void ProcessInput();
	bool WndProcHook(HWND hWnd, uint32_t message, WPARAM wParam, LPARAM lParam, LRESULT* result);
	void AddAxes(float axes[NUM_JOYAXIS]);
	void GetDevices(TArray<IJoystickConfig*>& sticks);
	IJoystickConfig* Rescan();

	// Request XInput rumble for a controller.
	// - userIndex: 0..3 (XInput user index)
	// - low/high: 0..65535 motor strengths (will be scaled by joy_rumble_strength)
	// - tics: duration in game tics (35 tics = 1 second)
	void RequestRumbleTics(int userIndex, int low, int high, int tics);

protected:
	struct RumbleState
	{
		WORD Low = 0;
		WORD High = 0;
		ULONGLONG EndTimeMs = 0;
		bool Active = false;
	};

	void UpdateRumble();

	HMODULE XInputDLL;
	FXInputController* Devices[XUSER_MAX_COUNT];
	RumbleState Rumble[XUSER_MAX_COUNT];
};

// EXTERNAL FUNCTION PROTOTYPES --------------------------------------------

// PUBLIC FUNCTION PROTOTYPES ----------------------------------------------

// PRIVATE FUNCTION PROTOTYPES ---------------------------------------------

// EXTERNAL DATA DECLARATIONS ----------------------------------------------

// PUBLIC DATA DEFINITIONS -------------------------------------------------

CUSTOM_CVAR(Bool, joy_xinput, true, CVAR_GLOBALCONFIG | CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	I_StartupXInput();
	event_t ev = { EV_DeviceChange };
	D_PostEvent(&ev);
}

// PRIVATE DATA DEFINITIONS ------------------------------------------------

static XInputGetStateType			InputGetState;
static XInputSetStateType			InputSetState;
static XInputGetCapabilitiesType	InputGetCapabilities;
static XInputEnableType				InputEnable;

static void I_ApplyPendingUseJoystick()
{
	// Detect manual changes to use_joystick and lock out auto-toggling for this session.
	// (We can't easily hook the CVAR's setter from here, so we watch for changes.)
	if (g_lastUseJoystick < 0)
	{
		g_lastUseJoystick = use_joystick ? 1 : 0;
	}
	else if (!g_inAutoJoyChange)
	{
		const int now = use_joystick ? 1 : 0;
		if (now != g_lastUseJoystick)
		{
			g_autoJoyLocked = true;
			g_lastUseJoystick = now;
		}
	}

	if (g_pendingUseJoystick < 0) return;
	if (g_autoJoyLocked)
	{
		g_pendingUseJoystick = -1;
		return;
	}

	const bool want = (g_pendingUseJoystick != 0);
	g_pendingUseJoystick = -1;

	if (use_joystick != want)
	{
		g_inAutoJoyChange = true;
		use_joystick = want;
		g_inAutoJoyChange = false;
		g_lastUseJoystick = want ? 1 : 0;
		Printf(TEXTCOLOR_GREEN "[Input] use_joystick = %d\n", want);
	}
}
static const char* AxisNames[] =
{
	"Left Thumb X Axis",
	"Left Thumb Y Axis",
	"Right Thumb X Axis",
	"Right Thumb Y Axis",
	"Left Trigger",
	"Right Trigger"
};
static bool I_HasGamepadEnvironment()
{
	auto cv = FindCVar("g_steamdeck", nullptr);
	if (cv && cv->ToInt() == 1)
		return true;

	return in_gamepadconnected;
}
static void I_SyncUseJoystickToGamepadHint(bool connected)
{
	if (connected)
	{
		if (!use_joystick) return;

		use_joystick = true;
	}
	else
	{
		use_joystick = false;
	}
}
FXInputController::DefaultAxisConfig FXInputController::DefaultAxes[NUM_AXES] =
{
	// Dead zone, game axis, multiplier
	{ XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE / 32768.f, JOYAXIS_Side, 1 },		// ThumbLX
	{ XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE / 32768.f, JOYAXIS_Forward, 1 },	// ThumbLY
	{ XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE / 32768.f, JOYAXIS_Yaw, 1 },		// ThumbRX
	{ XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE / 32768.f, JOYAXIS_Pitch, 0.75 },	// ThumbRY
	{ XINPUT_GAMEPAD_TRIGGER_THRESHOLD / 256.f, JOYAXIS_None, 0 },			// LeftTrigger
	{ XINPUT_GAMEPAD_TRIGGER_THRESHOLD / 256.f, JOYAXIS_None, 0 }			// RightTrigger
};

// CODE --------------------------------------------------------------------

//==========================================================================
//
// FXInputController - Constructor
//
//==========================================================================

FXInputController::FXInputController(int index)
{
	Index = index;
	Connected = false;
	Enabled = true;
	M_LoadJoystickConfig(this);
}

//==========================================================================
//
// FXInputController - Destructor
//
//==========================================================================

FXInputController::~FXInputController()
{
	// Send button up events before destroying this.
	ProcessThumbstick(0, &Axes[AXIS_ThumbLX], 0, &Axes[AXIS_ThumbLY], KEY_PAD_LTHUMB_RIGHT);
	ProcessThumbstick(0, &Axes[AXIS_ThumbRX], 0, &Axes[AXIS_ThumbRY], KEY_PAD_RTHUMB_RIGHT);
	ProcessTrigger(0, &Axes[AXIS_LeftTrigger], KEY_PAD_LTRIGGER);
	ProcessTrigger(0, &Axes[AXIS_RightTrigger], KEY_PAD_RTRIGGER);
	Joy_GenerateButtonEvents(LastButtons, 0, 16, KEY_PAD_DPAD_UP);
	M_SaveJoystickConfig(this);
}

//==========================================================================
//
// FXInputController :: ProcessInput
//
//==========================================================================

void FXInputController::ProcessInput()
{
	DWORD res;
	XINPUT_STATE state;
	I_ApplyPendingUseJoystick();
	res = InputGetState(Index, &state);
	if (res == ERROR_DEVICE_NOT_CONNECTED)
	{
		if (Connected)
		{
			Detached();
		}
		return;
	}
	if (res != ERROR_SUCCESS)
	{
		return;
	}
	if (!Connected)
	{
		Attached();
	}
	if (!use_joystick)
	{
		LastPacketNumber = state.dwPacketNumber;
		LastButtons = state.Gamepad.wButtons;
		return;
	}

	if (state.dwPacketNumber == LastPacketNumber || !Enabled)
	{ // Nothing has changed since last time.
		return;
	}

	// There is a hole in the wButtons bitmask where two buttons could fit.
	// As per the XInput documentation, "bits that are set but not defined ... are reserved,
	// and their state is undefined," so we clear them to make sure they're not set.
	// Our keymapping uses these two slots for the triggers as buttons.
	state.Gamepad.wButtons &= 0xF3FF;

	// Convert axes to floating point and cancel out deadzones.
	// XInput's Y axes are reversed compared to DirectInput.
	ProcessThumbstick(state.Gamepad.sThumbLX, &Axes[AXIS_ThumbLX],
		-state.Gamepad.sThumbLY, &Axes[AXIS_ThumbLY], KEY_PAD_LTHUMB_RIGHT);
	ProcessThumbstick(state.Gamepad.sThumbRX, &Axes[AXIS_ThumbRX],
		-state.Gamepad.sThumbRY, &Axes[AXIS_ThumbRY], KEY_PAD_RTHUMB_RIGHT);
	ProcessTrigger(state.Gamepad.bLeftTrigger, &Axes[AXIS_LeftTrigger], KEY_PAD_LTRIGGER);
	ProcessTrigger(state.Gamepad.bRightTrigger, &Axes[AXIS_RightTrigger], KEY_PAD_RTRIGGER);

	// Generate events for buttons that have changed.

	int buttons = state.Gamepad.wButtons;
	buttons &= ~XINPUT_GAMEPAD_START;

	Joy_GenerateButtonEvents(LastButtons, buttons, 16, KEY_PAD_DPAD_UP);


	const int prevStart = (LastButtons & XINPUT_GAMEPAD_START) != 0;
	const int nowStart = (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;

	if (prevStart != nowStart)
	{
		const int key = I_IsGUICaptureActive() ? KEY_PAD_B : KEY_ESCAPE;

		event_t ev;
		ev.type = nowStart ? EV_KeyDown : EV_KeyUp;
		ev.data1 = key;
		ev.data2 = 0;
		ev.data3 = 0;
		D_PostEvent(&ev);
	}

	const int prevBack = (LastButtons & XINPUT_GAMEPAD_BACK) != 0;
	const int nowBack = (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;

	if (prevBack != nowBack)
	{
		// Map the controller BACK button to the keyboard TAB key.
		const int key = KEY_TAB;

		event_t ev;
		ev.type = nowBack ? EV_KeyDown : EV_KeyUp;
		ev.data1 = key;
		ev.data2 = 0;
		ev.data3 = 0;
		D_PostEvent(&ev);
	}
	LastPacketNumber = state.dwPacketNumber;
	LastButtons = state.Gamepad.wButtons;

}

//==========================================================================
//
// FXInputController :: ProcessThumbstick							STATIC
//
// Converts both axes of a thumb stick to floating point, cancels out the
// deadzone, and generates button up/down events for them.
//
//==========================================================================

void FXInputController::ProcessThumbstick(int value1, AxisInfo* axis1,
	int value2, AxisInfo* axis2, int base)
{
	uint8_t buttonstate;
	double axisval1, axisval2;

	axisval1 = (value1 - SHRT_MIN) * 2.0 / 65536 - 1.0;
	axisval2 = (value2 - SHRT_MIN) * 2.0 / 65536 - 1.0;
	axisval1 = Joy_RemoveDeadZone(axisval1, axis1->DeadZone, NULL);
	axisval2 = Joy_RemoveDeadZone(axisval2, axis2->DeadZone, NULL);
	axis1->Value = float(axisval1);
	axis2->Value = float(axisval2);

	// We store all four buttons in the first axis and ignore the second.
	buttonstate = Joy_XYAxesToButtons(axisval1, axisval2);
	Joy_GenerateButtonEvents(axis1->ButtonValue, buttonstate, 4, base);
	axis1->ButtonValue = buttonstate;
}

//==========================================================================
//
// FXInputController :: ProcessTrigger								STATIC
//
// Much like ProcessThumbstick, except triggers only go in the positive
// direction and have less precision.
//
//==========================================================================

void FXInputController::ProcessTrigger(int value, AxisInfo* axis, int base)
{
	uint8_t buttonstate;
	double axisval;

	axisval = Joy_RemoveDeadZone(value / 256.0, axis->DeadZone, &buttonstate);
	Joy_GenerateButtonEvents(axis->ButtonValue, buttonstate, 1, base);
	axis->ButtonValue = buttonstate;
	axis->Value = float(axisval);
}

//==========================================================================
//
// FXInputController :: Attached
//
// This controller was just attached. Set all buttons and axes to 0.
//
//==========================================================================


void FXInputController::Attached()
{
	int i;

	Connected = true;
	LastPacketNumber = ~0;
	LastButtons = 0;

	for (i = 0; i < NUM_AXES; ++i)
	{
		Axes[i].Value = 0;
		Axes[i].ButtonValue = 0;
	}

	UpdateJoystickMenu(this);

	if (g_xinputConnectedCount < XUSER_MAX_COUNT) g_xinputConnectedCount++;
	in_gamepadconnected = (g_xinputConnectedCount > 0);

	g_pendingUseJoystick = 1;
}

//==========================================================================
//
// FXInputController :: Detached
//
// This controller was just detached. Send button ups for buttons that
// were pressed the last time we got input from it.
//
//==========================================================================

void FXInputController::Detached()
{
	int i;

	Connected = false;

	for (i = 0; i < 4; i += 2)
	{
		ProcessThumbstick(0, &Axes[i], 0, &Axes[i + 1], KEY_PAD_LTHUMB_RIGHT + i * 2);
	}
	for (i = 0; i < 2; ++i)
	{
		ProcessTrigger(0, &Axes[4 + i], KEY_PAD_LTRIGGER + i);
	}

	Joy_GenerateButtonEvents(LastButtons, 0, 16, KEY_PAD_DPAD_UP);
	LastButtons = 0;

	UpdateJoystickMenu(NULL);

	if (g_xinputConnectedCount > 0) g_xinputConnectedCount--;
	in_gamepadconnected = (g_xinputConnectedCount > 0);

	if (!in_gamepadconnected)
	{
		g_pendingUseJoystick = 0;
	}
}
//==========================================================================
//
// FXInputController :: AddAxes
//
// Add the values of each axis to the game axes.
//
//==========================================================================

void FXInputController::AddAxes(float axes[NUM_JOYAXIS])
{
	// Add to game axes.
	for (int i = 0; i < NUM_AXES; ++i)
	{
		axes[Axes[i].GameAxis] -= float(Axes[i].Value * Multiplier * Axes[i].Multiplier);
	}
}

//==========================================================================
//
// FXInputController :: SetDefaultConfig
//
//==========================================================================

void FXInputController::SetDefaultConfig()
{
	Multiplier = 1;
	for (int i = 0; i < NUM_AXES; ++i)
	{
		Axes[i].DeadZone = DefaultAxes[i].DeadZone;
		Axes[i].GameAxis = DefaultAxes[i].GameAxis;
		Axes[i].Multiplier = DefaultAxes[i].Multiplier;
	}
}

//==========================================================================
//
// FXInputController :: GetIdentifier
//
//==========================================================================

FString FXInputController::GetIdentifier()
{
	return FStringf("XI:%d", Index);
}

//==========================================================================
//
// FXInputController :: GetName
//
//==========================================================================

FString FXInputController::GetName()
{
	FString res;
	res.Format("XInput Controller #%d", Index + 1);
	return res;
}

//==========================================================================
//
// FXInputController :: GetSensitivity
//
//==========================================================================

float FXInputController::GetSensitivity()
{
	return Multiplier;
}

//==========================================================================
//
// FXInputController :: SetSensitivity
//
//==========================================================================

void FXInputController::SetSensitivity(float scale)
{
	Multiplier = scale;
}

//==========================================================================
//
// FXInputController :: IsSensitivityDefault
//
//==========================================================================

bool FXInputController::IsSensitivityDefault()
{
	return Multiplier == 1;
}

//==========================================================================
//
// FXInputController :: GetNumAxes
//
//==========================================================================

int FXInputController::GetNumAxes()
{
	return NUM_AXES;
}

//==========================================================================
//
// FXInputController :: GetAxisDeadZone
//
//==========================================================================

float FXInputController::GetAxisDeadZone(int axis)
{
	if (unsigned(axis) < NUM_AXES)
	{
		return Axes[axis].DeadZone;
	}
	return 0;
}

//==========================================================================
//
// FXInputController :: GetAxisMap
//
//==========================================================================

EJoyAxis FXInputController::GetAxisMap(int axis)
{
	if (unsigned(axis) < NUM_AXES)
	{
		return Axes[axis].GameAxis;
	}
	return JOYAXIS_None;
}

//==========================================================================
//
// FXInputController :: GetAxisName
//
//==========================================================================

const char* FXInputController::GetAxisName(int axis)
{
	if (unsigned(axis) < NUM_AXES)
	{
		return AxisNames[axis];
	}
	return "Invalid";
}

//==========================================================================
//
// FXInputController :: GetAxisScale
//
//==========================================================================

float FXInputController::GetAxisScale(int axis)
{
	if (unsigned(axis) < NUM_AXES)
	{
		return Axes[axis].Multiplier;
	}
	return 0;
}

//==========================================================================
//
// FXInputController :: SetAxisDeadZone
//
//==========================================================================

void FXInputController::SetAxisDeadZone(int axis, float deadzone)
{
	if (unsigned(axis) < NUM_AXES)
	{
		Axes[axis].DeadZone = clamp(deadzone, 0.f, 1.f);
	}
}

//==========================================================================
//
// FXInputController :: SetAxisMap
//
//==========================================================================

void FXInputController::SetAxisMap(int axis, EJoyAxis gameaxis)
{
	if (unsigned(axis) < NUM_AXES)
	{
		Axes[axis].GameAxis = (unsigned(gameaxis) < NUM_JOYAXIS) ? gameaxis : JOYAXIS_None;
	}
}

//==========================================================================
//
// FXInputController :: SetAxisScale
//
//==========================================================================

void FXInputController::SetAxisScale(int axis, float scale)
{
	if (unsigned(axis) < NUM_AXES)
	{
		Axes[axis].Multiplier = scale;
	}
}

//===========================================================================
//
// FXInputController :: IsAxisDeadZoneDefault
//
//===========================================================================

bool FXInputController::IsAxisDeadZoneDefault(int axis)
{
	if (unsigned(axis) < NUM_AXES)
	{
		return Axes[axis].DeadZone == DefaultAxes[axis].DeadZone;
	}
	return true;
}

//===========================================================================
//
// FXInputController :: IsAxisScaleDefault
//
//===========================================================================

bool FXInputController::IsAxisScaleDefault(int axis)
{
	if (unsigned(axis) < NUM_AXES)
	{
		return Axes[axis].Multiplier == DefaultAxes[axis].Multiplier;
	}
	return true;
}

//===========================================================================
//
// FXInputController :: GetEnabled
//
//===========================================================================

bool FXInputController::GetEnabled()
{
	return Enabled;
}

//===========================================================================
//
// FXInputController :: SetEnabled
//
//===========================================================================

void FXInputController::SetEnabled(bool enabled)
{
	Enabled = enabled;
}

//===========================================================================
//
// FXInputController :: IsAxisMapDefault
//
//===========================================================================

bool FXInputController::IsAxisMapDefault(int axis)
{
	if (unsigned(axis) < NUM_AXES)
	{
		return Axes[axis].GameAxis == DefaultAxes[axis].GameAxis;
	}
	return true;
}

//==========================================================================
//
// FXInputManager - Constructor
//
//==========================================================================

FXInputManager::FXInputManager()
{
	XInputDLL = LoadLibrary(XINPUT_DLL);
	if (XInputDLL != NULL)
	{
		InputGetState = (XInputGetStateType)GetProcAddress(XInputDLL, "XInputGetState");
		InputSetState = (XInputSetStateType)GetProcAddress(XInputDLL, "XInputSetState");
		InputGetCapabilities = (XInputGetCapabilitiesType)GetProcAddress(XInputDLL, "XInputGetCapabilities");
		InputEnable = (XInputEnableType)GetProcAddress(XInputDLL, "XInputEnable");
		// Treat XInputEnable() function as optional
		// It is not available in xinput9_1_0.dll which is XINPUT_DLL in modern SDKs
		// See https://msdn.microsoft.com/en-us/library/windows/desktop/hh405051(v=vs.85).aspx
		if (InputGetState == NULL || InputSetState == NULL || InputGetCapabilities == NULL)
		{
			FreeLibrary(XInputDLL);
			XInputDLL = NULL;
		}
	}
	for (int i = 0; i < XUSER_MAX_COUNT; ++i)
	{
		Devices[i] = (XInputDLL != NULL) ? new FXInputController(i) : NULL;
	}

	g_xinputConnectedCount = 0;
	in_gamepadconnected = false;

	for (int i = 0; i < XUSER_MAX_COUNT; ++i)
	{
		if (Devices[i] != NULL)
		{
			Devices[i]->ProcessInput();
		}
	}
	// If a pad was already connected at startup, the first ProcessInput() call will set
	// g_pendingUseJoystick but it won't apply until the next tick. Apply it now.
	I_ApplyPendingUseJoystick();
}

//==========================================================================
//
// FXInputManager - Destructor
//
//==========================================================================

FXInputManager::~FXInputManager()
{
	for (int i = 0; i < XUSER_MAX_COUNT; ++i)
	{
		if (Devices[i] != NULL)
		{
			delete Devices[i];
		}
	}
	if (XInputDLL != NULL)
	{
		FreeLibrary(XInputDLL);
	}
}

//==========================================================================
//
// FXInputManager :: GetDevice
//
//==========================================================================

bool FXInputManager::GetDevice()
{
	return (XInputDLL != NULL);
}


//===========================================================================
//
// FXInputManager :: RequestRumbleTics
//
//===========================================================================

void FXInputManager::RequestRumbleTics(int userIndex, int low, int high, int tics)
{
	if (!joy_rumble) return;
	if (XInputDLL == NULL || InputSetState == NULL) return;
	if (userIndex < 0 || userIndex >= XUSER_MAX_COUNT) return;
	if (tics <= 0) return;

	// Scale by user option
	float s = joy_rumble_strength;
	if (s < 0.f) s = 0.f;
	// no hard cap, but clamp final motor strengths

	const double msPerTic = 1000.0 / 35.0;
	int durationMs = (int)llround(tics * msPerTic);
	if (durationMs < 1) durationMs = 1;

	WORD lo = ClampRumble((int)llround(low * s));
	WORD hi = ClampRumble((int)llround(high * s));

	ULONGLONG now = GetTickCount64();
	ULONGLONG endTime = now + (ULONGLONG)durationMs;

	// Merge with existing rumble: max strength + extend time
	RumbleState& rs = Rumble[userIndex];
	rs.Low = max(rs.Low, lo);
	rs.High = max(rs.High, hi);
	rs.EndTimeMs = max(rs.EndTimeMs, endTime);
	rs.Active = (rs.Low != 0 || rs.High != 0);

	if (rs.Active)
	{
		XINPUT_VIBRATION vib;
		vib.wLeftMotorSpeed = rs.Low;
		vib.wRightMotorSpeed = rs.High;
		InputSetState((DWORD)userIndex, &vib);
	}
}

//===========================================================================
//
// FXInputManager :: UpdateRumble
//
//===========================================================================

void FXInputManager::UpdateRumble()
{
	if (XInputDLL == NULL || InputSetState == NULL) return;

	ULONGLONG now = GetTickCount64();

	for (int i = 0; i < XUSER_MAX_COUNT; ++i)
	{
		RumbleState& rs = Rumble[i];
		if (!rs.Active) continue;

		if (now >= rs.EndTimeMs)
		{
			rs.Active = false;
			rs.Low = 0;
			rs.High = 0;
			rs.EndTimeMs = 0;

			XINPUT_VIBRATION vib = {};
			InputSetState((DWORD)i, &vib);
		}
	}
}

//==========================================================================
//
// FXInputManager :: ProcessInput
//
// Process input for every attached device.
//
//==========================================================================

void FXInputManager::ProcessInput()
{
	for (int i = 0; i < XUSER_MAX_COUNT; ++i)
	{
		if (AppActive || Devices[i]->GetEnabledInBackground())
		{
			Devices[i]->ProcessInput();
		}
	}
	UpdateRumble();
}

//===========================================================================
//
// FXInputManager :: AddAxes
//
// Adds the state of all attached device axes to the passed array.
//
//===========================================================================

void FXInputManager::AddAxes(float axes[NUM_JOYAXIS])
{
	for (int i = 0; i < XUSER_MAX_COUNT; ++i)
	{
		if (Devices[i]->IsConnected())
		{
			Devices[i]->AddAxes(axes);
		}
	}
}

//===========================================================================
//
// FXInputManager :: GetJoysticks
//
// Adds the IJoystick interfaces for each device we created to the sticks
// array, if they are detected as connected.
//
//===========================================================================

void FXInputManager::GetDevices(TArray<IJoystickConfig*>& sticks)
{
	for (int i = 0; i < XUSER_MAX_COUNT; ++i)
	{
		if (Devices[i]->IsConnected())
		{
			sticks.Push(Devices[i]);
		}
	}
}

//===========================================================================
//
// FXInputManager :: WndProcHook
//
// Enable and disable XInput as our window is (de)activated.
//
//===========================================================================

bool FXInputManager::WndProcHook(HWND hWnd, uint32_t message, WPARAM wParam, LPARAM lParam, LRESULT* result)
{
	if (nullptr != InputEnable && message == WM_ACTIVATE)
	{
		if (LOWORD(wParam) == WA_INACTIVE)
		{
			InputEnable(FALSE);
		}
		else
		{
			InputEnable(TRUE);
		}
	}

	// Hotplug detection even when use_joystick is off:
	// Windows will send WM_DEVICECHANGE on (un)plug. On those events, rescan XInput once
	// so FXInputController::Attached/Detached can update in_gamepadconnected and request
	// auto-enabling use_joystick (if not locked by the user).
	if (message == WM_DEVICECHANGE)
	{
		if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE || wParam == DBT_DEVNODES_CHANGED)
		{
			for (int i = 0; i < XUSER_MAX_COUNT; ++i)
			{
				if (Devices[i] != NULL)
				{
					Devices[i]->ProcessInput();
				}
			}
			I_ApplyPendingUseJoystick();
			event_t ev = { EV_DeviceChange };
			D_PostEvent(&ev);
		}
	}
	return false;
}

//===========================================================================
//
// FXInputManager :: Rescan
//
//===========================================================================

IJoystickConfig* FXInputManager::Rescan()
{
	return NULL;
}

//===========================================================================
//
// I_StartupXInput
//
//===========================================================================



//===========================================================================
//
// I_XInputRumbleTics
//
// External helper for game code (e.g. ZScript native binding) to request rumble.
// Duration is in game tics (35 tics = 1 second).
//
// Returns false if XInput is unavailable or rumble is disabled.
//
//===========================================================================

bool I_XInputRumbleTics(int userIndex, int low, int high, int tics)
{
	if (!joy_rumble) return false;
	if (JoyDevices[INPUT_XInput] == NULL) return false;

	auto mgr = static_cast<FXInputManager*>(JoyDevices[INPUT_XInput]);
	if (!mgr->GetDevice()) return false;

	mgr->RequestRumbleTics(userIndex, low, high, tics);
	return true;
}

void I_StartupXInput()
{
	// Keep XInput alive even when use_joystick is 0 so we can detect hotplug
	// and (if allowed) auto-enable it when a gamepad is present.
	if (!joy_xinput || Args->CheckParm("-nojoy"))
	{
		if (JoyDevices[INPUT_XInput] != NULL)
		{
			delete JoyDevices[INPUT_XInput];
			JoyDevices[INPUT_XInput] = NULL;
			UpdateJoystickMenu(NULL);
		}
	}
	else
	{
		if (JoyDevices[INPUT_XInput] == NULL)
		{
			FJoystickCollection* joys = new FXInputManager;
			if (joys->GetDevice())
			{
				JoyDevices[INPUT_XInput] = joys;
			}
			else
			{
				delete joys;
			}
		}
	}
}

