/*
** gitinfo.cpp
** Returns strings from gitinfo.h.
**
**---------------------------------------------------------------------------
** Copyright 2013 Randy Heit
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
** This file is just here so that when gitinfo.h changes, only one source
** file needs to be recompiled.
*/
#include "gitinfo.h"
#include "version.h"

#ifdef _WIN32
#include <cstring>   // _stricmp
#else
#include <strings.h> // strcasecmp
#endif

static const char* NAKARA_FALLBACK_VERSION = "Nakara 1.1 (GZDoom 4.14.2)";

static bool IsUnknownOrEmpty(const char* s)
{
    if (s == nullptr || s[0] == '\0') return true;

#ifdef _WIN32
    if (_stricmp(s, "unknownversion") == 0) return true;
    if (_stricmp(s, "<unknown version>") == 0) return true;
    if (_stricmp(s, "unknown version") == 0) return true;
#else
    if (strcasecmp(s, "unknownversion") == 0) return true;
    if (strcasecmp(s, "<unknown version>") == 0) return true;
    if (strcasecmp(s, "unknown version") == 0) return true;
#endif

    return false;
}

const char* GetGitDescription()
{
    if (IsUnknownOrEmpty(GIT_DESCRIPTION))
        return NAKARA_FALLBACK_VERSION;

    return GIT_DESCRIPTION;
}


const char* GetGitHash()
{

#ifdef GIT_HASH
    if (IsUnknownOrEmpty(GIT_HASH)) return "custom";
    return GIT_HASH;
#else
    return "custom";
#endif
}

const char* GetGitTime()
{
#ifdef GIT_TIME
    if (IsUnknownOrEmpty(GIT_TIME)) return "";
    return GIT_TIME;
#else
    return "";
#endif
}

const char* GetVersionString()
{
    return GetGitDescription();
}
