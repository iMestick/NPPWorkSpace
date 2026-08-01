#pragma once
#include <windows.h>
#include <cstdint>

struct NppData {
    HWND _nppHandle{};
    HWND _scintillaMainHandle{};
    HWND _scintillaSecondHandle{};
};

struct ShortcutKey {
    bool _isCtrl{};
    bool _isAlt{};
    bool _isShift{};
    UCHAR _key{};
};

struct FuncItem {
    wchar_t _itemName[64]{};
    void (*_pFunc)(){};
    int _cmdID{};
    bool _init2Check{};
    ShortcutKey* _pShKey{};
};
