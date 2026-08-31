/*
** joystickmenu.cpp
** The joystick configuration menus
**
**---------------------------------------------------------------------------
** Copyright 2010 Christoph Oelckers
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

#include "menu.h"
#include "m_joy.h"
#include "vm.h"

static TArray<IJoystickConfig *> Joysticks;

DEFINE_ACTION_FUNCTION(IJoystickConfig, GetSensitivity)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	ACTION_RETURN_FLOAT(self->GetSensitivity());
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, SetSensitivity)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	PARAM_FLOAT(sens);
	self->SetSensitivity((float)sens);
	return 0;
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, GetAxisScale)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	PARAM_INT(axis);
	ACTION_RETURN_FLOAT(self->GetAxisScale(axis));
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, SetAxisScale)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	PARAM_INT(axis);
	PARAM_FLOAT(sens);
	self->SetAxisScale(axis, (float)sens);
	return 0;
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, GetAxisDeadZone)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	PARAM_INT(axis);

	// Axis -1 is reserved by Nakara's simplified gamepad menu. It represents
	// one shared dead-zone value for every axis that is mapped to a game axis.
	// Unmapped axes (XInput triggers by default) keep their own threshold.
	if (axis == -1)
	{
		for (int i = 0; i < self->GetNumAxes(); ++i)
		{
			if (self->GetAxisMap(i) != JOYAXIS_None)
			{
				ACTION_RETURN_FLOAT(self->GetAxisDeadZone(i));
			}
		}
		ACTION_RETURN_FLOAT(0.f);
	}

	ACTION_RETURN_FLOAT(self->GetAxisDeadZone(axis));
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, SetAxisDeadZone)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	PARAM_INT(axis);
	PARAM_FLOAT(dz);

	if (axis == -1)
	{
		for (int i = 0; i < self->GetNumAxes(); ++i)
		{
			if (self->GetAxisMap(i) != JOYAXIS_None)
			{
				self->SetAxisDeadZone(i, (float)dz);
			}
		}
		return 0;
	}

	self->SetAxisDeadZone(axis, (float)dz);
	return 0;
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, GetAxisMap)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	PARAM_INT(axis);
	ACTION_RETURN_INT(self->GetAxisMap(axis));
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, SetAxisMap)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	PARAM_INT(axis);
	PARAM_INT(map);
	self->SetAxisMap(axis, (EJoyAxis)map);
	return 0;
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, GetName)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	ACTION_RETURN_STRING(self->GetName());
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, GetAxisName)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	PARAM_INT(axis);
	ACTION_RETURN_STRING(self->GetAxisName(axis));
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, GetNumAxes)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	ACTION_RETURN_INT(self->GetNumAxes());
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, GetEnabled)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	ACTION_RETURN_BOOL(self->GetEnabled());
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, SetEnabled)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	PARAM_BOOL(enabled);
	self->SetEnabled(enabled);
	return 0;
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, AllowsEnabledInBackground)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	ACTION_RETURN_BOOL(self->AllowsEnabledInBackground());
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, GetEnabledInBackground)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	ACTION_RETURN_BOOL(self->GetEnabledInBackground());
}

DEFINE_ACTION_FUNCTION(IJoystickConfig, SetEnabledInBackground)
{
	PARAM_SELF_STRUCT_PROLOGUE(IJoystickConfig);
	PARAM_BOOL(enabled);
	self->SetEnabledInBackground(enabled);
	return 0;
}


static DMenuItemBase *CreateGamepadCVarOption(const char *label, FName cvar, FName values)
{
	auto c = PClass::FindClass("OptionMenuItemOption");
	if (c == nullptr) return nullptr;

	auto p = c->CreateNew();
	FString labelstr = label;
	TArray<VMValue> params;
	params.Push(p);
	params.Push(&labelstr);
	params.Push(cvar.GetIndex());
	params.Push(values.GetIndex());
	auto f = dyn_cast<PFunction>(c->FindSymbol("Init", false));
	if (f == nullptr) return nullptr;
	VMCallWithDefaults(f->Variants[0].Implementation, params, nullptr, 0);
	return (DMenuItemBase *)p;
}

static DMenuItemBase *CreateGamepadSensitivitySlider(const char *label, IJoystickConfig *joy)
{
	auto c = PClass::FindClass("OptionMenuSliderJoySensitivity");
	if (c == nullptr) return nullptr;

	auto p = c->CreateNew();
	FString labelstr = label;
	VMValue params[] = { p, &labelstr, 0.0, 2.0, 0.1, 3, joy };
	auto f = dyn_cast<PFunction>(c->FindSymbol("Init", false));
	if (f == nullptr) return nullptr;
	VMCall(f->Variants[0].Implementation, params, countof(params), nullptr, 0);
	return (DMenuItemBase *)p;
}

static DMenuItemBase *CreateGamepadDeadZoneSlider(const char *label, IJoystickConfig *joy)
{
	auto c = PClass::FindClass("OptionMenuSliderJoyDeadZone");
	if (c == nullptr) return nullptr;

	auto p = c->CreateNew();
	FString labelstr = label;
	// Axis -1 is handled by the native Get/SetAxisDeadZone wrappers above and
	// means one shared dead-zone value for all mapped gamepad axes.
	VMValue params[] = { p, &labelstr, -1, 0.0, 0.9, 0.05, 3, joy };
	auto f = dyn_cast<PFunction>(c->FindSymbol("Init", false));
	if (f == nullptr) return nullptr;
	VMCall(f->Variants[0].Implementation, params, countof(params), nullptr, 0);
	return (DMenuItemBase *)p;
}

static void AddGamepadMenuItem(DOptionMenuDescriptor *menu, DMenuItemBase *item)
{
	if (menu == nullptr || item == nullptr) return;
	GC::WriteBarrier(menu, item);
	menu->mItems.Push(item);
}

static bool IsXInputController(IJoystickConfig *joy)
{
	if (joy == nullptr) return false;
	FString id = joy->GetIdentifier();
	const char *text = id.GetChars();
	return id.Len() >= 3 && text[0] == 'X' && text[1] == 'I' && text[2] == ':';
}

static IJoystickConfig *PickGamepadMenuController(IJoystickConfig *selected)
{
	// Prefer XInput even if DirectInput exposes the same physical pad too.
	// This also keeps the Rumble toggle and the configured device aligned.
	if (IsXInputController(selected)) return selected;

	for (unsigned i = 0; i < Joysticks.Size(); ++i)
	{
		if (IsXInputController(Joysticks[i])) return Joysticks[i];
	}

	if (selected != nullptr)
	{
		for (unsigned i = 0; i < Joysticks.Size(); ++i)
		{
			if (Joysticks[i] == selected) return selected;
		}
	}

	return Joysticks.Size() > 0 ? Joysticks[0] : nullptr;
}

static void UpdateGamepadOptionsEntryLabel()
{
	DMenuDescriptor **desc = MenuDescriptors.CheckKey("OptionsMenu");
	if (desc == nullptr || *desc == nullptr || !(*desc)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor))) return;

	auto opt = (DOptionMenuDescriptor *)*desc;
	for (unsigned i = 0; i < opt->mItems.Size(); ++i)
	{
		auto item = opt->mItems[i];
		if (item != nullptr && item->mAction == NAME_JoystickOptions)
		{
			auto replacement = CreateOptionMenuItemSubmenu("$GMPD_TTLE", NAME_JoystickOptions, 0);
			if (replacement != nullptr)
			{
				GC::WriteBarrier(opt, replacement);
				opt->mItems[i] = replacement;
			}
			break;
		}
	}
}

void UpdateJoystickMenu(IJoystickConfig *selected)
{
	DMenuDescriptor **desc = MenuDescriptors.CheckKey(NAME_JoystickOptions);
	if (desc == nullptr || *desc == nullptr || !(*desc)->IsKindOf(RUNTIME_CLASS(DOptionMenuDescriptor))) return;

	auto opt = (DOptionMenuDescriptor *)*desc;
	I_GetJoysticks(Joysticks);
	IJoystickConfig *joy = PickGamepadMenuController(selected);

	UpdateGamepadOptionsEntryLabel();

	opt->mTitle = "$GMPD_TTLE";
	opt->mItems.Clear();

#ifdef _WIN32
	// Rumble is the existing XInput CVAR. No separate strength control is
	// exposed here; the simplified menu intentionally keeps one on/off item.
	AddGamepadMenuItem(opt, CreateGamepadCVarOption("$GMPD_RUMBLE", "joy_rumble", "OnOff"));
#endif

	if (joy != nullptr)
	{
		AddGamepadMenuItem(opt, CreateGamepadSensitivitySlider("$JOYMNU_OVRSENS", joy));
		AddGamepadMenuItem(opt, CreateGamepadDeadZoneSlider("$JOYMNU_DEADZONE", joy));
	}
	else
	{
		AddGamepadMenuItem(opt, CreateOptionMenuItemStaticText("$JOYMNU_NOCON", 0));
	}

	opt->mScrollPos = 0;
	opt->mSelectedItem = -1;
	opt->mIndent = 0;
	opt->mPosition = -25;
}
