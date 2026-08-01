#pragma once
#include <windows.h>

#ifndef NPPMSG
#define NPPMSG (WM_USER + 1000)
#endif

#define NPPM_MODELESSDIALOG             (NPPMSG + 12)
#define NPPM_DOOPEN                     (NPPMSG + 77)
#define NPPM_ISDARKMODEENABLED          (NPPMSG + 107)
#define NPPM_GETDARKMODECOLORS          (NPPMSG + 108)
#define NPPM_DARKMODESUBCLASSANDTHEME   (NPPMSG + 112)
#define NPPM_GETNPPSETTINGSDIRPATH      (NPPMSG + 119)
#define NPPN_DARKMODECHANGED            (1000 + 27)
#define NPPM_GETCURRENTDIRECTORY        ((WM_USER + 3000) + 2)

namespace NppDarkMode
{
    constexpr ULONG dmfInit = 0x0000000BUL;
    constexpr ULONG dmfHandleChange = 0x0000000CUL;

    struct Colors
    {
        COLORREF background = 0;
        COLORREF softerBackground = 0;
        COLORREF hotBackground = 0;
        COLORREF pureBackground = 0;
        COLORREF errorBackground = 0;
        COLORREF text = 0;
        COLORREF darkerText = 0;
        COLORREF disabledText = 0;
        COLORREF linkText = 0;
        COLORREF edge = 0;
        COLORREF hotEdge = 0;
        COLORREF disabledEdge = 0;
    };
}
