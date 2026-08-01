#include "PluginInterface.h"
#include "QuickOpen.h"
#include <windows.h>

namespace
{
NppData g_nppData{};
FuncItem g_funcItems[4]{};

// Default shortcuts requested for NPPWorkSpace.
ShortcutKey g_workspaceShortcut{true, false, false, 'B'};
ShortcutKey g_searchShortcut{true, false, false, 'P'};
ShortcutKey g_settingsShortcut{false, false, false, 0};
ShortcutKey g_refreshShortcut{false, false, false, 0};

void commandWorkspace()
{
    QuickOpen::instance().toggleWorkspace();
}

void commandSearch()
{
    QuickOpen::instance().focusSearch();
}

void commandSettings()
{
    QuickOpen::instance().openSettings();
}

void commandRefresh()
{
    QuickOpen::instance().refreshWorkspace();
}

void setText(int index, const wchar_t* text)
{
    wcsncpy_s(g_funcItems[index]._itemName, text, _TRUNCATE);
}
}

extern "C" __declspec(dllexport) void setInfo(NppData data)
{
    g_nppData = data;
    QuickOpen::instance().initialize(data._nppHandle);

    setText(0, L"NPPWorkSpace");
    g_funcItems[0]._pFunc = commandWorkspace;
    g_funcItems[0]._pShKey = &g_workspaceShortcut;

    setText(1, L"NPPWorkSpace — Pesquisar");
    g_funcItems[1]._pFunc = commandSearch;
    g_funcItems[1]._pShKey = &g_searchShortcut;

    setText(2, L"NPPWorkSpace — Configurações...");
    g_funcItems[2]._pFunc = commandSettings;
    g_funcItems[2]._pShKey = &g_settingsShortcut;

    setText(3, L"NPPWorkSpace — Atualizar workspace");
    g_funcItems[3]._pFunc = commandRefresh;
    g_funcItems[3]._pShKey = &g_refreshShortcut;
}

extern "C" __declspec(dllexport) const wchar_t* getName()
{
    return L"NPPWorkSpace";
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* n)
{
    if (n) *n = 4;
    // The first plugin command is used as the docking manager's dlgID.
    QuickOpen::instance().setDockCommandId(0);
    QuickOpen::instance().registerDockPanel();
    return g_funcItems;
}

extern "C" __declspec(dllexport) void beNotified(void* notifyCode)
{
    if (!notifyCode) return;
    const auto* hdr = reinterpret_cast<const NMHDR*>(notifyCode);
    if (hdr->code == NPPN_DARKMODECHANGED)
        QuickOpen::instance().onDarkModeChanged();
}

extern "C" __declspec(dllexport) LRESULT messageProc(UINT, WPARAM, LPARAM)
{
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL isUnicode()
{
    return TRUE;
}
