#include "PluginInterface.h"
#include "QuickOpen.h"
#include <windows.h>

NppData g_nppData{};
FuncItem g_funcItems[2]{};

// Default shortcuts requested for NPPWorkSpace.
ShortcutKey g_workspaceShortcut{true, false, false, 'B'};
ShortcutKey g_searchShortcut{true, false, false, 'P'};

std::wstring shortcutToString(const ShortcutKey& key)
{
    std::wstring result;
    if (key._isCtrl) result += L"Ctrl+";
    if (key._isAlt) result += L"Alt+";
    if (key._isShift) result += L"Shift+";
    if (key._key) result += static_cast<wchar_t>(key._key);
    return result;
}

bool setShortcutFromString(ShortcutKey& key, const std::wstring& value)
{
    if (value.empty()) return false;
    ShortcutKey parsed{};
    size_t start = 0;
    while (start < value.size())
    {
        const size_t plus = value.find(L'+', start);
        const std::wstring token = value.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start);
        if (_wcsicmp(token.c_str(), L"Ctrl") == 0) parsed._isCtrl = true;
        else if (_wcsicmp(token.c_str(), L"Alt") == 0) parsed._isAlt = true;
        else if (_wcsicmp(token.c_str(), L"Shift") == 0) parsed._isShift = true;
        else if (token.size() == 1) parsed._key = static_cast<char>(token[0]);
        else return false;
        if (plus == std::wstring::npos) break;
        start = plus + 1;
    }
    if (!parsed._key) return false;
    key = parsed;
    return true;
}

namespace
{

void commandWorkspace()
{
    QuickOpen::instance().toggleWorkspace();
}

void commandSearch()
{
    QuickOpen::instance().focusSearch();
}


void setText(int index, const wchar_t* text)
{
    wcsncpy_s(g_funcItems[index]._itemName, text, _TRUNCATE);
}
}

std::wstring NPPWorkSpace_GetToggleShortcut()
{
    return shortcutToString(g_workspaceShortcut);
}

std::wstring NPPWorkSpace_GetSearchShortcut()
{
    return shortcutToString(g_searchShortcut);
}

void NPPWorkSpace_SetToggleShortcut(const std::wstring& value)
{
    setShortcutFromString(g_workspaceShortcut, value);
}

void NPPWorkSpace_SetSearchShortcut(const std::wstring& value)
{
    setShortcutFromString(g_searchShortcut, value);
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

}

extern "C" __declspec(dllexport) const wchar_t* getName()
{
    return L"NPPWorkSpace";
}

extern "C" __declspec(dllexport) FuncItem* getFuncsArray(int* n)
{
    if (n) *n = 2;
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
