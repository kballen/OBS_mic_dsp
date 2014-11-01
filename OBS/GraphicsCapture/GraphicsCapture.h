/********************************************************************************
 Copyright (C) 2012 Hugh Bailey <obs.jim@gmail.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
********************************************************************************/


#pragma once

#include "OBSApi.h"
#include "resource.h"

#include "GlobalCaptureStuff.h"

//-----------------------------------------------------------

extern HINSTANCE hinstMain;
extern HANDLE textureMutexes[2];

//-----------------------------------------------------------

class GraphicsCaptureMethod
{
public:
    virtual ~GraphicsCaptureMethod() {}
    virtual bool Init(CaptureInfo &info)=0;
    virtual void Destroy()=0;

    virtual Texture* LockTexture()=0;
    virtual void UnlockTexture()=0;
};

//-----------------------------------------------------------

inline BOOL Is64BitWindows()
{
#if defined(_WIN64)
    return TRUE;
#elif defined(_WIN32)
    BOOL f64 = FALSE;
    return IsWow64Process(GetCurrentProcess(), &f64) && f64;
#endif
}

//-----------------------------------------------------------

#include "MemoryCapture.h"
#include "SharedTexCapture.h"
#include "WindowCapture.h"
#include "GraphicsCaptureSource.h"
