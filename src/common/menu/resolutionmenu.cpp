/*
** resolutionmenu.cpp
** Basic Custom Resolution Selector for the Menu
**
**---------------------------------------------------------------------------
** Copyright 2018 Rachael Alexanderson
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

#include "c_dispatch.h"
#include "c_cvars.h"
#include "v_video.h"
#include "menu.h"
#include "printf.h"

CVAR(Int, menu_resolution_custom_width, 1024, 0)
CVAR(Int, menu_resolution_custom_height, 768, 0)

EXTERN_CVAR(Bool, vid_fullscreen)
EXTERN_CVAR(Bool, win_maximized)
EXTERN_CVAR(Float, vid_scale_custompixelaspect)
EXTERN_CVAR(Int, vid_scale_customwidth)
EXTERN_CVAR(Int, vid_scale_customheight)
EXTERN_CVAR(Int, vid_scalemode)
EXTERN_CVAR(Float, vid_scalefactor)

static bool IsSafeResolution(int width, int height)
{
	// Keep obsolete low-resolution modes out while retaining 1280x720.
	// The upper bound prevents accidental, console-entered allocations that
	// are unreasonably large for the renderer.
	if (width < 1024 || height < 720 || width > 7680 || height > 4320)
	{
		return false;
	}
	return width * height >= 1024 * 768;
}

static void ApplyResolutionSize(int width, int height)
{
	if (screen == nullptr || !IsSafeResolution(width, height))
	{
		return;
	}

	menu_resolution_custom_width = width;
	menu_resolution_custom_height = height;

	// Do not write vid_fullscreen here. In fullscreen/borderless mode only the
	// internal render resolution is changed. SetWindowSize is restricted to a
	// genuinely windowed state because calling it on a fullscreen Windows
	// window can force the application back to windowed mode.
	const bool keepFullscreen = vid_fullscreen || screen->IsFullscreen();

	vid_scale_customwidth = width;
	vid_scale_customheight = height;
	vid_scale_custompixelaspect = 1.0;
	vid_scalefactor = 1.0;
	vid_scalemode = 5;

	if (!keepFullscreen && !win_maximized)
	{
		screen->SetWindowSize(width, height);
		V_OutputResized(screen->GetClientWidth(), screen->GetClientHeight());
	}
}

static void ApplyResolutionPreset(int packedResolution)
{
	if (packedResolution <= 0)
	{
		return;
	}

	ApplyResolutionSize(packedResolution / 10000, packedResolution % 10000);
}

// Temporary menu transport value: width * 10000 + height.
// It is deliberately not archived; the actual video CVARs remain authoritative.
CUSTOM_CVAR(Int, menu_resolution_preset, 0, 0)
{
	ApplyResolutionPreset(self);
}

CCMD (menu_resolution_set_custom)
{
	if (argv.argc() > 2)
	{
		const int width = atoi(argv[1]);
		const int height = atoi(argv[2]);
		if (IsSafeResolution(width, height))
		{
			menu_resolution_custom_width = width;
			menu_resolution_custom_height = height;
		}
	}
	else
	{
		Printf("This command is not meant to be used outside the menu! But if you want to use it, please specify <x> and <y>.\n");
	}
	M_PreviousMenu();
}

CCMD (menu_resolution_commit_changes)
{
	// Keep this legacy command safe even if a console bind or external MENUDEF
	// still calls it. It no longer changes fullscreen state.
	ApplyResolutionSize(menu_resolution_custom_width, menu_resolution_custom_height);
}
