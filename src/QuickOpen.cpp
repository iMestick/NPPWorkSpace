#include "QuickOpen.h"
#include "NotepadPlusMsgs.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <functional>
#include <array>
#include <iterator>
#include <system_error>
#include <shellapi.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <regex>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
constexpr wchar_t WINDOW_CLASS[] = L"NPPWorkSpacePanel";
constexpr wchar_t SEARCH_POPUP_CLASS[] = L"NPPWorkSpaceSearchPopup";
constexpr wchar_t PANEL_NAME[] = L"NPPWorkSpace";
constexpr wchar_t MODULE_NAME[] = L"NPPWorkSpace.dll";
constexpr wchar_t REGISTRY_KEY[] = L"Software\\NPPWorkSpace";
constexpr wchar_t REGISTRY_WORKSPACE_VALUE[] = L"WorkspaceFile";

constexpr int ID_TITLE = 1000;
constexpr int ID_SEARCH = 1001;
constexpr int ID_TREE = 1002;
constexpr int ID_RESULTS = 1003;
constexpr int ID_ADD = 1004;
constexpr int ID_NEW = 1005;
constexpr int ID_SAVE = 1006;
constexpr int ID_OPEN = 1014;
constexpr int ID_REMOVE = 1007;
constexpr int ID_EXPAND_ALL = 1012;
constexpr int ID_COLLAPSE_ALL = 1013;
constexpr int ID_CREATE_CONTAINER = 1016;
constexpr int ID_OPEN_SELECTED = 1015;
constexpr int ID_STATUS = 1009;
constexpr int ID_SEARCH_GROUP = 1010;
constexpr int ID_SEARCH_SCOPE = 1017;
constexpr int ID_CONTENT_SEARCH = 1018;
constexpr int ID_CANCEL_SEARCH = 1019;
constexpr int ID_SEARCH_PROGRESS = 1020;
constexpr int ID_SEARCH_PROGRESS_TEXT = 1021;
constexpr int ID_RUN_SEARCH = 1022;
constexpr int ID_VIEW_FOLDERS = 1023;
constexpr int ID_VIEW_SEARCH = 1024;
constexpr int ID_CONTAINER_COLOR = 1025;
constexpr int ID_WORKSPACE_GROUP = 1011;
constexpr int ID_POPUP_SEARCH = 1200;
constexpr int ID_POPUP_RESULTS = 1201;
constexpr int ID_POPUP_CONTENT_SEARCH = 1202;
constexpr int ID_POPUP_RUN_SEARCH = 1203;
constexpr UINT_PTR WORKSPACE_SYNC_TIMER = 4101;
constexpr UINT WM_SEARCH_INDEX_READY = WM_APP + 0x5A1;
constexpr UINT WM_CONTENT_SEARCH_BATCH = WM_APP + 0x5A2;
constexpr UINT WM_CONTENT_SEARCH_PROGRESS = WM_APP + 0x5A3;
constexpr UINT WM_CONTENT_SEARCH_DONE = WM_APP + 0x5A4;
constexpr size_t MAX_CONTENT_FILE_SIZE = 16ull * 1024ull * 1024ull;
constexpr size_t CONTENT_BATCH_FILE_COUNT = 8;
constexpr size_t CONTENT_QUEUE_LIMIT = 512;
constexpr size_t MAX_SNIPPET_CHARS = 220;

constexpr UINT SCI_GOTOPOS = 2025;
constexpr UINT SCI_SETANCHOR = 2026;
constexpr UINT SCI_GETCODEPAGE = 2137;
constexpr UINT SCI_GETLINEENDPOSITION = 2136;
constexpr UINT SCI_POSITIONFROMLINE = 2167;
constexpr UINT SCI_SCROLLCARET = 2169;
constexpr UINT SCI_SETTARGETSTART = 2190;
constexpr UINT SCI_GETTARGETSTART = 2191;
constexpr UINT SCI_SETTARGETEND = 2192;
constexpr UINT SCI_GETTARGETEND = 2193;
constexpr UINT SCI_SEARCHINTARGET = 2197;
constexpr UINT SCI_SETSEARCHFLAGS = 2198;
constexpr UINT SCI_ENSUREVISIBLEENFORCEPOLICY = 2234;
constexpr UINT SCI_SETVISIBLEPOLICY = 2394;
constexpr WPARAM CARET_EVEN = 0x08;
constexpr WPARAM CARET_JUMPS = 0x10;

constexpr int ID_NODE_DUMMY = 2000;
constexpr int MAX_SEARCH_RESULTS = 250;
constexpr int SEARCH_LIMIT_PER_ROOT = 20000;

QuickOpen* g_instance = nullptr;

struct SortingData4lParamMirror
{
    std::wstring _rootPath;
    std::wstring _label;
    bool _isFolder = false;
};

enum class TextEncoding
{
    Unknown,
    Utf8,
    Utf8Bom,
    Utf16LE,
    Utf16BE,
    ShiftJis,
    Big5,
    Ansi
};

struct SnippetInfo
{
    std::wstring text;
    size_t matchStart{};
};

void setButtonChecked(HWND button, bool checked)
{
    if (button) SendMessageW(button, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool isButtonChecked(HWND button)
{
    return button && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

#ifdef _DEBUG
void debugLog(const std::wstring& message)
{
    OutputDebugStringW((L"[NPPWorkSpace] " + message + L"\n").c_str());
}
#define QO_DEBUG_LOG(expr) debugLog((expr))
#else
#define QO_DEBUG_LOG(expr) ((void)0)
#endif

const wchar_t* encodingName(TextEncoding encoding)
{
    switch (encoding)
    {
    case TextEncoding::Utf8: return L"UTF-8";
    case TextEncoding::Utf8Bom: return L"UTF-8 BOM";
    case TextEncoding::Utf16LE: return L"UTF-16 LE";
    case TextEncoding::Utf16BE: return L"UTF-16 BE";
    case TextEncoding::ShiftJis: return L"Shift-JIS";
    case TextEncoding::Big5: return L"Big5";
    case TextEncoding::Ansi: return L"ANSI";
    default: return L"Unknown";
    }
}

std::wstring lowerText(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool isSupportedTextExtension(const std::filesystem::path& path)
{
    std::wstring ext = lowerText(path.extension().wstring());
    static const std::array<const wchar_t*, 17> textExts = {
        L".ini", L".txt", L".json", L".xml", L".lua", L".cfg", L".conf",
        L".csv", L".log", L".hpp", L".h", L".cpp", L".c", L".cs", L".py",
        L".js", L".ts"
    };
    return std::find_if(textExts.begin(), textExts.end(),
        [&](const wchar_t* allowed) { return ext == allowed; }) != textExts.end();
}

bool isAsciiString(const std::wstring& value)
{
    return std::all_of(value.begin(), value.end(), [](wchar_t ch) { return ch >= 0 && ch < 0x80; });
}

std::wstring normalizeScopePath(std::wstring value)
{
    value = lowerText(std::move(value));
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (!value.empty() && value.back() == L'\\')
        value.pop_back();
    return value;
}

bool isPathEnabledForScope(const std::filesystem::path& path,
                           const std::unordered_set<std::wstring>& includedPaths,
                           const std::unordered_set<std::wstring>& disabledPaths)
{
    std::wstring value = normalizeScopePath(path.wstring());
    if (!value.empty()) value.push_back(L'\\');

    auto isUnderAny = [&](const std::unordered_set<std::wstring>& roots)
    {
        for (const auto& root : roots)
        {
            std::wstring prefix = normalizeScopePath(root);
            if (!prefix.empty()) prefix.push_back(L'\\');
            if (!prefix.empty() && value.size() >= prefix.size() &&
                value.compare(0, prefix.size(), prefix) == 0)
                return true;
        }
        return false;
    };

    if (!includedPaths.empty() && !isUnderAny(includedPaths)) return false;
    if (!disabledPaths.empty() && isUnderAny(disabledPaths)) return false;
    return true;
}

bool isPathEnabledForScope(const std::filesystem::path& path, const std::unordered_set<std::wstring>& disabledPaths)
{
    static const std::unordered_set<std::wstring> emptyIncludedPaths;
    return isPathEnabledForScope(path, emptyIncludedPaths, disabledPaths);
}

COLORREF defaultContainerColor(size_t index)
{
    static constexpr COLORREF palette[] = {
        RGB(86, 156, 214),
        RGB(78, 201, 176),
        RGB(206, 145, 120),
        RGB(220, 220, 170),
        RGB(197, 134, 192),
        RGB(181, 206, 168),
        RGB(244, 166, 88),
        RGB(114, 159, 207)
    };
    return palette[index % std::size(palette)];
}

std::wstring colorToHex(COLORREF color)
{
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return buffer;
}

int hexValue(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

bool parseHexColor(const std::wstring& value, COLORREF& color)
{
    size_t offset = !value.empty() && value[0] == L'#' ? 1 : 0;
    if (value.size() - offset != 6) return false;

    int channels[3]{};
    for (size_t i = 0; i < 3; ++i)
    {
        const int hi = hexValue(value[offset + i * 2]);
        const int lo = hexValue(value[offset + i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        channels[i] = (hi << 4) | lo;
    }
    color = RGB(channels[0], channels[1], channels[2]);
    return true;
}

unsigned char foldAscii(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? static_cast<unsigned char>(ch + ('a' - 'A')) : ch;
}

bool containsAsciiBytesInsensitive(const std::vector<unsigned char>& data, const std::string& needle)
{
    if (needle.empty() || data.size() < needle.size()) return false;

    std::string foldedNeedle = needle;
    for (char& ch : foldedNeedle)
        ch = static_cast<char>(foldAscii(static_cast<unsigned char>(ch)));

    const size_t m = foldedNeedle.size();
    size_t skip[256];
    for (size_t i = 0; i < std::size(skip); ++i) skip[i] = m;
    for (size_t i = 0; i + 1 < m; ++i)
        skip[foldAscii(static_cast<unsigned char>(foldedNeedle[i]))] = m - 1 - i;

    for (size_t pos = 0; pos + m <= data.size(); )
    {
        size_t j = m;
        while (j > 0 &&
               foldAscii(data[pos + j - 1]) == static_cast<unsigned char>(foldedNeedle[j - 1]))
            --j;
        if (j == 0) return true;
        pos += skip[foldAscii(data[pos + m - 1])];
    }
    return false;
}

bool hasUtf16Pattern(const std::vector<unsigned char>& data)
{
    if (data.size() < 4) return false;
    if ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))
        return true;

    size_t evenZeros = 0;
    size_t oddZeros = 0;
    const size_t probe = (std::min)(data.size(), size_t(4096));
    for (size_t i = 0; i < probe; ++i)
    {
        if (data[i] == 0)
        {
            if ((i & 1) == 0) ++evenZeros;
            else ++oddZeros;
        }
    }
    return oddZeros > probe / 8 || evenZeros > probe / 8;
}

bool looksBinaryWithoutUtf16(const std::vector<unsigned char>& data)
{
    if (data.empty()) return true;
    if (hasUtf16Pattern(data)) return false;

    size_t zeroCount = 0;
    size_t controlCount = 0;
    const size_t probe = (std::min)(data.size(), size_t(8192));
    for (size_t i = 0; i < probe; ++i)
    {
        const unsigned char ch = data[i];
        if (ch == 0) ++zeroCount;
        else if (ch < 0x20 && ch != '\r' && ch != '\n' && ch != '\t' && ch != '\f')
            ++controlCount;
    }
    return zeroCount > 0 || controlCount > probe / 8;
}

bool decodeWithCodePage(UINT codePage, DWORD flags, const unsigned char* data, size_t size, std::wstring& out)
{
    if (size == 0) { out.clear(); return true; }
    if (size > static_cast<size_t>((std::numeric_limits<int>::max)())) return false;
    const int inputSize = static_cast<int>(size);
    const auto* bytes = reinterpret_cast<LPCCH>(data);
    int needed = MultiByteToWideChar(codePage, flags, bytes, inputSize, nullptr, 0);
    if (needed <= 0 && flags != 0)
        needed = MultiByteToWideChar(codePage, 0, bytes, inputSize, nullptr, 0);
    if (needed <= 0) return false;

    out.assign(static_cast<size_t>(needed), L'\0');
    int written = MultiByteToWideChar(codePage, needed > 0 && flags != 0 ? flags : 0,
                                      bytes, inputSize, out.data(), needed);
    if (written <= 0 && flags != 0)
        written = MultiByteToWideChar(codePage, 0, bytes, inputSize, out.data(), needed);
    if (written <= 0) return false;
    out.resize(static_cast<size_t>(written));
    return true;
}

bool decodeUtf16(const std::vector<unsigned char>& data, bool bigEndian, size_t offset, std::wstring& out)
{
    if (data.size() <= offset) { out.clear(); return true; }
    const size_t units = (data.size() - offset) / 2;
    out.assign(units, L'\0');
    for (size_t i = 0; i < units; ++i)
    {
        const size_t pos = offset + i * 2;
        const uint16_t code = bigEndian
            ? static_cast<uint16_t>((data[pos] << 8) | data[pos + 1])
            : static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
        out[i] = static_cast<wchar_t>(code);
    }
    return true;
}

int scoreDecodedText(const std::wstring& value)
{
    if (value.empty()) return 0;
    int score = 0;
    const size_t probe = (std::min)(value.size(), size_t(8192));
    for (size_t i = 0; i < probe; ++i)
    {
        const wchar_t ch = value[i];
        if (ch == 0 || ch == 0xFFFD)
        {
            score -= 20;
        }
        else if (ch == L'\r' || ch == L'\n' || ch == L'\t')
        {
            score += 1;
        }
        else if (ch < 0x20)
        {
            score -= 8;
        }
        else if (ch >= 0x20 && ch <= 0x7E)
        {
            score += 3;
        }
        else if ((ch >= 0x00C0 && ch <= 0x024F) || (ch >= 0x1E00 && ch <= 0x1EFF))
        {
            score += 4;
        }
        else if ((ch >= 0x3040 && ch <= 0x30FF) || (ch >= 0x4E00 && ch <= 0x9FFF))
        {
            score += 4;
        }
        else
        {
            score += 1;
        }
    }
    return score;
}

void sanitizeDecodedText(std::wstring& value)
{
    for (wchar_t& ch : value)
    {
        if (ch == 0 || ch == 0xFFFD || (ch < 0x20 && ch != L'\r' && ch != L'\n' && ch != L'\t'))
            ch = L' ';
    }
}

bool decodeTextBytes(const std::vector<unsigned char>& data, std::wstring& text, TextEncoding& encoding)
{
    text.clear();
    encoding = TextEncoding::Unknown;
    if (data.empty() || looksBinaryWithoutUtf16(data)) return false;

    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
    {
        if (decodeWithCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, data.data() + 3, data.size() - 3, text))
        {
            encoding = TextEncoding::Utf8Bom;
            sanitizeDecodedText(text);
            return true;
        }
    }
    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE)
    {
        decodeUtf16(data, false, 2, text);
        encoding = TextEncoding::Utf16LE;
        sanitizeDecodedText(text);
        return true;
    }
    if (data.size() >= 2 && data[0] == 0xFE && data[1] == 0xFF)
    {
        decodeUtf16(data, true, 2, text);
        encoding = TextEncoding::Utf16BE;
        sanitizeDecodedText(text);
        return true;
    }

    if (hasUtf16Pattern(data))
    {
        size_t evenZeros = 0;
        size_t oddZeros = 0;
        const size_t probe = (std::min)(data.size(), size_t(4096));
        for (size_t i = 0; i < probe; ++i)
        {
            if (data[i] == 0)
            {
                if ((i & 1) == 0) ++evenZeros;
                else ++oddZeros;
            }
        }
        const bool bigEndian = evenZeros > oddZeros;
        decodeUtf16(data, bigEndian, 0, text);
        encoding = bigEndian ? TextEncoding::Utf16BE : TextEncoding::Utf16LE;
        sanitizeDecodedText(text);
        return scoreDecodedText(text) > 0;
    }

    std::wstring utf8;
    if (decodeWithCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), data.size(), utf8) &&
        scoreDecodedText(utf8) > 0)
    {
        text = std::move(utf8);
        encoding = TextEncoding::Utf8;
        sanitizeDecodedText(text);
        return true;
    }

    struct Candidate
    {
        UINT codePage;
        TextEncoding encoding;
        DWORD flags;
    };
    static const Candidate candidates[] = {
        {CP_ACP, TextEncoding::Ansi, 0},
        {932, TextEncoding::ShiftJis, MB_ERR_INVALID_CHARS},
        {950, TextEncoding::Big5, MB_ERR_INVALID_CHARS}
    };

    int bestScore = (std::numeric_limits<int>::min)();
    std::wstring bestText;
    TextEncoding bestEncoding = TextEncoding::Unknown;
    for (const auto& candidate : candidates)
    {
        std::wstring decoded;
        if (!decodeWithCodePage(candidate.codePage, candidate.flags, data.data(), data.size(), decoded))
            continue;
        const int score = scoreDecodedText(decoded);
        if (score > bestScore)
        {
            bestScore = score;
            bestText = std::move(decoded);
            bestEncoding = candidate.encoding;
        }
    }

    if (bestEncoding == TextEncoding::Unknown || bestScore <= 0) return false;
    text = std::move(bestText);
    encoding = bestEncoding;
    sanitizeDecodedText(text);
    return true;
}

SnippetInfo makeSnippet(const std::wstring& line, size_t matchStart, size_t matchLength)
{
    const size_t half = MAX_SNIPPET_CHARS / 2;
    size_t start = 0;
    if (matchStart > half) start = matchStart - half;

    size_t end = (std::min)(line.size(), start + MAX_SNIPPET_CHARS);
    if (end < matchStart + matchLength)
    {
        end = (std::min)(line.size(), matchStart + matchLength + half);
        start = end > MAX_SNIPPET_CHARS ? end - MAX_SNIPPET_CHARS : 0;
    }

    std::wstring snippet = line.substr(start, end - start);
    for (wchar_t& ch : snippet)
    {
        if (ch == L'\r' || ch == L'\n') ch = L' ';
        else if (ch == L'\t') ch = L' ';
    }

    size_t adjusted = matchStart >= start ? matchStart - start : 0;
    if (start > 0)
    {
        snippet.insert(0, L"... ");
        adjusted += 4;
    }
    if (end < line.size())
        snippet += L" ...";

    return {std::move(snippet), adjusted};
}

std::string utf8FromWide(const std::wstring& value)
{
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wideFromUtf8(const std::string& value)
{
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring jsonEscape(const std::wstring& value)
{
    std::wstring out;
    out.reserve(value.size() + 16);
    for (wchar_t ch : value)
    {
        switch (ch)
        {
        case L'\\': out += L"\\\\"; break;
        case L'"': out += L"\\\""; break;
        case L'\r': out += L"\\r"; break;
        case L'\n': out += L"\\n"; break;
        case L'\t': out += L"\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::wstring jsonUnescape(const std::wstring& value)
{
    std::wstring out;
    out.reserve(value.size());
    bool escaped = false;
    for (wchar_t ch : value)
    {
        if (!escaped)
        {
            if (ch == L'\\') escaped = true;
            else out += ch;
            continue;
        }
        switch (ch)
        {
        case L'\\': out += L'\\'; break;
        case L'"': out += L'"'; break;
        case L'r': out += L'\r'; break;
        case L'n': out += L'\n'; break;
        case L't': out += L'\t'; break;
        default: out += ch; break;
        }
        escaped = false;
    }
    if (escaped) out += L'\\';
    return out;
}

bool extractJsonString(const std::wstring& json, const std::wstring& key, std::wstring& value)
{
    const std::wstring needle = L"\"" + key + L"\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::wstring::npos) return false;
    size_t pos = json.find(L':', keyPos + needle.size());
    if (pos == std::wstring::npos) return false;
    ++pos;
    while (pos < json.size() && iswspace(json[pos])) ++pos;
    if (pos >= json.size() || json[pos] != L'"') return false;
    ++pos;
    std::wstring raw;
    bool escaped = false;
    while (pos < json.size())
    {
        const wchar_t ch = json[pos++];
        if (!escaped && ch == L'"')
        {
            value = jsonUnescape(raw);
            return true;
        }
        if (!escaped && ch == L'\\')
        {
            raw += ch;
            escaped = true;
        }
        else
        {
            raw += ch;
            escaped = false;
        }
    }
    return false;
}

bool extractJsonStringArray(const std::wstring& json, const std::wstring& key, std::vector<std::wstring>& values)
{
    const std::wstring needle = L"\"" + key + L"\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::wstring::npos) return false;
    const size_t open = json.find(L'[', keyPos + needle.size());
    if (open == std::wstring::npos) return false;
    const size_t close = json.find(L']', open + 1);
    if (close == std::wstring::npos) return false;

    size_t pos = open + 1;
    while (pos < close)
    {
        while (pos < close && (iswspace(json[pos]) || json[pos] == L',')) ++pos;
        if (pos >= close) break;
        if (json[pos] != L'"') return false;
        ++pos;
        std::wstring value;
        bool escaped = false;
        while (pos < close)
        {
            const wchar_t ch = json[pos++];
            if (!escaped && ch == L'"') break;
            if (!escaped && ch == L'\\') { escaped = true; value += ch; continue; }
            value += ch;
            escaped = false;
        }
        values.push_back(jsonUnescape(value));
    }
    return true;
}

bool readWorkspacePathFromRegistry(std::wstring& value)
{
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(key, REGISTRY_WORKSPACE_VALUE, nullptr, &type, nullptr, &bytes);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
    {
        RegCloseKey(key);
        return false;
    }

    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    result = RegQueryValueExW(key, REGISTRY_WORKSPACE_VALUE, nullptr, &type,
                              reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return false;

    buffer.resize(wcsnlen_s(buffer.c_str(), buffer.size()));
    value = std::move(buffer);
    return !value.empty();
}

void writeWorkspacePathToRegistry(const std::wstring& value)
{
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    RegSetValueExW(key, REGISTRY_WORKSPACE_VALUE, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

bool chooseFolder(HWND owner, std::filesystem::path& result)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Adicionar pasta ao NPPWorkSpace";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;

    wchar_t path[MAX_PATH * 4]{};
    const bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return false;

    result = std::filesystem::path(path);
    return true;
}


struct TextPromptState
{
    std::wstring value;
    HWND npp{};
    HFONT font{};
    HFONT titleFont{};
    HBRUSH backgroundBrush{};
    COLORREF background{};
    COLORREF text{};
    COLORREF edge{};
    bool accepted{false};
    POINT screenPoint{-1, -1};
};

INT_PTR CALLBACK textPromptProc(HWND dlg, UINT msg, WPARAM w, LPARAM l)
{
    auto* state = reinterpret_cast<TextPromptState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (msg == WM_INITDIALOG)
    {
        state = reinterpret_cast<TextPromptState*>(l);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        NppDarkMode::Colors colors{};
        const bool dark = state->npp && SendMessageW(state->npp, NPPM_ISDARKMODEENABLED, 0, 0) != FALSE;
        bool themed = dark && state->npp &&
            SendMessageW(state->npp, NPPM_GETDARKMODECOLORS, sizeof(colors), reinterpret_cast<LPARAM>(&colors)) != FALSE;
        if (!themed)
        {
            colors.background = GetSysColor(COLOR_BTNFACE);
            colors.pureBackground = GetSysColor(COLOR_WINDOW);
            colors.text = GetSysColor(COLOR_WINDOWTEXT);
            colors.edge = GetSysColor(COLOR_BTNSHADOW);
        }
        state->background = colors.background;
        state->text = colors.text;
        state->edge = colors.edge;
        state->backgroundBrush = CreateSolidBrush(colors.background);

        // Let Notepad++ apply the exact same dark/light control theme used by
        // its own dialogs, while still keeping this dialog independent.
        if (state->npp)
        {
            for (HWND child : {dlg, GetDlgItem(dlg, 1000), GetDlgItem(dlg, 1001),
                               GetDlgItem(dlg, IDOK), GetDlgItem(dlg, IDCANCEL)})
            {
                if (child)
                    SendMessageW(state->npp, NPPM_DARKMODESUBCLASSANDTHEME,
                                 static_cast<WPARAM>(NppDarkMode::dmfHandleChange), reinterpret_cast<LPARAM>(child));
            }
        }

        if (state->font)
        {
            for (HWND child : {GetDlgItem(dlg, 1000), GetDlgItem(dlg, 1001),
                               GetDlgItem(dlg, IDOK), GetDlgItem(dlg, IDCANCEL)})
            {
                if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            }
        }
        if (state->titleFont)
            SendMessageW(GetDlgItem(dlg, 1000), WM_SETFONT, reinterpret_cast<WPARAM>(state->titleFont), TRUE);

        SetDlgItemTextW(dlg, 1001, state->value.c_str());

        // Open the compact prompt close to the point that triggered it.
        if (state->screenPoint.x >= 0 && state->screenPoint.y >= 0)
        {
            RECT rc{};
            GetWindowRect(dlg, &rc);
            const int promptWidth = rc.right - rc.left;
            const int promptHeight = rc.bottom - rc.top;
            HMONITOR monitor = MonitorFromPoint(state->screenPoint, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(monitor, &mi);
            int x = state->screenPoint.x + 10;
            int y = state->screenPoint.y + 10;
            if (x + promptWidth > mi.rcWork.right) x = state->screenPoint.x - promptWidth - 10;
            if (y + promptHeight > mi.rcWork.bottom) y = state->screenPoint.y - promptHeight - 10;
            x = (std::max)(static_cast<int>(mi.rcWork.left), (std::min)(x, static_cast<int>(mi.rcWork.right) - promptWidth));
            y = (std::max)(static_cast<int>(mi.rcWork.top), (std::min)(y, static_cast<int>(mi.rcWork.bottom) - promptHeight));
            SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        SetFocus(GetDlgItem(dlg, 1001));
        SendDlgItemMessageW(dlg, 1001, EM_SETSEL, 0, -1);
        return FALSE;
    }
    if (!state) return FALSE;

    if (msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT)
    {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, state->text);
        SetBkColor(dc, state->background);
        if (msg == WM_CTLCOLORSTATIC)
            SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(state->backgroundBrush);
    }

    if (msg == WM_COMMAND)
    {
        if (LOWORD(w) == IDOK)
        {
            wchar_t buffer[512]{};
            GetDlgItemTextW(dlg, 1001, buffer, static_cast<int>(std::size(buffer)));
            state->value = buffer;
            state->accepted = !state->value.empty();
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL)
        {
            state->accepted = false;
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    if (msg == WM_CLOSE)
    {
        state->accepted = false;
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    if (msg == WM_DESTROY)
    {
        if (state->backgroundBrush)
        {
            DeleteObject(state->backgroundBrush);
            state->backgroundBrush = nullptr;
        }
    }
    return FALSE;
}

bool promptForText(HWND owner, HWND npp, HFONT font, HFONT titleFont,
                   const wchar_t* title, const wchar_t* prompt, std::wstring& value,
                   POINT screenPoint)
{
    struct DialogBuffer
    {
        DLGTEMPLATE dlg{};
    };

    // Compact prompt sized like a small native Notepad++ utility dialog.
    std::vector<BYTE> data(2048, 0);
    auto* dlg = reinterpret_cast<DLGTEMPLATE*>(data.data());
    dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dlg->dwExtendedStyle = 0;
    dlg->cdit = 4;
    dlg->x = 0; dlg->y = 0; dlg->cx = 250; dlg->cy = 88;

    WORD* p = reinterpret_cast<WORD*>(dlg + 1);
    *p++ = 0; *p++ = 0;
    const wchar_t* t = title;
    while ((*p++ = static_cast<WORD>(*t++)) != 0) {}

    // The actual font is applied to every control at WM_INITDIALOG. The
    // template still needs a valid face name for Win32 dialog creation.
    *p++ = 9;
    const wchar_t* fontName = L"Segoe UI";
    while ((*p++ = static_cast<WORD>(*fontName++)) != 0) {}

    auto alignDword = [&p]()
    {
        p = reinterpret_cast<WORD*>((reinterpret_cast<ULONG_PTR>(p) + 3) & ~static_cast<ULONG_PTR>(3));
    };
    auto addControl = [&](DWORD style, short x, short y, short cx, short cy, WORD id,
                          LPCWSTR cls, LPCWSTR text)
    {
        alignDword();
        auto* item = reinterpret_cast<DLGITEMTEMPLATE*>(p);
        item->style = style;
        item->dwExtendedStyle = 0;
        item->x = x; item->y = y; item->cx = cx; item->cy = cy; item->id = id;
        p = reinterpret_cast<WORD*>(item + 1);
        if (cls[0] == L'\0')
        {
            *p++ = 0xFFFF; *p++ = 0x0082;
        }
        else
        {
            while ((*p++ = static_cast<WORD>(*cls++)) != 0) {}
        }
        if (text)
        {
            while ((*p++ = static_cast<WORD>(*text++)) != 0) {}
        }
        else *p++ = 0;
        *p++ = 0;
    };

    addControl(WS_CHILD | WS_VISIBLE, 12, 10, 226, 14, 1000, L"STATIC", prompt);
    addControl(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
               12, 28, 226, 18, 1001, L"EDIT", value.c_str());
    addControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
               132, 58, 50, 20, IDOK, L"BUTTON", L"OK");
    addControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
               188, 58, 50, 20, IDCANCEL, L"BUTTON", L"Cancelar");

    TextPromptState state{value, npp, font, titleFont, nullptr, 0, 0, 0, false, screenPoint};
    const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dlg, owner,
                                                   textPromptProc, reinterpret_cast<LPARAM>(&state));
    if (result == IDOK && state.accepted)
    {
        value = state.value;
        return true;
    }
    return false;
}

BOOL CALLBACK enumTree(HWND hwnd, LPARAM lp)
{
    auto* result = reinterpret_cast<HWND*>(lp);
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (_wcsicmp(cls, L"SysTreeView32") != 0) return TRUE;

    const int id = GetDlgCtrlID(hwnd);
    if (id == 3531)
    {
        *result = hwnd;
        return FALSE;
    }

    HWND p = hwnd;
    for (int i = 0; i < 8 && p; ++i)
    {
        wchar_t text[256]{};
        GetWindowTextW(p, text, 256);
        if (_wcsicmp(text, L"Folder as Workspace") == 0)
        {
            *result = hwnd;
            return FALSE;
        }
        p = GetParent(p);
    }
    return TRUE;
}
}

QuickOpen& QuickOpen::instance()
{
    static QuickOpen instance;
    return instance;
}

void QuickOpen::initialize(HWND nppHandle)
{
    _npp = nppHandle;
    g_instance = this;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    loadSettings();
    if (_workspaceFile.empty()) _workspaceFile = getWorkspaceFilePath();
    createWindow();
    if (_npp) {
        DragAcceptFiles(_npp, TRUE);
        _oldNppProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(_npp, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::nppProc)));
    }
    refreshWorkspace();
    disableNativeFolderWorkspace();
}

void QuickOpen::destroy()
{
    _searchIndexGeneration.fetch_add(1);
    _searchQueryGeneration.fetch_add(1);
    if (_searchIndexThread.joinable()) _searchIndexThread.join();
    if (_searchQueryThread.joinable()) _searchQueryThread.join();
    if (_syncTimer && _window)
    {
        KillTimer(_window, _syncTimer);
        _syncTimer = 0;
    }
    if (_tooltips)
    {
        DestroyWindow(_tooltips);
        _tooltips = nullptr;
    }
    if (_searchPopup)
    {
        DestroyWindow(_searchPopup);
        _searchPopup = nullptr;
        _searchPopupEdit = nullptr;
        _searchPopupResults = nullptr;
    }

    if (_npp && _oldNppProc) {
        SetWindowLongPtrW(_npp, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(_oldNppProc));
        _oldNppProc = nullptr;
    }

    releaseDockHost();
    releaseDockSplitters();
    unregisterDock();

    if (_window)
    {
        DestroyWindow(_window);
        _window = nullptr;
    }

    if (_font) DeleteObject(_font);
    if (_titleFont) DeleteObject(_titleFont);
    if (_symbolFont) DeleteObject(_symbolFont);
    if (_backgroundBrush) DeleteObject(_backgroundBrush);
    _font = nullptr;
    _titleFont = nullptr;
    _symbolFont = nullptr;
    _backgroundBrush = nullptr;
    g_instance = nullptr;
}

bool QuickOpen::isVisible() const
{
    return _window && IsWindowVisible(_window);
}

void QuickOpen::createWindow()
{
    if (_window) return;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &QuickOpen::windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    // Use the Windows UI font that Notepad++ itself follows instead of a
    // plugin-specific font. This keeps the panel consistent with the host.
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    const LOGFONTW& lf = ncm.lfMessageFont;
    _font = CreateFontIndirectW(&lf);
    LOGFONTW titleLf = lf;
    titleLf.lfWeight = FW_SEMIBOLD;
    _titleFont = CreateFontIndirectW(&titleLf);
    _symbolFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");

    // This is a CHILD window intentionally. Notepad++'s docking manager takes
    // ownership of its placement and visibility after registration.
    _window = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        WINDOW_CLASS,
        PANEL_NAME,
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 460, 700,
        _npp,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (!_window) return;

    createControls();
    applyTheme();
    _syncTimer = SetTimer(_window, WORKSPACE_SYNC_TIMER, 500, nullptr);
}

void QuickOpen::registerDockPanel()
{
    registerDock();
}

void QuickOpen::registerDock()
{
    if (!_window || _registeredDock || !_npp) return;

    DockedWidgetData data{};
    data.hClient = _window;
    data.pszName = PANEL_NAME;
    data.dlgID = _dockCommandId;
    data.uMask = DWS_ICONTAB | DWS_DF_CONT_LEFT;
    data.hIconTab = LoadIconW(nullptr, IDI_APPLICATION);
    data.pszModuleName = MODULE_NAME;

    if (SendMessageW(_npp, NPPM_DMMREGASDCKDLG, 0, reinterpret_cast<LPARAM>(&data)))
    {
        _registeredDock = true;
        refreshDockHost();
        refreshDockSplitters();
    }
}

void QuickOpen::refreshDockHost()
{
    if (!_window) return;
    HWND host = GetParent(_window);
    if (!host || host == _dockHost) return;

    releaseDockHost();
    _dockHost = host;
    _oldDockHostProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        _dockHost, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::dockHostProc)));
}

void QuickOpen::releaseDockHost()
{
    if (_dockHost && _oldDockHostProc)
    {
        SetWindowLongPtrW(_dockHost, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(_oldDockHostProc));
    }
    _dockHost = nullptr;
    _oldDockHostProc = nullptr;
}

LRESULT CALLBACK QuickOpen::dockHostProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (!g_instance) return DefWindowProcW(h, msg, w, l);
    return g_instance->handleDockHostMessage(h, msg, w, l);
}

LRESULT CALLBACK QuickOpen::dockSplitterProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (!g_instance) return DefWindowProcW(h, msg, w, l);
    return g_instance->handleDockSplitterMessage(h, msg, w, l);
}

namespace
{
constexpr int WORKSPACE_MIN_WIDTH = 430;
constexpr int WORKSPACE_MIN_HEIGHT = 360;

enum class DockSide { None, Left, Right, Top, Bottom };

DockSide getDockSideForSplitter(HWND panel, HWND splitter)
{
    if (!panel || !splitter) return DockSide::None;
    HWND container = GetParent(panel);
    if (!container) return DockSide::None;

    RECT rcContainer{}, rcSplitter{};
    if (!GetWindowRect(container, &rcContainer) || !GetWindowRect(splitter, &rcSplitter))
        return DockSide::None;

    constexpr LONG tolerance = 8;
    if (rcSplitter.left >= rcContainer.right - tolerance && rcSplitter.left <= rcContainer.right + tolerance)
        return DockSide::Left;
    if (rcSplitter.right <= rcContainer.left + tolerance && rcSplitter.right >= rcContainer.left - tolerance)
        return DockSide::Right;
    if (rcSplitter.top >= rcContainer.bottom - tolerance && rcSplitter.top <= rcContainer.bottom + tolerance)
        return DockSide::Top;
    if (rcSplitter.bottom <= rcContainer.top + tolerance && rcSplitter.bottom >= rcContainer.top - tolerance)
        return DockSide::Bottom;
    return DockSide::None;
}

void clampWorkspaceMinMaxInfo(HWND, MINMAXINFO* info)
{
    if (!info) return;
    info->ptMinTrackSize.x = (std::max)(info->ptMinTrackSize.x, static_cast<LONG>(WORKSPACE_MIN_WIDTH));
    info->ptMinTrackSize.y = (std::max)(info->ptMinTrackSize.y, static_cast<LONG>(WORKSPACE_MIN_HEIGHT));
}

void clampWorkspaceWindowPos(HWND, WINDOWPOS* pos)
{
    if (!pos || (pos->flags & SWP_NOSIZE)) return;
    const int minWidth = static_cast<int>(WORKSPACE_MIN_WIDTH);
    const int minHeight = static_cast<int>(WORKSPACE_MIN_HEIGHT);
    pos->cx = (std::max)(minWidth, pos->cx);
    pos->cy = (std::max)(minHeight, pos->cy);
}
}

void QuickOpen::refreshDockSplitters()
{
    if (!_npp) return;

    auto hookClass = [this](const wchar_t* className)
    {
        HWND h = nullptr;
        while ((h = FindWindowExW(_npp, h, className, nullptr)) != nullptr)
        {
            bool already = false;
            for (const auto& hook : _dockSplitters)
            {
                if (hook.hwnd == h) { already = true; break; }
            }
            if (already) continue;

            WNDPROC oldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::dockSplitterProc)));
            if (oldProc)
                _dockSplitters.push_back({h, oldProc});
        }
    };

    // Notepad++ creates these two native DockingSplitter classes. The splitter,
    // not the plugin child window, owns the mouse-driven resize operation.
    // This is important for docked panels: constraining only the plugin HWND
    // cannot stop DockingManager from moving the splitter past the panel.
    hookClass(L"wedockspliter");
    hookClass(L"nsdockspliter");
}

void QuickOpen::releaseDockSplitters()
{
    for (auto it = _dockSplitters.rbegin(); it != _dockSplitters.rend(); ++it)
    {
        if (it->hwnd && IsWindow(it->hwnd) && it->oldProc)
            SetWindowLongPtrW(it->hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->oldProc));
    }
    _dockSplitters.clear();
    _activeSplitter = nullptr;
    _splitterTracking = false;
}

LRESULT QuickOpen::handleDockSplitterMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    WNDPROC oldProc = nullptr;
    for (const auto& hook : _dockSplitters)
    {
        if (hook.hwnd == h) { oldProc = hook.oldProc; break; }
    }
    if (!oldProc) return DefWindowProcW(h, msg, w, l);

    if (msg == WM_LBUTTONDOWN)
    {
        GetCursorPos(&_lastSplitterCursor);
        _activeSplitter = h;
        _splitterTracking = true;
    }
    else if (msg == WM_MOUSEMOVE && _splitterTracking && _activeSplitter == h && (w & MK_LBUTTON))
    {
        HWND container = _window ? GetParent(_window) : nullptr;
        DockSide side = getDockSideForSplitter(_window, h);
        if (container && side != DockSide::None)
        {
            RECT rc{};
            GetWindowRect(container, &rc);
            POINT cursor{};
            GetCursorPos(&cursor);

            const int dx = cursor.x - _lastSplitterCursor.x;
            const int dy = cursor.y - _lastSplitterCursor.y;
            int current = 0;
            int delta = 0;
            int minSize = 0;

            switch (side)
            {
            case DockSide::Left:
                current = rc.right - rc.left;
                delta = dx;
                minSize = WORKSPACE_MIN_WIDTH;
                break;
            case DockSide::Right:
                current = rc.right - rc.left;
                delta = -dx;
                minSize = WORKSPACE_MIN_WIDTH;
                break;
            case DockSide::Top:
                current = rc.bottom - rc.top;
                delta = dy;
                minSize = WORKSPACE_MIN_HEIGHT;
                break;
            case DockSide::Bottom:
                current = rc.bottom - rc.top;
                delta = -dy;
                minSize = WORKSPACE_MIN_HEIGHT;
                break;
            default:
                break;
            }

            const int requested = current + delta;
            const int clamped = (std::max)(minSize, requested);
            if (clamped != requested)
            {
                const int allowedDelta = clamped - current;
                POINT corrected = cursor;
                if (side == DockSide::Left) corrected.x = _lastSplitterCursor.x + allowedDelta;
                else if (side == DockSide::Right) corrected.x = _lastSplitterCursor.x - allowedDelta;
                else if (side == DockSide::Top) corrected.y = _lastSplitterCursor.y + allowedDelta;
                else if (side == DockSide::Bottom) corrected.y = _lastSplitterCursor.y - allowedDelta;

                SetCursorPos(corrected.x, corrected.y);
                GetCursorPos(&cursor);
            }

            _lastSplitterCursor = cursor;
        }
    }
    else if (msg == WM_LBUTTONUP || msg == WM_NCLBUTTONUP ||
             (msg == WM_CAPTURECHANGED && _splitterTracking))
    {
        _splitterTracking = false;
        _activeSplitter = nullptr;
    }

    return CallWindowProcW(oldProc, h, msg, w, l);
}

LRESULT QuickOpen::handleDockHostMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    // Clamp BEFORE forwarding WM_WINDOWPOSCHANGING so the docking manager
    // receives the corrected size instead of immediately overwriting it.
    if (msg == WM_WINDOWPOSCHANGING)
        clampWorkspaceWindowPos(h, reinterpret_cast<WINDOWPOS*>(l));

    LRESULT result = _oldDockHostProc ? CallWindowProcW(_oldDockHostProc, h, msg, w, l)
                                      : DefWindowProcW(h, msg, w, l);

    if (msg == WM_GETMINMAXINFO)
        clampWorkspaceMinMaxInfo(h, reinterpret_cast<MINMAXINFO*>(l));

    return result;
}

void QuickOpen::unregisterDock()
{
    if (!_npp || !_window || !_registeredDock) return;
    // Notepad++ unregisters docking widgets as part of plugin shutdown. Hiding
    // first avoids leaving a stale tab during plugin unload.
    SendMessageW(_npp, NPPM_DMMHIDE, 0, reinterpret_cast<LPARAM>(_window));
    _registeredDock = false;
}

void QuickOpen::createControls()
{
    // Two distinct native-looking group boxes keep the panel visually close to
    // Notepad++ dialogs: search on top, workspace actions underneath.
    _searchGroup = CreateWindowExW(0, L"BUTTON", L"Pesquisar",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        8, 4, 420, 102, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_GROUP)),
        GetModuleHandleW(nullptr), nullptr);

    _search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOHIDESEL,
        18, 24, 250, 26, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _runSearch = CreateWindowExW(0, L"BUTTON", L"Pesquisar",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        274, 24, 74, 26, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RUN_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _searchScopeButton = CreateWindowExW(0, L"BUTTON", L"Escopo",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        354, 24, 64, 26, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_SCOPE)),
        GetModuleHandleW(nullptr), nullptr);
    _contentSearchCheck = CreateWindowExW(0, L"BUTTON", L"Pesquisar dentro dos arquivos",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        18, 54, 220, 22, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CONTENT_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _cancelSearch = CreateWindowExW(0, L"BUTTON", L"Cancelar",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        342, 52, 76, 24, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CANCEL_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _searchProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        18, 80, 150, 14, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_PROGRESS)),
        GetModuleHandleW(nullptr), nullptr);
    _searchProgressText = CreateWindowExW(0, L"STATIC", L"Pronto",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        176, 74, 242, 24, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_PROGRESS_TEXT)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(_searchProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    EnableWindow(_cancelSearch, FALSE);
    setButtonChecked(_contentSearchCheck, _searchInsideFiles);

    _workspaceGroup = CreateWindowExW(0, L"BUTTON", L"Workspace",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        8, 110, 420, 52, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_WORKSPACE_GROUP)),
        GetModuleHandleW(nullptr), nullptr);

    _addFolder = CreateWindowExW(0, L"BUTTON", L"\xE710",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        18, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_ADD)),
        GetModuleHandleW(nullptr), nullptr);
    _newWorkspace = CreateWindowExW(0, L"BUTTON", L"\xE8A7",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        54, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_NEW)),
        GetModuleHandleW(nullptr), nullptr);
    _saveWorkspace = CreateWindowExW(0, L"BUTTON", L"\xE74E",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        90, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE)),
        GetModuleHandleW(nullptr), nullptr);
    _openWorkspace = CreateWindowExW(0, L"BUTTON", L"\xE8B7",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        126, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OPEN)),
        GetModuleHandleW(nullptr), nullptr);
    _removeFolder = CreateWindowExW(0, L"BUTTON", L"\xE74D",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        162, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REMOVE)),
        GetModuleHandleW(nullptr), nullptr);
    _expandAll = CreateWindowExW(0, L"BUTTON", L"\xE8A0",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        198, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EXPAND_ALL)),
        GetModuleHandleW(nullptr), nullptr);
    _collapseAll = CreateWindowExW(0, L"BUTTON", L"\xE8A1",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        234, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_COLLAPSE_ALL)),
        GetModuleHandleW(nullptr), nullptr);
    _createContainer = CreateWindowExW(0, L"BUTTON", L"\xE8B8",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        270, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CREATE_CONTAINER)),
        GetModuleHandleW(nullptr), nullptr);

    _tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
        TVS_SHOWSELALWAYS | TVS_INFOTIP,
        10, 170, 410, 500, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TREE)),
        GetModuleHandleW(nullptr), nullptr);

    _results = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS,
        10, 170, 410, 500, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RESULTS)),
        GetModuleHandleW(nullptr), nullptr);

    _viewFolders = CreateWindowExW(0, L"BUTTON", L"Pastas",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
        10, 602, 200, 26, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIEW_FOLDERS)),
        GetModuleHandleW(nullptr), nullptr);
    _viewSearch = CreateWindowExW(0, L"BUTTON", L"Pesquisa",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
        220, 602, 200, 26, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIEW_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);

    _status = CreateWindowExW(0, L"STATIC", L"Ctrl+B mostrar/ocultar  |  Ctrl+P pesquisar",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        10, 632, 410, 22, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)),
        GetModuleHandleW(nullptr), nullptr);

    for (HWND h : {_searchGroup, _workspaceGroup, _search, _runSearch, _contentSearchCheck, _searchProgressText,
                   _cancelSearch, _searchScopeButton, _tree, _results, _viewFolders, _viewSearch,
                   _addFolder, _newWorkspace, _saveWorkspace, _openWorkspace, _removeFolder,
                   _expandAll, _collapseAll, _createContainer, _status})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);

    for (HWND h : {_addFolder, _newWorkspace, _saveWorkspace, _openWorkspace, _removeFolder,
                   _expandAll, _collapseAll, _createContainer})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(_symbolFont), TRUE);

    SendMessageW(_search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Pesquisar arquivos..."));

    createTooltips();
    updatePanelSwitcher();

    ListView_SetExtendedListViewStyle(_results, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 220;
    col.pszText = const_cast<LPWSTR>(L"Arquivo");
    ListView_InsertColumn(_results, 0, &col);
    col.cx = 240;
    col.pszText = const_cast<LPWSTR>(L"Pasta");
    ListView_InsertColumn(_results, 1, &col);
    col.cx = 560;
    col.pszText = const_cast<LPWSTR>(L"Path completo");
    ListView_InsertColumn(_results, 2, &col);

    // Ask Notepad++ to apply its own native dark-mode treatment to the panel.
    SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                 static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(_window));

    _oldSearchProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        _search, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::editProc)));
}

void QuickOpen::createTooltips()
{
    if (_tooltips) return;
    _tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        _window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!_tooltips) return;
    SendMessageW(_tooltips, TTM_SETMAXTIPWIDTH, 0, 320);
    addButtonTooltip(_addFolder, L"Adicionar pasta ao NPPWorkSpace");
    addButtonTooltip(_newWorkspace, L"Criar um novo NPPWorkSpace");
    addButtonTooltip(_saveWorkspace, L"Salvar o NPPWorkSpace atual (.worknpp)");
    addButtonTooltip(_openWorkspace, L"Abrir um arquivo .worknpp");
    addButtonTooltip(_removeFolder, L"Remover a pasta selecionada");
    addButtonTooltip(_expandAll, L"Expandir pastas ou grupos de pesquisa");
    addButtonTooltip(_collapseAll, L"Retrair pastas ou grupos de pesquisa");
    addButtonTooltip(_createContainer, L"Criar contêiner de projeto");
    addButtonTooltip(_runSearch, L"Executar pesquisa");
    addButtonTooltip(_searchScopeButton, L"Escolher contêineres e pastas incluídos na pesquisa");
    addButtonTooltip(_contentSearchCheck, L"Quando marcado, pesquisa o conteúdo dos arquivos de texto suportados");
    addButtonTooltip(_cancelSearch, L"Cancelar a pesquisa em conteúdo em andamento");
    addButtonTooltip(_viewFolders, L"Mostrar pastas da Workspace sem limpar a pesquisa");
    addButtonTooltip(_viewSearch, L"Voltar aos resultados preservados da pesquisa");
}

void QuickOpen::addButtonTooltip(HWND button, const wchar_t* text)
{
    if (!_tooltips || !button) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = _window;
    ti.uId = reinterpret_cast<UINT_PTR>(button);
    ti.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(_tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

void QuickOpen::disableNativeFolderWorkspace()
{
    if (!_npp) return;
    HWND tree = findFolderWorkspaceTree();
    if (tree)
    {
        HWND panel = GetParent(tree);
        for (int i = 0; i < 8 && panel; ++i)
        {
            wchar_t title[256]{};
            GetWindowTextW(panel, title, static_cast<int>(std::size(title)));
            if (_wcsicmp(title, L"Folder as Workspace") == 0)
            {
                SendMessageW(_npp, NPPM_DMMHIDE, 0, reinterpret_cast<LPARAM>(panel));
                break;
            }
            panel = GetParent(panel);
        }
        // If the docking wrapper could not be found, at least hide the tree.
        ShowWindow(tree, SW_HIDE);
    }
}

void QuickOpen::syncNativeFolderWorkspace()
{
    if (!_npp) return;
    const auto roots = getNppWorkspaceRoots();
    bool changed = false;
    for (const auto& root : roots)
    {
        bool alreadyInContainer = false;
        for (const auto& container : _containers)
        {
            if (std::find(container.folders.begin(), container.folders.end(), root) != container.folders.end())
            {
                alreadyInContainer = true;
                break;
            }
        }
        if (!alreadyInContainer && std::find(_savedRoots.begin(), _savedRoots.end(), root) == _savedRoots.end())
        {
            _savedRoots.push_back(root);
            changed = true;
        }
    }
    if (changed)
    {
        writeWorkspaceFile();
        _nppRoots = roots;
        rebuildWorkspaceTree();
    }
    else if (_nppRoots != roots)
    {
        _nppRoots = roots;
        rebuildWorkspaceTree();
    }
    disableNativeFolderWorkspace();
}

void QuickOpen::layoutControls(int width, int height)
{
    if (!_window) return;
    width = (std::max)(static_cast<int>(WORKSPACE_MIN_WIDTH), width);
    height = (std::max)(static_cast<int>(WORKSPACE_MIN_HEIGHT), height);

    const int pad = 8;
    const int searchGroupH = 102;
    const int workspaceGroupY = 108;
    const int workspaceGroupH = 58;
    const int contentY = 172;
    const int statusH = 22;
    const int switcherH = 28;
    const int switcherY = height - statusH - switcherH - 8;
    const int contentH = (std::max)(60, switcherY - contentY - 6);

    MoveWindow(_searchGroup, pad, 4, width - 2 * pad, searchGroupH, TRUE);
    const int scopeButtonX = width - pad - 74;
    const int runButtonX = scopeButtonX - 86;
    MoveWindow(_search, pad + 10, 24, (std::max)(90, runButtonX - (pad + 10) - 6), 26, TRUE);
    MoveWindow(_runSearch, runButtonX, 24, 80, 26, TRUE);
    MoveWindow(_searchScopeButton, width - pad - 74, 24, 64, 26, TRUE);
    MoveWindow(_contentSearchCheck, pad + 10, 54, (std::max)(160, width - 2 * pad - 112), 22, TRUE);
    MoveWindow(_cancelSearch, width - pad - 86, 52, 76, 24, TRUE);
    MoveWindow(_searchProgress, pad + 10, 82, (std::max)(100, width / 3), 14, TRUE);
    MoveWindow(_searchProgressText, pad + 20 + (std::max)(100, width / 3), 76,
               (std::max)(120, width - 2 * pad - 30 - (std::max)(100, width / 3)), 24, TRUE);
    MoveWindow(_workspaceGroup, pad, workspaceGroupY, width - 2 * pad, workspaceGroupH, TRUE);

    constexpr int buttonSize = 32;
    constexpr int gap = 5;
    const int buttonY = 126;
    int x = pad + 10;
    HWND buttons[] = {_addFolder, _newWorkspace, _saveWorkspace, _openWorkspace, _removeFolder, _expandAll, _collapseAll, _createContainer};
    for (HWND button : buttons)
    {
        MoveWindow(button, x, buttonY, buttonSize, 30, TRUE);
        x += buttonSize + gap;
    }

    MoveWindow(_tree, pad, contentY, width - 2 * pad, contentH, TRUE);
    MoveWindow(_results, pad, contentY, width - 2 * pad, contentH, TRUE);
    const int switcherW = width - 2 * pad;
    const int half = (switcherW - 6) / 2;
    MoveWindow(_viewFolders, pad, switcherY, half, switcherH, TRUE);
    MoveWindow(_viewSearch, pad + half + 6, switcherY, switcherW - half - 6, switcherH, TRUE);
    MoveWindow(_status, pad, height - statusH - 2, width - 2 * pad, statusH, TRUE);
    configureResultsColumns(_results, _searchInsideFiles && _searchOnly);
}

void QuickOpen::updatePanelSwitcher()
{
    setButtonChecked(_viewFolders, !_resultsViewVisible);
    setButtonChecked(_viewSearch, _resultsViewVisible);
    const bool hasSearch = _searchOnly || !_searchResults.empty() || _searchQueryRunning;
    if (_viewSearch) EnableWindow(_viewSearch, hasSearch);
}

void QuickOpen::showFoldersPanel()
{
    _resultsViewVisible = false;
    if (_results) ShowWindow(_results, SW_HIDE);
    if (_tree) ShowWindow(_tree, SW_SHOW);
    updatePanelSwitcher();
    if (_tree) SetFocus(_tree);
    if (_searchOnly && _status)
        SetWindowTextW(_status, L"Pastas  •  resultados preservados em Pesquisa");
}

void QuickOpen::showResultsPanel()
{
    if (!_searchOnly && _searchResults.empty() && !_searchQueryRunning)
    {
        updatePanelSwitcher();
        if (_status) SetWindowTextW(_status, L"Nenhuma pesquisa executada");
        return;
    }

    _resultsViewVisible = true;
    if (_tree) ShowWindow(_tree, SW_HIDE);
    if (_results) ShowWindow(_results, SW_SHOW);
    configureResultsColumns(_results, _searchInsideFiles && _searchOnly);
    updatePanelSwitcher();
    if (_results) SetFocus(_results);
}

void QuickOpen::toggleWorkspace()
{
    createWindow();
    if (!isVisible()) showWorkspace();
    else hideWorkspace();
}

void QuickOpen::showWorkspace()
{
    createWindow();
    if (!_window) return;
    SendMessageW(_npp, NPPM_DMMSHOW, 0, reinterpret_cast<LPARAM>(_window));
    SetFocus(_tree);
}

void QuickOpen::hideWorkspace()
{
    if (!_npp || !_window) return;
    SendMessageW(_npp, NPPM_DMMHIDE, 0, reinterpret_cast<LPARAM>(_window));
}

void QuickOpen::focusSearch()
{
    createSearchPopup();
    showSearchPopup();
    SetFocus(_searchPopupEdit);
    SendMessageW(_searchPopupEdit, EM_SETSEL, 0, -1);
}

void QuickOpen::createSearchPopup()
{
    if (_searchPopup) return;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &QuickOpen::searchPopupProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = SEARCH_POPUP_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    _searchPopup = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
        SEARCH_POPUP_CLASS,
        L"NPPWorkSpace - Pesquisar",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 430,
        _npp, nullptr, GetModuleHandleW(nullptr), this);
    if (!_searchPopup) return;

    CreateWindowExW(0, L"STATIC", L"Pesquisar no NPPWorkSpace",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        12, 10, 300, 24, _searchPopup, nullptr, GetModuleHandleW(nullptr), nullptr);

    _searchPopupEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOHIDESEL,
        12, 38, 558, 30, _searchPopup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_POPUP_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _searchPopupRunSearch = CreateWindowExW(0, L"BUTTON", L"Pesquisar",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        576, 38, 86, 30, _searchPopup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_POPUP_RUN_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _searchPopupScopeButton = CreateWindowExW(0, L"BUTTON", L"Escopo",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        670, 38, 78, 30, _searchPopup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_SCOPE)),
        GetModuleHandleW(nullptr), nullptr);
    _searchPopupContentCheck = CreateWindowExW(0, L"BUTTON", L"Pesquisar dentro dos arquivos",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        12, 72, 230, 22, _searchPopup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_POPUP_CONTENT_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    setButtonChecked(_searchPopupContentCheck, _searchInsideFiles);
    SendMessageW(_searchPopupEdit, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    SendMessageW(_searchPopupRunSearch, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    SendMessageW(_searchPopupScopeButton, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    SendMessageW(_searchPopupContentCheck, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    SendMessageW(_searchPopupEdit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Nome do arquivo/pasta..."));

    _searchPopupResults = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        12, 100, 720, 288, _searchPopup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_POPUP_RESULTS)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(_searchPopupResults, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    ListView_SetExtendedListViewStyle(_searchPopupResults, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 220;
    col.pszText = const_cast<LPWSTR>(L"Arquivo");
    ListView_InsertColumn(_searchPopupResults, 0, &col);
    col.cx = 240;
    col.pszText = const_cast<LPWSTR>(L"Pasta");
    ListView_InsertColumn(_searchPopupResults, 1, &col);
    col.cx = 560;
    col.pszText = const_cast<LPWSTR>(L"Path completo");
    ListView_InsertColumn(_searchPopupResults, 2, &col);

    _searchPopupHint = CreateWindowExW(0, L"STATIC", L"Enter pesquisar  |  Na lista: Enter abrir  |  Esc fechar",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        12, 394, 300, 20, _searchPopup, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(_searchPopupHint, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);

    SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                 static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(_searchPopup));

    _oldPopupSearchProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        _searchPopupEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::editProc)));
}

void QuickOpen::layoutSearchPopup()
{
    if (!_searchPopup) return;
    RECT rc{};
    GetClientRect(_searchPopup, &rc);
    const int w = (std::max)(420, static_cast<int>(rc.right - rc.left));
    const int h = (std::max)(250, static_cast<int>(rc.bottom - rc.top));
    const int scopeButtonX = w - 90;
    const int runButtonX = scopeButtonX - 94;
    MoveWindow(_searchPopupEdit, 12, 38, (std::max)(140, runButtonX - 20), 30, TRUE);
    MoveWindow(_searchPopupRunSearch, runButtonX, 38, 86, 30, TRUE);
    MoveWindow(_searchPopupScopeButton, w - 90, 38, 78, 30, TRUE);
    MoveWindow(_searchPopupContentCheck, 12, 72, w - 24, 22, TRUE);
    MoveWindow(_searchPopupResults, 12, 100, w - 24, h - 140, TRUE);
    MoveWindow(_searchPopupHint, 12, h - 32, w - 24, 20, TRUE);
    configureResultsColumns(_searchPopupResults, _searchInsideFiles);
}

void QuickOpen::showSearchPopup()
{
    createSearchPopup();
    if (!_searchPopup) return;
    // Ctrl+P must reuse the already-built index. Rebuilding/invalidation on every
    // invocation made the first query race with the index worker and caused
    // subsequent queries to appear stale or empty. The index is invalidated only
    // when the workspace/scope actually changes.
    if (_searchPopupEdit) SetWindowTextW(_searchPopupEdit, L"");
    if (_searchPopupResults) ListView_DeleteAllItems(_searchPopupResults);
    _searchResults.clear();
    _contentSearchGroups.clear();
    _contentSearchGroupByPath.clear();
    configureResultsColumns(_searchPopupResults, false);
    if (_searchPopupContentCheck)
        setButtonChecked(_searchPopupContentCheck, _searchInsideFiles);
    if (!_searchIndexValid && !_searchIndexBuilding)
        startSearchIndexBuild();
    layoutSearchPopup();
    RECT npp{};
    GetWindowRect(_npp, &npp);
    const int width = 760;
    const int height = 430;
    const int x = npp.left + ((npp.right - npp.left) - width) / 2;
    const int y = npp.top + ((npp.bottom - npp.top) - height) / 3;
    SetWindowPos(_searchPopup, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
    SetForegroundWindow(_searchPopup);
}

void QuickOpen::hideSearchPopup()
{
    if (_searchPopup)
    {
        if (_contentSearchPopup) cancelContentSearch();
        ShowWindow(_searchPopup, SW_HIDE);
    }
}

void QuickOpen::updatePopupSearch()
{
    if (!_searchPopupEdit) return;
    const std::wstring query = getWindowText(_searchPopupEdit);
    if (query.empty())
    {
        if (_contentSearchPopup) cancelContentSearch();
        _searchResults.clear();
        _contentSearchGroups.clear();
        _contentSearchGroupByPath.clear();
        ListView_DeleteAllItems(_searchPopupResults);
        configureResultsColumns(_searchPopupResults, false);
        return;
    }
    showPopupSearchResults(query);
}


void QuickOpen::startContentSearch(const std::wstring& query, bool popup)
{
    if (!isContentSearchEnabled(popup) || query.size() < 2 || !_window) return;

    if (_searchQueryRunning)
    {
        _pendingContentQuery = query;
        _pendingContentPopup = popup;
        _searchQueryGeneration.fetch_add(1);
        return;
    }
    if (_searchQueryThread.joinable())
        _searchQueryThread.join();

    _searchQueryRunning = true;
    const unsigned int generation = _searchQueryGeneration.fetch_add(1) + 1;
    _runningContentQuery = query;
    _runningContentPopup = popup;
    _contentSearchPopup = popup;
    _contentSearchProcessed = 0;
    _contentSearchTotal = 0;
    _contentSearchHits = 0;

    std::vector<std::filesystem::path> roots;
    if (_searchIncludedPaths.empty())
    {
        roots = getWorkspaceRootsForPanel();
    }
    else
    {
        roots.reserve(_searchIncludedPaths.size());
        for (const auto& included : _searchIncludedPaths)
            roots.emplace_back(included);
    }
    const auto included = _searchIncludedPaths;
    const auto disabled = _searchDisabledPaths;
    const HWND target = _window;
    const std::wstring queryLower = lowerText(query);
    const bool asciiQuery = isAsciiString(query);
    const std::string asciiNeedle = asciiQuery ? utf8FromWide(query) : std::string();

    EnableWindow(_cancelSearch, TRUE);
    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, 0, 0);
    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Preparando pesquisa...");
    if (popup && _searchPopupHint) SetWindowTextW(_searchPopupHint, L"Pesquisando...");

    QO_DEBUG_LOG(L"Início da pesquisa em conteúdo");
    QO_DEBUG_LOG(L"Filtro: " + query);

    _searchQueryThread = std::thread([this, query, queryLower, asciiQuery, asciiNeedle,
                                      roots, included, disabled, popup, generation, target]()
    {
#ifdef _DEBUG
        const auto started = std::chrono::steady_clock::now();
#endif

        struct FileJob
        {
            std::filesystem::path path;
            std::wstring fileName;
            std::wstring folder;
            std::wstring relative;
        };

        std::deque<FileJob> jobs;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> enumerationDone{false};
        std::atomic<size_t> total{0};
        std::atomic<size_t> processed{0};
        std::atomic<size_t> hits{0};

        auto stillCurrent = [&]()
        {
            return _searchQueryGeneration.load() == generation;
        };

        auto enabled = [&](const std::filesystem::path& path)
        {
            return isPathEnabledForScope(path, included, disabled);
        };

        auto postProgress = [&](const std::wstring& currentFile)
        {
            if (!target) return;
            auto* progress = new ContentSearchProgress();
            progress->generation = generation;
            progress->popup = popup;
            progress->processed = processed.load();
            progress->total = total.load();
            progress->hits = hits.load();
            progress->currentFile = currentFile;
            if (!PostMessageW(target, WM_CONTENT_SEARCH_PROGRESS, 0, reinterpret_cast<LPARAM>(progress)))
                delete progress;
        };

        auto postBatch = [&](std::vector<ContentFileGroup>& batch)
        {
            if (batch.empty() || !target) return;
            auto* message = new ContentSearchBatch();
            message->generation = generation;
            message->popup = popup;
            message->files.swap(batch);
            if (!PostMessageW(target, WM_CONTENT_SEARCH_BATCH, 0, reinterpret_cast<LPARAM>(message)))
                delete message;
        };

        auto makeJob = [](const std::filesystem::path& root, const std::filesystem::path& path)
        {
            FileJob job;
            job.path = path;
            job.fileName = path.filename().wstring();
            if (job.fileName.empty()) job.fileName = path.wstring();

            std::error_code relEc;
            std::filesystem::path relPath = std::filesystem::relative(path, root, relEc);
            if (relEc || relPath.empty()) relPath = path.filename();
            job.relative = relPath.wstring();
            std::filesystem::path folderPath = relPath.parent_path();
            job.folder = folderPath.empty() ? L"\\" : L"\\" + folderPath.wstring();
            std::replace(job.folder.begin(), job.folder.end(), L'/', L'\\');
            return job;
        };

        auto scanFile = [&](const FileJob& job)
        {
            ContentFileGroup group;
            group.path = job.path;
            group.fileName = job.fileName;
            group.folder = job.folder;
            group.relative = job.relative;

#ifdef _DEBUG
            const auto fileStart = std::chrono::steady_clock::now();
#endif
            std::error_code ec;
            const auto size = std::filesystem::file_size(job.path, ec);
            if (ec || size == 0 || size > MAX_CONTENT_FILE_SIZE)
                return group;

            std::ifstream in(job.path, std::ios::binary);
            if (!in) return group;

            std::vector<unsigned char> bytes(static_cast<size_t>(size));
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            const std::streamsize got = in.gcount();
            if (got <= 0) return group;
            bytes.resize(static_cast<size_t>(got));

            if (asciiQuery && !hasUtf16Pattern(bytes) && !containsAsciiBytesInsensitive(bytes, asciiNeedle))
                return group;

            std::wstring text;
            TextEncoding encoding = TextEncoding::Unknown;
            if (!decodeTextBytes(bytes, text, encoding))
                return group;

            size_t lineNumber = 1;
            size_t lineStart = 0;
            while (lineStart <= text.size() && stillCurrent())
            {
                size_t lineEnd = text.find(L'\n', lineStart);
                if (lineEnd == std::wstring::npos) lineEnd = text.size();
                size_t logicalEnd = lineEnd;
                if (logicalEnd > lineStart && text[logicalEnd - 1] == L'\r')
                    --logicalEnd;

                std::wstring line = text.substr(lineStart, logicalEnd - lineStart);
                std::wstring lineLower = lowerText(line);

                size_t searchFrom = 0;
                size_t occurrenceOnLine = 0;
                while (stillCurrent())
                {
                    const size_t pos = lineLower.find(queryLower, searchFrom);
                    if (pos == std::wstring::npos) break;

                    const std::wstring matchText = line.substr(pos, (std::min)(query.size(), line.size() - pos));
                    SnippetInfo snippet = makeSnippet(line, pos, matchText.size());
                    group.matches.push_back({lineNumber, std::move(snippet.text), snippet.matchStart,
                                             matchText.size(), occurrenceOnLine, matchText});
                    searchFrom = pos + (std::max)(size_t(1), queryLower.size());
                    ++occurrenceOnLine;
                }

                if (lineEnd == text.size()) break;
                lineStart = lineEnd + 1;
                ++lineNumber;
            }

#ifdef _DEBUG
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - fileStart).count();
            QO_DEBUG_LOG(L"Arquivo: " + job.relative + L" | encoding=" + encodingName(encoding) +
                         L" | tempo=" + std::to_wstring(elapsed) + L"ms | ocorrências=" +
                         std::to_wstring(group.matches.size()));
#endif
            return group;
        };

        auto worker = [&]()
        {
            std::vector<ContentFileGroup> batch;
            batch.reserve(CONTENT_BATCH_FILE_COUNT);

            while (stillCurrent())
            {
                FileJob job;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&]() { return !jobs.empty() || enumerationDone.load() || !stillCurrent(); });
                    if (!stillCurrent()) break;
                    if (jobs.empty())
                    {
                        if (enumerationDone.load()) break;
                        continue;
                    }
                    job = std::move(jobs.front());
                    jobs.pop_front();
                    cv.notify_one();
                }

                postProgress(job.relative);
                ContentFileGroup group = scanFile(job);
                const size_t fileHits = group.matches.size();
                if (fileHits > 0)
                {
                    hits.fetch_add(fileHits);
                    batch.push_back(std::move(group));
                    if (batch.size() >= CONTENT_BATCH_FILE_COUNT)
                        postBatch(batch);
                }
                processed.fetch_add(1);
                postProgress(job.relative);
            }

            if (stillCurrent())
                postBatch(batch);
        };

        const unsigned int hardware = std::thread::hardware_concurrency();
        const size_t requestedWorkers = static_cast<size_t>(hardware ? hardware : 2);
        const size_t workerCount = (std::max)(size_t(1), (std::min)(size_t(4), requestedWorkers));
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i)
            workers.emplace_back(worker);

        std::unordered_set<std::wstring> seen;
        for (const auto& root : roots)
        {
            if (!stillCurrent()) break;
            QO_DEBUG_LOG(L"Diretório: " + root.wstring());
            std::error_code ec;
            std::filesystem::recursive_directory_iterator it(
                root, std::filesystem::directory_options::skip_permission_denied, ec);
            for (; it != std::filesystem::recursive_directory_iterator{} && !ec && stillCurrent(); it.increment(ec))
            {
                const auto path = it->path();
                std::error_code itemEc;
                if (it->is_directory(itemEc))
                {
                    if (isHiddenSystemDirectory(path)) it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(itemEc) || !isSupportedTextExtension(path) || !enabled(path))
                    continue;

                std::wstring key = lowerText(path.lexically_normal().wstring());
                std::replace(key.begin(), key.end(), L'/', L'\\');
                if (!seen.insert(key).second) continue;

                FileJob job = makeJob(root, path);
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&]() { return jobs.size() < CONTENT_QUEUE_LIMIT || !stillCurrent(); });
                    if (!stillCurrent()) break;
                    jobs.push_back(std::move(job));
                    total.fetch_add(1);
                }
                cv.notify_one();
            }
        }

        enumerationDone = true;
        cv.notify_all();
        for (auto& thread : workers)
            if (thread.joinable()) thread.join();

        _searchQueryRunning = false;

        auto* complete = new ContentSearchComplete();
        complete->generation = generation;
        complete->popup = popup;
        complete->cancelled = _searchQueryGeneration.load() != generation;
        complete->processed = processed.load();
        complete->total = total.load();
        complete->hits = hits.load();

#ifdef _DEBUG
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        QO_DEBUG_LOG((complete->cancelled ? L"Pesquisa cancelada" : L"Pesquisa concluída") +
                     (L" | arquivos=" + std::to_wstring(complete->processed) + L"/" +
                      std::to_wstring(complete->total) + L" | ocorrências=" +
                      std::to_wstring(complete->hits) + L" | tempo total=" +
                      std::to_wstring(elapsed) + L"ms"));
#endif

        if (!target || !PostMessageW(target, WM_CONTENT_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(complete)))
            delete complete;
    });
}

void QuickOpen::applyContentSearchBatch(ContentSearchBatch* batch)
{
    std::unique_ptr<ContentSearchBatch> holder(batch);
    if (!holder || holder->generation != _searchQueryGeneration.load()) return;

    HWND list = holder->popup ? _searchPopupResults : _results;
    if (!list) return;

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    for (auto& group : holder->files)
        appendContentSearchGroup(std::move(group), list);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, FALSE);

    if (!_searchResults.empty() && ListView_GetSelectedCount(list) == 0)
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void QuickOpen::updateContentSearchProgress(ContentSearchProgress* progress)
{
    std::unique_ptr<ContentSearchProgress> holder(progress);
    if (!holder || holder->generation != _searchQueryGeneration.load()) return;

    _contentSearchProcessed = holder->processed;
    _contentSearchTotal = holder->total;
    _contentSearchHits = holder->hits;

    const int percent = holder->total > 0
        ? static_cast<int>((holder->processed * 100) / holder->total)
        : 0;
    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, percent, 0);

    std::wstring text = std::to_wstring(holder->processed) + L"/" +
        std::to_wstring(holder->total) + L" (" + std::to_wstring(percent) + L"%)";
    if (!holder->currentFile.empty())
        text += L"  Pesquisando: " + holder->currentFile;
    if (_searchProgressText) SetWindowTextW(_searchProgressText, text.c_str());

    if (holder->popup && _searchPopupHint)
    {
        std::wstring hint = std::to_wstring(holder->hits) + L" ocorrência(s)  |  " + text;
        SetWindowTextW(_searchPopupHint, hint.c_str());
    }
    if (!holder->popup)
    {
        std::wstring status = std::to_wstring(holder->hits) + L" ocorrência(s) em " +
            std::to_wstring(_contentSearchGroups.size()) + L" arquivo(s)";
        SetWindowTextW(_status, status.c_str());
    }
}

void QuickOpen::completeContentSearch(ContentSearchComplete* complete)
{
    std::unique_ptr<ContentSearchComplete> holder(complete);

    if (!_pendingContentQuery.empty() && !_searchQueryRunning)
    {
        const std::wstring query = _pendingContentQuery;
        const bool popup = _pendingContentPopup;
        _pendingContentQuery.clear();
        _pendingContentPopup = false;
        resetContentSearchResults(popup);
        startContentSearch(query, popup);
        return;
    }

    if (!holder || holder->generation != _searchQueryGeneration.load())
        return;

    _contentSearchProcessed = holder->processed;
    _contentSearchTotal = holder->total;
    _contentSearchHits = holder->hits;
    EnableWindow(_cancelSearch, FALSE);

    const int percent = holder->total > 0
        ? static_cast<int>((holder->processed * 100) / holder->total)
        : 100;
    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, percent, 0);

    std::wstring text = (holder->cancelled ? L"Pesquisa cancelada: " : L"Pesquisa concluída: ");
    text += std::to_wstring(holder->processed) + L"/" + std::to_wstring(holder->total) +
        L" arquivo(s), " + std::to_wstring(holder->hits) + L" ocorrência(s)";
    if (_searchProgressText) SetWindowTextW(_searchProgressText, text.c_str());

    if (holder->popup && _searchPopupHint)
        SetWindowTextW(_searchPopupHint, text.c_str());
    if (!holder->popup)
        SetWindowTextW(_status, (text + L"  •  Enter abrir  •  Esc limpar pesquisa").c_str());
}

void QuickOpen::cancelContentSearch()
{
    _pendingContentQuery.clear();
    _pendingContentPopup = false;
    if (!_searchQueryRunning)
    {
        EnableWindow(_cancelSearch, FALSE);
        return;
    }

    _searchQueryGeneration.fetch_add(1);
    EnableWindow(_cancelSearch, FALSE);
    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Pesquisa cancelada. Resultados preservados.");
    if (_searchPopupHint) SetWindowTextW(_searchPopupHint, L"Pesquisa cancelada. Resultados preservados.");
    SetWindowTextW(_status, L"Pesquisa cancelada. Resultados preservados.");
    QO_DEBUG_LOG(L"Cancelamento solicitado");
}

bool QuickOpen::isContentSearchEnabled(bool /*popup*/) const
{
    return _searchInsideFiles;
}

void QuickOpen::setContentSearchEnabled(bool enabled)
{
    if (_searchInsideFiles == enabled)
    {
        setButtonChecked(_contentSearchCheck, enabled);
        setButtonChecked(_searchPopupContentCheck, enabled);
        return;
    }

    _searchInsideFiles = enabled;
    setButtonChecked(_contentSearchCheck, enabled);
    setButtonChecked(_searchPopupContentCheck, enabled);
    _runSearchAfterIndexBuild = false;
    _runSearchAfterIndexPopup = false;

    cancelContentSearch();
    _searchResults.clear();
    _contentSearchGroups.clear();
    _contentSearchGroupByPath.clear();

    if (_results)
    {
        configureResultsColumns(_results, enabled);
        ListView_DeleteAllItems(_results);
    }
    if (_searchPopupResults)
    {
        configureResultsColumns(_searchPopupResults, enabled);
        ListView_DeleteAllItems(_searchPopupResults);
    }

    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, 0, 0);
    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Pressione Pesquisar ou Enter");
    if (_searchPopupHint) SetWindowTextW(_searchPopupHint, L"Enter pesquisar  |  Na lista: Enter abrir  |  Esc fechar");
    if (_searchOnly) SetWindowTextW(_status, L"Modo de pesquisa alterado. Pressione Pesquisar ou Enter.");
}

void QuickOpen::resetContentSearchResults(bool popup)
{
    HWND list = popup ? _searchPopupResults : _results;
    if (!list) return;

    _contentSearchGroups.clear();
    _contentSearchGroupByPath.clear();
    _searchResults.clear();
    configureResultsColumns(list, true);
    ListView_DeleteAllItems(list);
    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, 0, 0);
    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Preparando pesquisa...");
    if (popup && _searchPopupHint) SetWindowTextW(_searchPopupHint, L"Pesquisando...");
}

void QuickOpen::configureResultsColumns(HWND list, bool contentMode)
{
    if (!list) return;

    RECT rc{};
    GetClientRect(list, &rc);
    const int available = (std::max)(320, static_cast<int>(rc.right - rc.left) - GetSystemMetrics(SM_CXVSCROLL) - 8);
    const int firstWidth = contentMode ? (std::max)(170, available * 28 / 100)
                                       : (std::max)(180, available * 25 / 100);
    const int secondWidth = contentMode ? (std::max)(82, available * 14 / 100)
                                        : (std::max)(170, available * 28 / 100);
    const int thirdWidth = (std::max)(160, available - firstWidth - secondWidth);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = firstWidth;
    col.pszText = const_cast<LPWSTR>(contentMode ? L"Arquivo / linha" : L"Arquivo");
    ListView_SetColumn(list, 0, &col);
    col.cx = secondWidth;
    col.pszText = const_cast<LPWSTR>(contentMode ? L"Linha" : L"Pasta");
    ListView_SetColumn(list, 1, &col);
    col.cx = thirdWidth;
    col.pszText = const_cast<LPWSTR>(contentMode ? L"Trecho / path" : L"Path completo");
    ListView_SetColumn(list, 2, &col);
}

void QuickOpen::appendContentSearchGroup(ContentFileGroup&& group, HWND list)
{
    if (group.matches.empty()) return;

    std::wstring key = lowerText(group.path.lexically_normal().wstring());
    std::replace(key.begin(), key.end(), L'/', L'\\');
    if (auto it = _contentSearchGroupByPath.find(key); it != _contentSearchGroupByPath.end())
    {
        auto& existing = _contentSearchGroups[it->second];
        existing.matches.insert(existing.matches.end(),
                                std::make_move_iterator(group.matches.begin()),
                                std::make_move_iterator(group.matches.end()));
        rebuildContentResultsList(list);
        return;
    }

    const size_t groupIndex = _contentSearchGroups.size();
    _contentSearchGroupByPath[key] = groupIndex;
    _contentSearchGroups.push_back(std::move(group));

    auto appendRow = [&](const SearchResult& result)
    {
        const int row = static_cast<int>(_searchResults.size());
        _searchResults.push_back(result);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        std::wstring col0;
        std::wstring col1;
        std::wstring col2;
        const SearchResult& visible = _searchResults.back();
        if (visible.rowType == SearchResult::RowType::ContentFile)
        {
            col0 = (visible.contentFileIndex < _contentSearchGroups.size() &&
                    _contentSearchGroups[visible.contentFileIndex].collapsed ? L"[+] " : L"[-] ") +
                visible.fileName + L" (" + std::to_wstring(visible.lineNumber) + L")";
            col1 = visible.folder;
            col2 = visible.path.wstring();
        }
        else
        {
            col0 = L"    Linha " + std::to_wstring(visible.lineNumber);
            col1 = std::to_wstring(visible.lineNumber);
            col2 = visible.snippet;
        }
        item.pszText = const_cast<LPWSTR>(col0.c_str());
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, row, 1, const_cast<LPWSTR>(col1.c_str()));
        ListView_SetItemText(list, row, 2, const_cast<LPWSTR>(col2.c_str()));
    };

    const auto& stored = _contentSearchGroups.back();
    SearchResult header{};
    header.path = stored.path;
    header.fileName = stored.fileName;
    header.folder = stored.folder;
    header.relative = stored.relative;
    header.rowType = SearchResult::RowType::ContentFile;
    header.contentFileIndex = groupIndex;
    header.lineNumber = stored.matches.size();
    appendRow(header);

    if (!stored.collapsed)
    {
        for (size_t i = 0; i < stored.matches.size(); ++i)
        {
            const auto& match = stored.matches[i];
            SearchResult line{};
            line.path = stored.path;
            line.fileName = stored.fileName;
            line.folder = stored.folder;
            line.relative = stored.relative;
            line.rowType = SearchResult::RowType::ContentLine;
            line.contentFileIndex = groupIndex;
            line.contentMatchIndex = i;
            line.lineNumber = match.lineNumber;
            line.snippet = match.snippet;
            line.matchStart = match.matchStart;
            line.matchLength = match.matchLength;
            line.occurrenceOnLine = match.occurrenceOnLine;
            line.matchText = match.matchText;
            appendRow(line);
        }
    }
}

void QuickOpen::rebuildContentResultsList(HWND list)
{
    if (!list) return;

    std::vector<ContentFileGroup> groups = std::move(_contentSearchGroups);
    _contentSearchGroups.clear();
    _contentSearchGroupByPath.clear();
    _searchResults.clear();
    ListView_DeleteAllItems(list);

    for (auto& group : groups)
        appendContentSearchGroup(std::move(group), list);
}

void QuickOpen::toggleContentGroupFromRow(HWND list, int row)
{
    if (row < 0 || row >= static_cast<int>(_searchResults.size())) return;
    const SearchResult& result = _searchResults[static_cast<size_t>(row)];
    if (result.rowType != SearchResult::RowType::ContentFile ||
        result.contentFileIndex >= _contentSearchGroups.size())
        return;

    _contentSearchGroups[result.contentFileIndex].collapsed =
        !_contentSearchGroups[result.contentFileIndex].collapsed;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    rebuildContentResultsList(list);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, FALSE);
}

void QuickOpen::showPopupSearchResults(const std::wstring& query)
{
    if (isContentSearchEnabled(true))
    {
        resetContentSearchResults(true);
        if (query.size() < 2)
        {
            cancelContentSearch();
            if (_searchPopupHint) SetWindowTextW(_searchPopupHint, L"Digite ao menos 2 caracteres");
            return;
        }
        startContentSearch(query, true);
        return;
    }

    cancelContentSearch();
    configureResultsColumns(_searchPopupResults, false);

    if (!_searchIndexValid)
    {
        if (!_searchIndexBuilding) startSearchIndexBuild();
        _runSearchAfterIndexBuild = true;
        _runSearchAfterIndexPopup = true;
        SetWindowTextW(_status, L"Indexando arquivos... aguarde");
        return;
    }

    const std::wstring q = lower(query);
    std::vector<std::pair<int, size_t>> ranked;
    ranked.reserve(MAX_SEARCH_RESULTS * 4);
    for (size_t i = 0; i < _searchIndex.size(); ++i)
    {
        const auto& item = _searchIndex[i];
        if (!isSearchPathEnabled(item.path)) continue;
        const int fileScore = fuzzyScoreLower(q, item.fileNameLower);
        const int pathScore = fuzzyScoreLower(q, item.relativeLower);
        const int score = (std::max)(fileScore, pathScore);
        if (score >= 0)
        {
            ranked.emplace_back(score, i);
            if (ranked.size() >= static_cast<size_t>(MAX_SEARCH_RESULTS * 4)) break;
        }
    }

    const size_t wanted = (std::min)(ranked.size(), static_cast<size_t>(MAX_SEARCH_RESULTS));
    std::partial_sort(ranked.begin(), ranked.begin() + wanted, ranked.end(),
        [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });

    _searchResults.clear();
    _searchResults.reserve(wanted);
    for (size_t i = 0; i < wanted; ++i)
        _searchResults.push_back(_searchIndex[ranked[i].second]);

    ListView_DeleteAllItems(_searchPopupResults);
    for (size_t i = 0; i < _searchResults.size(); ++i)
    {
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(_searchResults[i].fileName.c_str());
        ListView_InsertItem(_searchPopupResults, &item);
        ListView_SetItemText(_searchPopupResults, static_cast<int>(i), 1,
                             const_cast<LPWSTR>(_searchResults[i].folder.c_str()));
        const std::wstring fullPath = _searchResults[i].path.wstring();
        ListView_SetItemText(_searchPopupResults, static_cast<int>(i), 2,
                             const_cast<LPWSTR>(fullPath.c_str()));
    }
    if (!_searchResults.empty())
        ListView_SetItemState(_searchPopupResults, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
}

void QuickOpen::openPopupSearchResult()
{
    const int row = ListView_GetNextItem(_searchPopupResults, -1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(_searchResults.size())) return;
    openSearchResultRow(_searchResults[static_cast<size_t>(row)]);
    hideSearchPopup();
}

void QuickOpen::refreshWorkspace()
{
    createWindow();
    _nppRoots = getNppWorkspaceRoots();
    loadWorkspace();
    for (const auto& root : _nppRoots)
        if (std::find(_savedRoots.begin(), _savedRoots.end(), root) == _savedRoots.end())
            _savedRoots.push_back(root);
    writeWorkspaceFile();
    rebuildWorkspaceTree();
}

void QuickOpen::clearTreeData()
{
    if (_tree)
    {
        for (const auto& entry : _nodeData)
        {
            TVITEMW tv{};
            tv.mask = TVIF_PARAM;
            tv.hItem = entry.first;
            if (TreeView_GetItem(_tree, &tv) && tv.lParam && tv.lParam != ID_NODE_DUMMY)
                delete reinterpret_cast<NodeData*>(tv.lParam);
        }
    }
    _selectedTreeFiles.clear();
    _nodeData.clear();
}

void QuickOpen::rebuildWorkspaceTree(bool /*preserveExpansion*/)
{
    invalidateSearchIndex();
    if (!_tree) return;
    clearTreeData();
    TreeView_DeleteAllItems(_tree);

    // NPPWorkSpace is the authoritative workspace. Roots discovered in
    // Notepad++'s legacy Folder as Workspace are imported into _savedRoots
    // and the native panel is kept hidden.
    for (size_t i = 0; i < _containers.size(); ++i)
        addContainerToTree(i);

    for (const auto& root : _savedRoots)
        addRootToTree(root, false);

    _searchOnly = false;
    showFoldersPanel();

    std::wstring status = L"NPPWorkSpace  •  " + std::to_wstring(_containers.size()) + L" projeto(s)  •  " + std::to_wstring(_savedRoots.size()) + L" pasta(s)  •  Ctrl+B mostrar/ocultar  •  Ctrl+P pesquisar";
    SetWindowTextW(_status, status.c_str());
}

void QuickOpen::addContainerToTree(size_t containerIndex)
{
    if (containerIndex >= _containers.size()) return;
    const auto& container = _containers[containerIndex];
    addNode(_tree, TVI_ROOT, container.name, {}, NodeType::Container, false, !container.folders.empty(), containerIndex);
    HTREEITEM item = nullptr;
    // Locate the newly inserted container by walking the top-level nodes.
    // container by walking the top-level nodes instead.
    for (HTREEITEM current = TreeView_GetRoot(_tree); current; current = TreeView_GetNextSibling(_tree, current))
    {
        auto it = _nodeData.find(current);
        if (it != _nodeData.end() && it->second.type == NodeType::Container && it->second.containerIndex == containerIndex)
        {
            item = current;
            break;
        }
    }
    if (!item || container.folders.empty()) return;
    // Replace the lazy dummy with the actual folders so containers behave like
    // regular expandable project folders.
    HTREEITEM child = TreeView_GetChild(_tree, item);
    if (child) TreeView_DeleteItem(_tree, child);
    for (const auto& folder : container.folders)
        addRootToTree(folder, false, item, containerIndex);
}

void QuickOpen::addRootToTree(const std::filesystem::path& root, bool fromNppWorkspace, HTREEITEM parent, size_t containerIndex)
{
    if (root.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;

    const std::wstring label = root.filename().wstring().empty() ? root.wstring() : root.filename().wstring();
    addNode(_tree, parent, label, root, NodeType::Root, fromNppWorkspace, true, containerIndex);
}

void QuickOpen::addNode(HWND tree, HTREEITEM parent, const std::wstring& label,
                        const std::filesystem::path& path, NodeType type,
                        bool fromNppWorkspace, bool hasChildren, size_t containerIndex)
{
    NodeData node{};
    node.type = type;
    node.path = path;
    node.fromNppWorkspace = fromNppWorkspace;
    node.containerIndex = containerIndex;
    node.label = label;
    node.hasChildren = hasChildren;

    const std::wstring displayLabel = formatTreeNodeLabel(node, false);

    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = const_cast<LPWSTR>(displayLabel.c_str());

    auto* data = new NodeData(node);
    ins.item.lParam = reinterpret_cast<LPARAM>(data);
    HTREEITEM item = TreeView_InsertItem(tree, &ins);
    _nodeData[item] = node;

    if (hasChildren)
    {
        TVINSERTSTRUCTW dummy{};
        dummy.hParent = item;
        dummy.hInsertAfter = TVI_LAST;
        dummy.item.mask = TVIF_TEXT | TVIF_PARAM;
        dummy.item.pszText = const_cast<LPWSTR>(L"...");
        dummy.item.lParam = ID_NODE_DUMMY;
        TreeView_InsertItem(tree, &dummy);
    }
}

std::wstring QuickOpen::formatTreeNodeLabel(const NodeData& data, bool expanded) const
{
    if (data.type == NodeType::File || !data.hasChildren)
        return data.label;
    return std::wstring(expanded ? L"[-] " : L"[+] ") + data.label;
}

void QuickOpen::refreshTreeNodeLabel(HTREEITEM item, bool expanded)
{
    if (!_tree || !item) return;
    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;

    std::wstring label = formatTreeNodeLabel(it->second, expanded);
    TVITEMW tv{};
    tv.mask = TVIF_TEXT;
    tv.hItem = item;
    tv.pszText = label.data();
    TreeView_SetItem(_tree, &tv);
}

void QuickOpen::expandNode(HTREEITEM item)
{
    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;
    if (it->second.type == NodeType::File) return;

    HTREEITEM child = TreeView_GetChild(_tree, item);
    if (child)
    {
        TVITEMW tv{};
        tv.mask = TVIF_PARAM;
        tv.hItem = child;
        TreeView_GetItem(_tree, &tv);
        if (tv.lParam == ID_NODE_DUMMY)
        {
            TreeView_DeleteItem(_tree, child);
            addDirectoryChildren(_tree, item, it->second.path, it->second.fromNppWorkspace);
        }
    }
}

void QuickOpen::addDirectoryChildren(HWND tree, HTREEITEM parent,
                                     const std::filesystem::path& directory,
                                     bool fromNppWorkspace)
{
    std::vector<std::filesystem::directory_entry> dirs;
    std::vector<std::filesystem::directory_entry> files;
    std::error_code ec;

    std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; it != std::filesystem::directory_iterator{} && !ec; it.increment(ec))
    {
        const auto& entry = *it;
        const auto path = entry.path();
        std::error_code itemEc;
        if (entry.is_directory(itemEc))
        {
            if (!isHiddenSystemDirectory(path)) dirs.push_back(entry);
        }
        else if (entry.is_regular_file(itemEc))
        {
            files.push_back(entry);
        }
    }

    auto byName = [](const auto& a, const auto& b)
    {
        std::wstring an = lower(a.path().filename().wstring());
        std::wstring bn = lower(b.path().filename().wstring());
        return an < bn;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    for (const auto& d : dirs)
    {
        std::error_code childEc;
        bool hasChildren = std::filesystem::directory_iterator(d.path(),
            std::filesystem::directory_options::skip_permission_denied, childEc) != std::filesystem::directory_iterator{};
        addNode(tree, parent, d.path().filename().wstring(), d.path(), NodeType::Folder,
                fromNppWorkspace, hasChildren);
    }

    for (const auto& f : files)
        addNode(tree, parent, f.path().filename().wstring(), f.path(), NodeType::File,
                fromNppWorkspace, false);
}

void QuickOpen::populateDirectory(HWND tree, HTREEITEM parent,
                                  const std::filesystem::path& directory,
                                  bool rootNode, bool fromNppWorkspace,
                                  std::unordered_map<HTREEITEM, NodeData>& /*nodes*/)
{
    if (rootNode) addRootToTree(directory, fromNppWorkspace);
    else addDirectoryChildren(tree, parent, directory, fromNppWorkspace);
}

void QuickOpen::clearTreeFileSelection()
{
    _selectedTreeFiles.clear();
    if (_tree) InvalidateRect(_tree, nullptr, TRUE);
}

bool QuickOpen::isTreeFileSelected(HTREEITEM item) const
{
    return item && _selectedTreeFiles.find(item) != _selectedTreeFiles.end();
}

void QuickOpen::toggleTreeFileSelection(HTREEITEM item, bool ctrlDown)
{
    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;

    if (it->second.type != NodeType::File)
    {
        if (!ctrlDown)
        {
            _selectedTreeFiles.clear();
            InvalidateRect(_tree, nullptr, TRUE);
        }
        return;
    }

    if (!ctrlDown)
        _selectedTreeFiles.clear();

    if (ctrlDown && isTreeFileSelected(item))
        _selectedTreeFiles.erase(item);
    else
        _selectedTreeFiles.insert(item);

    InvalidateRect(_tree, nullptr, TRUE);
}

void QuickOpen::openSelectedTreeFiles()
{
    std::vector<std::wstring> paths;
    for (HTREEITEM item : _selectedTreeFiles)
    {
        auto it = _nodeData.find(item);
        if (it != _nodeData.end() && it->second.type == NodeType::File)
            paths.push_back(it->second.path.wstring());
    }

    // If there is no multi-selection, keep the normal single-file behavior.
    if (paths.empty())
    {
        HTREEITEM item = TreeView_GetSelection(_tree);
        auto it = _nodeData.find(item);
        if (it != _nodeData.end() && it->second.type == NodeType::File)
            paths.push_back(it->second.path.wstring());
    }

    for (const auto& path : paths)
        SendMessageW(_npp, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(path.c_str()));
}

void QuickOpen::openTreeSelection()
{
    if (!_selectedTreeFiles.empty())
    {
        openSelectedTreeFiles();
        return;
    }

    HTREEITEM item = TreeView_GetSelection(_tree);
    if (!item) return;

    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;

    if (it->second.type == NodeType::File)
    {
        const std::wstring path = it->second.path.wstring();
        SendMessageW(_npp, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(path.c_str()));
    }
    else
    {
        expandNode(item);
        TVITEMW stateItem{};
        stateItem.mask = TVIF_STATE;
        stateItem.stateMask = TVIS_EXPANDED;
        stateItem.hItem = item;
        TreeView_GetItem(_tree, &stateItem);
        const bool expanding = (stateItem.state & TVIS_EXPANDED) == 0;
        TreeView_Expand(_tree, item, expanding ? TVE_EXPAND : TVE_COLLAPSE);
        refreshTreeNodeLabel(item, expanding);
    }
}

void QuickOpen::showResultsContextMenu(HWND list, POINT screenPoint)
{
    if (!list) return;
    const int count = ListView_GetSelectedCount(list);
    if (count <= 0) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_OPEN_SELECTED, L"Abrir arquivos selecionados");
    SetForegroundWindow(list);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, screenPoint.x, screenPoint.y, 0,
                   (list == _searchPopupResults ? _searchPopup : _window), nullptr);
    DestroyMenu(menu);
}

void QuickOpen::openSelectedResults(HWND list, bool closePopup)
{
    if (!list) return;

    std::vector<size_t> indexes;
    for (int row = -1; (row = ListView_GetNextItem(list, row, LVNI_SELECTED)) >= 0; )
    {
        if (row >= 0 && row < static_cast<int>(_searchResults.size()))
            indexes.push_back(static_cast<size_t>(row));
    }

    for (size_t index : indexes)
    {
        openSearchResultRow(_searchResults[index]);
    }

    if (closePopup) hideSearchPopup();
}

void QuickOpen::openSearchResultRow(const SearchResult& result)
{
    if (result.rowType == SearchResult::RowType::ContentLine)
    {
        openFileAtOccurrence(result);
        return;
    }

    const std::wstring path = result.path.wstring();
    SendMessageW(_npp, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(path.c_str()));
}

void QuickOpen::openFileAtOccurrence(const SearchResult& result)
{
    const std::wstring path = result.path.wstring();
    SendMessageW(_npp, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(path.c_str()));

    HWND scintilla = NPPWorkSpace_GetCurrentScintillaHandle();
    if (!scintilla || result.lineNumber == 0) return;

    const WPARAM line = static_cast<WPARAM>(result.lineNumber - 1);
    const LRESULT lineStart = SendMessageW(scintilla, SCI_POSITIONFROMLINE, line, 0);
    const LRESULT lineEnd = SendMessageW(scintilla, SCI_GETLINEENDPOSITION, line, 0);
    if (lineStart < 0 || lineEnd < lineStart) return;

    SendMessageW(scintilla, SCI_SETVISIBLEPOLICY, CARET_EVEN | CARET_JUMPS, 0);
    SendMessageW(scintilla, SCI_ENSUREVISIBLEENFORCEPOLICY, line, 0);
    SendMessageW(scintilla, SCI_SETSEARCHFLAGS, 0, 0);

    auto bytesFromWide = [](const std::wstring& value, UINT codePage)
    {
        std::string bytes;
        if (value.empty()) return bytes;
        int needed = WideCharToMultiByte(codePage, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return bytes;
        bytes.assign(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(codePage, 0, value.data(), static_cast<int>(value.size()),
                            bytes.data(), needed, nullptr, nullptr);
        return bytes;
    };

    auto findOccurrence = [&](const std::string& needle, LRESULT& foundStart, LRESULT& foundEnd)
    {
        if (needle.empty()) return false;
        LRESULT searchStart = lineStart;
        for (size_t occurrence = 0; occurrence <= result.occurrenceOnLine; ++occurrence)
        {
            SendMessageW(scintilla, SCI_SETTARGETSTART, static_cast<WPARAM>(searchStart), 0);
            SendMessageW(scintilla, SCI_SETTARGETEND, static_cast<WPARAM>(lineEnd), 0);
            const LRESULT pos = SendMessageA(scintilla, SCI_SEARCHINTARGET,
                                             static_cast<WPARAM>(needle.size()),
                                             reinterpret_cast<LPARAM>(needle.data()));
            if (pos < 0) return false;
            foundStart = pos;
            foundEnd = SendMessageW(scintilla, SCI_GETTARGETEND, 0, 0);
            searchStart = foundEnd > pos ? foundEnd : pos + 1;
        }
        return foundStart >= 0 && foundEnd >= foundStart;
    };

    LRESULT foundStart = -1;
    LRESULT foundEnd = -1;
    const std::wstring needle = result.matchText.empty() ? result.snippet : result.matchText;
    const UINT scintillaCodePage = static_cast<UINT>(SendMessageW(scintilla, SCI_GETCODEPAGE, 0, 0));

    std::vector<std::string> candidates;
    candidates.push_back(utf8FromWide(needle));
    if (scintillaCodePage != CP_UTF8)
    {
        std::string cpBytes = bytesFromWide(needle, scintillaCodePage ? scintillaCodePage : CP_ACP);
        if (!cpBytes.empty() && cpBytes != candidates.front())
            candidates.push_back(std::move(cpBytes));
    }
    std::string acpBytes = bytesFromWide(needle, CP_ACP);
    if (!acpBytes.empty() && std::find(candidates.begin(), candidates.end(), acpBytes) == candidates.end())
        candidates.push_back(std::move(acpBytes));

    for (const auto& candidate : candidates)
    {
        if (findOccurrence(candidate, foundStart, foundEnd))
            break;
    }

    if (foundStart >= 0 && foundEnd >= foundStart)
    {
        SendMessageW(scintilla, SCI_GOTOPOS, static_cast<WPARAM>(foundEnd), 0);
        SendMessageW(scintilla, SCI_SETANCHOR, static_cast<WPARAM>(foundStart), 0);
    }
    else
    {
        SendMessageW(scintilla, SCI_GOTOPOS, static_cast<WPARAM>(lineStart), 0);
        SendMessageW(scintilla, SCI_SETANCHOR, static_cast<WPARAM>(lineStart), 0);
    }
    SendMessageW(scintilla, SCI_SCROLLCARET, 0, 0);
}

LRESULT QuickOpen::handleResultsCustomDraw(HWND list, NMLVCUSTOMDRAW* customDraw)
{
    if (!customDraw) return CDRF_DODEFAULT;

    const DWORD stage = customDraw->nmcd.dwDrawStage;
    if (stage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (stage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
    if (stage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM))
    {
        const int row = static_cast<int>(customDraw->nmcd.dwItemSpec);
        if (row >= 0 && row < static_cast<int>(_searchResults.size()))
        {
            const auto& result = _searchResults[static_cast<size_t>(row)];
            if (result.rowType == SearchResult::RowType::ContentLine && customDraw->iSubItem == 2)
            {
                drawHighlightedSnippet(list, customDraw, result);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return CDRF_DODEFAULT;
}

void QuickOpen::drawHighlightedSnippet(HWND list, NMLVCUSTOMDRAW* customDraw, const SearchResult& result)
{
    const int row = static_cast<int>(customDraw->nmcd.dwItemSpec);
    RECT rc{};
    if (!ListView_GetSubItemRect(list, row, 2, LVIR_BOUNDS, &rc)) return;

    HDC dc = customDraw->nmcd.hdc;
    const bool selected = (ListView_GetItemState(list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    const COLORREF background = selected ? GetSysColor(COLOR_HIGHLIGHT) : ListView_GetBkColor(list);
    const COLORREF textColor = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : ListView_GetTextColor(list);
    const COLORREF matchBackground = _darkMode ? RGB(92, 76, 0) : RGB(255, 236, 120);
    const COLORREF matchText = _darkMode ? RGB(255, 255, 255) : RGB(0, 0, 0);

    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(dc, &rc, backgroundBrush);
    DeleteObject(backgroundBrush);

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, _font);

    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int y = rc.top + ((rc.bottom - rc.top) - tm.tmHeight) / 2;
    int x = rc.left + 4;

    const size_t safeStart = (std::min)(result.matchStart, result.snippet.size());
    const size_t safeLength = (std::min)(result.matchLength, result.snippet.size() - safeStart);
    const std::wstring prefix = result.snippet.substr(0, safeStart);
    const std::wstring match = result.snippet.substr(safeStart, safeLength);
    const std::wstring suffix = result.snippet.substr(safeStart + safeLength);

    auto textWidth = [&](const std::wstring& text)
    {
        SIZE size{};
        if (!text.empty()) GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        return size.cx;
    };

    auto drawText = [&](const std::wstring& text, COLORREF color)
    {
        if (text.empty()) return;
        SetTextColor(dc, color);
        ExtTextOutW(dc, x, y, ETO_CLIPPED, &rc, text.c_str(), static_cast<UINT>(text.size()), nullptr);
        x += textWidth(text);
    };

    drawText(prefix, textColor);
    if (!match.empty())
    {
        const int width = textWidth(match);
        RECT matchRect{x, rc.top + 2, (std::min)(x + width, static_cast<int>(rc.right)), rc.bottom - 2};
        HBRUSH matchBrush = CreateSolidBrush(matchBackground);
        FillRect(dc, &matchRect, matchBrush);
        DeleteObject(matchBrush);
        drawText(match, matchText);
    }
    drawText(suffix, textColor);

    RestoreDC(dc, saved);
}

void QuickOpen::showTreeContextMenu(HTREEITEM item, POINT screenPoint)
{
    auto it = _nodeData.find(item);
    const bool hasItem = it != _nodeData.end();
    const bool selectedFiles = !_selectedTreeFiles.empty();
    const bool isContainer = hasItem && it->second.type == NodeType::Container;
    const bool isRoot = hasItem && it->second.type == NodeType::Root && !it->second.fromNppWorkspace;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (selectedFiles)
    {
        AppendMenuW(menu, MF_STRING, ID_OPEN_SELECTED, L"Abrir arquivos selecionados");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    if (isContainer)
    {
        AppendMenuW(menu, MF_STRING, ID_ADD, L"Adicionar pasta ao projeto");
        AppendMenuW(menu, MF_STRING, ID_REMOVE, L"Excluir contêiner");
        AppendMenuW(menu, MF_STRING, ID_CREATE_CONTAINER + 1, L"Renomear contêiner");
        AppendMenuW(menu, MF_STRING, ID_CONTAINER_COLOR, L"Alterar cor do contêiner...");
    }
    else if (isRoot)
    {
        AppendMenuW(menu, MF_STRING, ID_CREATE_CONTAINER + 2, L"Mover pasta para contêiner");
        AppendMenuW(menu, MF_STRING, ID_REMOVE, L"Remover pasta do NPPWorkSpace");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_CREATE_CONTAINER, L"Criar contêiner");

    SetForegroundWindow(_window);
    const UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD,
                                        screenPoint.x, screenPoint.y, 0, _window, nullptr);
    DestroyMenu(menu);

    if (command == ID_CREATE_CONTAINER) { createContainer(screenPoint); return; }
    if (command == ID_ADD && isContainer) { addFolder(); return; }
    if (command == ID_REMOVE && (isContainer || isRoot)) { removeSelectedRoot(); return; }
    if (command == ID_CREATE_CONTAINER + 1 && isContainer) { renameContainer(it->second.containerIndex); return; }
    if (command == ID_CONTAINER_COLOR && isContainer) { colorContainer(it->second.containerIndex); return; }
    if (command == ID_CREATE_CONTAINER + 2 && isRoot)
    {
        HMENU sub = CreatePopupMenu();
        if (!sub) return;
        for (size_t i = 0; i < _containers.size(); ++i)
            AppendMenuW(sub, MF_STRING, 3000 + static_cast<UINT>(i), _containers[i].name.c_str());
        if (!_containers.empty())
        {
            const UINT selectedCommand = TrackPopupMenu(sub, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD,
                                                        screenPoint.x + 8, screenPoint.y + 8, 0, _window, nullptr);
            if (selectedCommand >= 3000 && selectedCommand < 3000 + _containers.size())
                moveFolderToContainer(it->second.path, selectedCommand - 3000);
        }
        else
        {
            MessageBoxW(_window, L"Crie um contêiner primeiro para organizar esta pasta.", L"NPPWorkSpace", MB_ICONINFORMATION);
        }
        DestroyMenu(sub);
        return;
    }
    if (command == ID_OPEN_SELECTED) openSelectedTreeFiles();
}

void QuickOpen::expandAllFolders()
{
    if (!_tree) return;
    std::function<void(HTREEITEM)> expand = [&](HTREEITEM item)
    {
        for (HTREEITEM current = item; current; current = TreeView_GetNextSibling(_tree, current))
        {
            auto it = _nodeData.find(current);
            if (it != _nodeData.end() && it->second.type != NodeType::File)
            {
                expandNode(current);
                TreeView_Expand(_tree, current, TVE_EXPAND);
                refreshTreeNodeLabel(current, true);
                expand(TreeView_GetChild(_tree, current));
            }
        }
    };
    expand(TreeView_GetRoot(_tree));
}

void QuickOpen::collapseAllFolders()
{
    if (!_tree) return;
    std::function<void(HTREEITEM)> collapse = [&](HTREEITEM item)
    {
        for (HTREEITEM current = item; current; current = TreeView_GetNextSibling(_tree, current))
        {
            HTREEITEM child = TreeView_GetChild(_tree, current);
            if (child)
            {
                collapse(child);
                TreeView_Expand(_tree, current, TVE_COLLAPSE);
                refreshTreeNodeLabel(current, false);
            }
        }
    };
    collapse(TreeView_GetRoot(_tree));
}

void QuickOpen::expandAllContentGroups()
{
    if (!_results || _contentSearchGroups.empty()) return;
    for (auto& group : _contentSearchGroups)
        group.collapsed = false;

    SendMessageW(_results, WM_SETREDRAW, FALSE, 0);
    rebuildContentResultsList(_results);
    SendMessageW(_results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(_results, nullptr, FALSE);
}

void QuickOpen::collapseAllContentGroups()
{
    if (!_results || _contentSearchGroups.empty()) return;
    for (auto& group : _contentSearchGroups)
        group.collapsed = true;

    SendMessageW(_results, WM_SETREDRAW, FALSE, 0);
    rebuildContentResultsList(_results);
    SendMessageW(_results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(_results, nullptr, FALSE);
}

void QuickOpen::handleTreeDoubleClick(LPNMTREEVIEWW tv)
{
    if (!tv) return;

    const HTREEITEM item = tv->itemNew.hItem;
    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;

    // A double-click on any selected file opens the whole current selection.
    // If the clicked file is not selected, make it the sole selection first.
    if (it->second.type == NodeType::File)
    {
        if (!isTreeFileSelected(item))
        {
            _selectedTreeFiles.clear();
            _selectedTreeFiles.insert(item);
            InvalidateRect(_tree, nullptr, TRUE);
        }
        openSelectedTreeFiles();
        return;
    }

    // Folders keep the normal expand/collapse behavior.
    openTreeSelection();
}

void QuickOpen::handleTreeItemExpanding(LPNMTREEVIEWW tv)
{
    if (!tv) return;
    if (tv->action == TVE_EXPAND)
    {
        expandNode(tv->itemNew.hItem);
        refreshTreeNodeLabel(tv->itemNew.hItem, true);
    }
    else if (tv->action == TVE_COLLAPSE)
    {
        refreshTreeNodeLabel(tv->itemNew.hItem, false);
    }
}

HWND QuickOpen::findFolderWorkspaceTree() const
{
    HWND tree = nullptr;
    if (_npp) EnumChildWindows(_npp, enumTree, reinterpret_cast<LPARAM>(&tree));
    return tree;
}

bool QuickOpen::isReadableAddress(const void* p, size_t bytes)
{
    if (!p || !bytes) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t end = begin + mbi.RegionSize;
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    return address >= begin && bytes <= end - address;
}

std::wstring QuickOpen::getTreeItemText(HWND tree, HTREEITEM item)
{
    wchar_t buffer[2048]{};
    TVITEMW tv{};
    tv.mask = TVIF_TEXT;
    tv.hItem = item;
    tv.pszText = buffer;
    tv.cchTextMax = static_cast<int>(std::size(buffer));
    return TreeView_GetItem(tree, &tv) ? std::wstring(buffer) : L"";
}

std::vector<std::filesystem::path> QuickOpen::getNppWorkspaceRoots() const
{
    std::vector<std::filesystem::path> roots;
    HWND tree = findFolderWorkspaceTree();
    if (!tree) return roots;

    // Notepad++'s Folder as Workspace stores _rootPath only on root nodes.
    // Reading only the top level avoids interpreting the private lParam of
    // normal folder/file nodes as SortingData4lParam.
    for (HTREEITEM item = TreeView_GetRoot(tree); item; item = TreeView_GetNextSibling(tree, item))
    {
        TVITEMW tv{};
        tv.mask = TVIF_PARAM;
        tv.hItem = item;
        if (!TreeView_GetItem(tree, &tv) || !tv.lParam) continue;
        if (!isReadableAddress(reinterpret_cast<void*>(tv.lParam), sizeof(SortingData4lParamMirror))) continue;

        const auto* data = reinterpret_cast<const SortingData4lParamMirror*>(tv.lParam);
        if (!data->_isFolder || data->_rootPath.empty()) continue;

        std::error_code ec;
        const std::filesystem::path path(data->_rootPath);
        if (std::filesystem::is_directory(path, ec)) roots.push_back(path);
    }

    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

bool QuickOpen::isHiddenSystemDirectory(const std::filesystem::path& p)
{
    const std::wstring n = lower(p.filename().wstring());
    if (n == L"$recycle.bin" || n == L"system volume information") return true;
    DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN) && n != L".git";
}

std::wstring QuickOpen::lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}

int QuickOpen::fuzzyScore(const std::wstring& query, const std::wstring& candidate)
{
    if (query.empty()) return 1;
    const auto q = lower(query);
    const auto c = lower(candidate);

    size_t pos = 0;
    int score = 0;
    int last = -1;
    for (wchar_t ch : q)
    {
        const size_t found = c.find(ch, pos);
        if (found == std::wstring::npos) return -1;
        score += 100;
        if (last >= 0)
            score += (static_cast<int>(found) == last + 1) ? 50 : -std::min(30, static_cast<int>(found) - last);
        if (found == 0 || c[found - 1] == L'\\' || c[found - 1] == L'/' ||
            c[found - 1] == L'_' || c[found - 1] == L'-')
            score += 40;
        last = static_cast<int>(found);
        pos = found + 1;
    }
    score -= static_cast<int>(c.size() - q.size());
    return std::max(0, std::min(1000000, score));
}


int QuickOpen::fuzzyScoreLower(const std::wstring& q, const std::wstring& c)
{
    if (q.empty()) return 1;
    size_t pos = 0;
    int score = 0;
    int last = -1;
    for (wchar_t ch : q)
    {
        const size_t found = c.find(ch, pos);
        if (found == std::wstring::npos) return -1;
        score += 100;
        if (last >= 0)
            score += (static_cast<int>(found) == last + 1) ? 50 : -(std::min)(30, static_cast<int>(found) - last);
        if (found == 0 || c[found - 1] == L'\\' || c[found - 1] == L'/' ||
            c[found - 1] == L'_' || c[found - 1] == L'-')
            score += 40;
        last = static_cast<int>(found);
        pos = found + 1;
    }
    score -= static_cast<int>(c.size() - q.size());
    return (std::max)(0, (std::min)(1000000, score));
}


bool QuickOpen::isSearchPathEnabled(const std::filesystem::path& path) const
{
    return isPathEnabledForScope(path, _searchIncludedPaths, _searchDisabledPaths);
}

void QuickOpen::invalidateSearchIndex()
{
    _searchIndexValid = false;
    _searchIndex.clear();
    _searchIndexSnapshot.reset();
    _searchIndexGeneration.fetch_add(1);
    _searchQueryGeneration.fetch_add(1);
}

void QuickOpen::startSearchIndexBuild()
{
    if (_searchIndexBuilding || !_window) return;
    if (_searchIndexThread.joinable()) _searchIndexThread.join();

    const unsigned int generation = _searchIndexGeneration.load();
    const auto roots = getWorkspaceRootsForPanel();
    _searchIndexBuilding = true;
    _searchIndexValid = false;

    _searchIndexThread = std::thread([this, roots, generation]()
    {
        auto* built = new std::vector<SearchResult>();
        constexpr size_t MAX_INDEX_FILES_PER_ROOT = 50000;
        for (const auto& root : roots)
        {
            if (_searchIndexGeneration.load() != generation) break;
            std::error_code ec;
            std::filesystem::recursive_directory_iterator it(
                root, std::filesystem::directory_options::skip_permission_denied, ec);
            size_t visited = 0;
            for (; it != std::filesystem::recursive_directory_iterator{} && !ec; it.increment(ec))
            {
                if (_searchIndexGeneration.load() != generation) break;
                if (++visited > MAX_INDEX_FILES_PER_ROOT) break;
                const auto p = it->path();
                std::error_code itemEc;
                if (it->is_directory(itemEc))
                {
                    if (isHiddenSystemDirectory(p)) it.disable_recursion_pending();
                    continue;
                }
                if (!it->is_regular_file(itemEc)) continue;

                std::wstring file = p.filename().wstring();
                if (file.empty()) continue;
                std::error_code relEc;
                auto rel = std::filesystem::relative(p, root, relEc);
                if (relEc || rel.empty()) rel = p.filename();
                auto folderPath = rel.parent_path();
                std::wstring folder = folderPath.empty() ? L"\\" : L"\\" + folderPath.wstring();
                std::replace(folder.begin(), folder.end(), L'/', L'\\');
                {
                    const std::wstring relative = rel.wstring();
                    built->push_back({p, file, folder, relative, lower(file), lower(relative)});
                }
            }
        }

        if (_searchIndexGeneration.load() != generation || !_window)
        {
            delete built;
            _searchIndexBuilding = false;
            return;
        }
        PostMessageW(_window, WM_SEARCH_INDEX_READY, static_cast<WPARAM>(generation), reinterpret_cast<LPARAM>(built));
    });
}

void QuickOpen::finishSearchIndexBuild(std::vector<SearchResult>* built)
{
    std::unique_ptr<std::vector<SearchResult>> holder(built);
    _searchIndexBuilding = false;
    if (!holder) return;
    _searchIndex = std::move(*holder);
    _searchIndexSnapshot = std::make_shared<const std::vector<SearchResult>>(_searchIndex);
    _searchIndexValid = true;
    if (_runSearchAfterIndexBuild)
    {
        const bool popup = _runSearchAfterIndexPopup;
        _runSearchAfterIndexBuild = false;
        _runSearchAfterIndexPopup = false;
        if (popup) updatePopupSearch();
        else updateSearch();
    }
}

void QuickOpen::showSearchScopeMenu(HWND owner, POINT screenPoint)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    _scopeMenuFolders.clear();
    _scopeMenuContainers.clear();
    _nextScopeMenuId = 50000;

    AppendMenuW(menu, MF_STRING | ((_searchIncludedPaths.empty() && _searchDisabledPaths.empty()) ? MF_CHECKED : 0),
                49999, L"Pesquisar em tudo");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    auto addFolderItem = [&](const std::filesystem::path& folder, const std::wstring& label)
    {
        if (_nextScopeMenuId >= 59900) return;
        const UINT id = _nextScopeMenuId++;
        _scopeMenuFolders[id] = folder;
        const bool enabled = isPathEnabledForScope(folder, _searchIncludedPaths, _searchDisabledPaths);
        AppendMenuW(menu, MF_STRING | (enabled ? MF_CHECKED : MF_UNCHECKED), id, label.c_str());
    };

    for (size_t i = 0; i < _containers.size(); ++i)
    {
        const auto& c = _containers[i];
        HMENU sub = CreatePopupMenu();
        if (!sub) continue;
        size_t enabledCount = 0;
        for (const auto& folder : c.folders)
        {
            if (isPathEnabledForScope(folder, _searchIncludedPaths, _searchDisabledPaths)) ++enabledCount;
            addFolderItem(folder, folder.filename().wstring().empty() ? folder.wstring() : folder.filename().wstring());
            // Move the last generated item from the main menu into this submenu.
            const UINT id = _nextScopeMenuId - 1;
            MENUITEMINFOW mi{sizeof(mi)};
            mi.fMask = MIIM_ID | MIIM_STATE | MIIM_STRING;
            wchar_t text[1024]{};
            GetMenuStringW(menu, id, text, static_cast<int>(std::size(text)), MF_BYCOMMAND);
            mi.wID = id;
            mi.fState = isPathEnabledForScope(folder, _searchIncludedPaths, _searchDisabledPaths) ? MFS_CHECKED : MFS_UNCHECKED;
            mi.dwTypeData = text;
            DeleteMenu(menu, id, MF_BYCOMMAND);
            InsertMenuItemW(sub, 0xFFFFFFFF, TRUE, &mi);
        }
        const UINT cid = _nextScopeMenuId++;
        _scopeMenuContainers[cid] = i;
        const bool allEnabled = !c.folders.empty() && enabledCount == c.folders.size();
        InsertMenuW(sub, 0, MF_BYPOSITION | MF_STRING | (allEnabled ? MF_CHECKED : MF_UNCHECKED), cid, L"Pesquisar neste contêiner");
        InsertMenuW(sub, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), c.name.c_str());
    }

    for (const auto& root : _savedRoots)
        addFolderItem(root, root.filename().wstring().empty() ? root.wstring() : root.filename().wstring());

    // The menu above needs command handling by the owner window. Track the
    // menu while it is open and dispatch the selected id through WM_COMMAND.
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, owner, nullptr);
    if (cmd == 49999)
    {
        _searchIncludedPaths.clear();
        _searchDisabledPaths.clear();
        _searchQueryGeneration.fetch_add(1);
        writeWorkspaceFile();
        markSearchPending(owner == _searchPopup);
    }
    else if (cmd >= 50000 && cmd < 60000)
    {
        applySearchScopeCommand(static_cast<UINT>(cmd));
    }
    DestroyMenu(menu);
    _scopeMenuFolders.clear();
    _scopeMenuContainers.clear();
}

void QuickOpen::applySearchScopeCommand(UINT id)
{
    if (id == 49999) { _searchIncludedPaths.clear(); _searchDisabledPaths.clear(); }
    else if (auto it = _scopeMenuFolders.find(id); it != _scopeMenuFolders.end())
    {
        const std::wstring key = it->second.wstring();
        if (!_searchIncludedPaths.empty())
        {
            if (!_searchIncludedPaths.insert(key).second) _searchIncludedPaths.erase(key);
        }
        else
        {
            if (!_searchDisabledPaths.insert(key).second) _searchDisabledPaths.erase(key);
        }
    }
    else if (auto containerIt = _scopeMenuContainers.find(id); containerIt != _scopeMenuContainers.end())
    {
        if (containerIt->second < _containers.size())
        {
            const auto& c = _containers[containerIt->second];
            _searchIncludedPaths.clear();
            _searchDisabledPaths.clear();
            for (const auto& folder : c.folders)
                _searchIncludedPaths.insert(folder.wstring());
        }
    }
    // Changing scope only changes filtering; the file index itself remains
    // valid. Rebuilding a large workspace index here made every checkbox
    // click stall the UI and caused unnecessary disk traversal.
    _searchQueryGeneration.fetch_add(1);
    writeWorkspaceFile();
    markSearchPending(_searchPopup && IsWindowVisible(_searchPopup));
}

void QuickOpen::searchDirectory(const std::filesystem::path& root, const std::wstring& query,
                                std::vector<SearchResult>& results, size_t limit) const
{
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);

    size_t visited = 0;
    for (; it != std::filesystem::recursive_directory_iterator{} && !ec; it.increment(ec))
    {
        if (++visited > SEARCH_LIMIT_PER_ROOT || results.size() >= limit) break;
        const auto p = it->path();
        std::error_code itemEc;
        if (it->is_directory(itemEc))
        {
            if (isHiddenSystemDirectory(p)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(itemEc)) continue;

        std::wstring file = p.filename().wstring();
        // A regular file should always have a filename. Keep a robust fallback
        // for unusual filesystem entries so the result row can never be blank.
        if (file.empty()) file = p.stem().wstring();
        if (file.empty()) file = p.wstring();

        std::error_code relEc;
        std::filesystem::path relPath = std::filesystem::relative(p, root, relEc);
        if (relEc || relPath.empty()) relPath = p.filename();
        const std::wstring rel = relPath.wstring();
        std::filesystem::path folderPath = relPath.parent_path();
        std::wstring folder = folderPath.empty() ? L"\\" : L"\\" + folderPath.wstring();
        // Normalize the folder separator for a stable Windows presentation.
        std::replace(folder.begin(), folder.end(), L'/', L'\\');
        const bool match = fuzzyScore(query, file) >= 0 || fuzzyScore(query, rel) >= 0;
        if (match) results.push_back({p, file, folder, rel});
    }
}

void QuickOpen::markSearchPending(bool popup)
{
    _pendingContentQuery.clear();
    _pendingContentPopup = false;
    _runSearchAfterIndexBuild = false;
    _runSearchAfterIndexPopup = false;
    if (_searchQueryRunning)
    {
        _searchQueryGeneration.fetch_add(1);
        EnableWindow(_cancelSearch, FALSE);
    }

    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Pressione Pesquisar ou Enter");
    if (popup)
    {
        if (_searchPopupHint) SetWindowTextW(_searchPopupHint, L"Enter pesquisar  |  Na lista: Enter abrir  |  Esc fechar");
    }
    else if (_searchOnly)
    {
        SetWindowTextW(_status, L"Pressione Pesquisar ou Enter para buscar");
    }
}

void QuickOpen::updateSearch()
{
    if (!_search || _suppressSearch) return;
    wchar_t buffer[2048]{};
    GetWindowTextW(_search, buffer, static_cast<int>(std::size(buffer)));
    const std::wstring query(buffer);

    if (query.empty())
    {
        if (!_contentSearchPopup) cancelContentSearch();
        _searchOnly = false;
        _searchResults.clear();
        _contentSearchGroups.clear();
        _contentSearchGroupByPath.clear();
        ListView_DeleteAllItems(_results);
        configureResultsColumns(_results, false);
        showFoldersPanel();
        if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, 0, 0);
        if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Pronto");
        return;
    }

    showSearchResults(query);
}

void QuickOpen::showSearchResults(const std::wstring& query)
{
    if (isContentSearchEnabled(false))
    {
        _searchOnly = true;
        showResultsPanel();
        resetContentSearchResults(false);
        if (query.size() < 2)
        {
            cancelContentSearch();
            SetWindowTextW(_status, L"Digite ao menos 2 caracteres para pesquisar dentro dos arquivos");
            if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Aguardando termo de pesquisa");
            return;
        }
        startContentSearch(query, false);
        SetWindowTextW(_status, L"Pesquisando conteúdo...");
        return;
    }

    cancelContentSearch();
    configureResultsColumns(_results, false);

    if (!_searchIndexValid)
    {
        if (!_searchIndexBuilding) startSearchIndexBuild();
        _runSearchAfterIndexBuild = true;
        _runSearchAfterIndexPopup = false;
        _searchOnly = true;
        showResultsPanel();
        SetWindowTextW(_status, L"Indexando arquivos... aguarde");
        return;
    }

    const std::wstring q = lower(query);
    std::vector<SearchResult> found;
    found.reserve(MAX_SEARCH_RESULTS);
    std::vector<std::pair<int, size_t>> ranked;
    ranked.reserve(MAX_SEARCH_RESULTS * 2);

    for (size_t i = 0; i < _searchIndex.size(); ++i)
    {
        const auto& item = _searchIndex[i];
        if (!isSearchPathEnabled(item.path)) continue;

        const int fileScore = fuzzyScoreLower(q, item.fileNameLower);
        const int pathScore = fuzzyScoreLower(q, item.relativeLower);
        const int score = (std::max)(fileScore, pathScore);
        if (score >= 0)
        {
            ranked.emplace_back(score, i);
        }
    }

    std::partial_sort(ranked.begin(), ranked.end(), ranked.begin() +
                      (std::min)(ranked.size(), static_cast<size_t>(MAX_SEARCH_RESULTS)),
        [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });

    const size_t count = (std::min)(ranked.size(), static_cast<size_t>(MAX_SEARCH_RESULTS));
    found.reserve(count);
    for (size_t i = 0; i < count; ++i)
        found.push_back(_searchIndex[ranked[i].second]);

    _searchResults = std::move(found);
    ListView_DeleteAllItems(_results);
    for (size_t i = 0; i < _searchResults.size(); ++i)
    {
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(_searchResults[i].fileName.c_str());
        ListView_InsertItem(_results, &item);
        ListView_SetItemText(_results, static_cast<int>(i), 1, const_cast<LPWSTR>(_searchResults[i].folder.c_str()));
        const std::wstring fullPath = _searchResults[i].path.wstring();
        ListView_SetItemText(_results, static_cast<int>(i), 2, const_cast<LPWSTR>(fullPath.c_str()));
    }
    _searchOnly = true;
    showResultsPanel();
    if (!_searchResults.empty())
        ListView_SetItemState(_results, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SetWindowTextW(_status, (std::to_wstring(_searchResults.size()) + L" resultado(s)  •  Enter abrir  •  Esc limpar pesquisa").c_str());
}

void QuickOpen::clearSearchResults()
{
    cancelContentSearch();
    _suppressSearch = true;
    SetWindowTextW(_search, L"");
    _suppressSearch = false;
    _searchResults.clear();
    _contentSearchGroups.clear();
    _contentSearchGroupByPath.clear();
    configureResultsColumns(_results, false);
    ListView_DeleteAllItems(_results);
    _searchOnly = false;
    showFoldersPanel();
    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, 0, 0);
    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Pronto");
}

void QuickOpen::openSearchResult()
{
    const int row = ListView_GetNextItem(_results, -1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(_searchResults.size())) return;
    openSearchResultRow(_searchResults[static_cast<size_t>(row)]);
}

void QuickOpen::createContainer(POINT screenPoint)
{
    if (screenPoint.x < 0 || screenPoint.y < 0) GetCursorPos(&screenPoint);
    std::wstring name = L"Novo Projeto";
    if (!promptForText(_window, _npp, _font, _titleFont, L"Criar contêiner", L"Nome do projeto:", name, screenPoint)) return;

    // Keep names unique and deterministic.
    const std::wstring base = name;
    int suffix = 2;
    while (std::any_of(_containers.begin(), _containers.end(), [&](const WorkspaceContainer& c)
    {
        return _wcsicmp(c.name.c_str(), name.c_str()) == 0;
    }))
    {
        name = base + L" " + std::to_wstring(suffix++);
    }

    _containers.push_back({name, {}, defaultContainerColor(_containers.size())});
    writeWorkspaceFile();
    rebuildWorkspaceTree(false);
}

void QuickOpen::renameContainer(size_t index)
{
    if (index >= _containers.size()) return;
    std::wstring name = _containers[index].name;
    POINT renamePoint{}; GetCursorPos(&renamePoint);
    if (!promptForText(_window, _npp, _font, _titleFont, L"Renomear contêiner", L"Nome do projeto:", name, renamePoint)) return;
    if (name.empty()) return;

    for (size_t i = 0; i < _containers.size(); ++i)
    {
        if (i != index && _wcsicmp(_containers[i].name.c_str(), name.c_str()) == 0)
        {
            MessageBoxW(_window, L"Já existe um projeto com esse nome.", L"NPPWorkSpace", MB_ICONWARNING);
            return;
        }
    }
    _containers[index].name = name;
    writeWorkspaceFile();
    rebuildWorkspaceTree(false);
}

void QuickOpen::colorContainer(size_t index)
{
    if (index >= _containers.size()) return;

    static COLORREF customColors[16] = {
        RGB(86, 156, 214), RGB(78, 201, 176), RGB(206, 145, 120), RGB(220, 220, 170),
        RGB(197, 134, 192), RGB(181, 206, 168), RGB(244, 166, 88), RGB(114, 159, 207),
        RGB(156, 220, 254), RGB(215, 186, 125), RGB(198, 120, 221), RGB(97, 175, 239),
        RGB(152, 195, 121), RGB(224, 108, 117), RGB(229, 192, 123), RGB(86, 182, 194)
    };

    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = _window;
    cc.lpCustColors = customColors;
    cc.rgbResult = _containers[index].color == CLR_INVALID
        ? defaultContainerColor(index)
        : _containers[index].color;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;

    _containers[index].color = cc.rgbResult;
    writeWorkspaceFile();
    InvalidateRect(_tree, nullptr, FALSE);
}

void QuickOpen::removeContainer(size_t index)
{
    if (index >= _containers.size()) return;
    const std::wstring question = L"Excluir o contêiner \"" + _containers[index].name +
                                  L"\"? As pastas serão mantidas na Workspace, fora do projeto.";
    if (MessageBoxW(_window, question.c_str(), L"Excluir contêiner", MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;

    for (const auto& folder : _containers[index].folders)
    {
        if (std::find(_savedRoots.begin(), _savedRoots.end(), folder) == _savedRoots.end())
            _savedRoots.push_back(folder);
    }
    _containers.erase(_containers.begin() + static_cast<std::ptrdiff_t>(index));
    writeWorkspaceFile();
    rebuildWorkspaceTree(false);
}

void QuickOpen::moveFolderToContainer(const std::filesystem::path& folder, size_t containerIndex)
{
    if (containerIndex >= _containers.size() || folder.empty()) return;

    for (auto& container : _containers)
        container.folders.erase(std::remove(container.folders.begin(), container.folders.end(), folder), container.folders.end());
    _savedRoots.erase(std::remove(_savedRoots.begin(), _savedRoots.end(), folder), _savedRoots.end());

    auto& target = _containers[containerIndex].folders;
    if (std::find(target.begin(), target.end(), folder) == target.end())
        target.push_back(folder);

    writeWorkspaceFile();
    rebuildWorkspaceTree(false);
}

void QuickOpen::addFolder()
{
    std::filesystem::path folder;
    if (!chooseFolder(_window, folder)) return;

    HTREEITEM selected = TreeView_GetSelection(_tree);
    auto node = _nodeData.find(selected);
    if (node != _nodeData.end() && node->second.type == NodeType::Container &&
        node->second.containerIndex < _containers.size())
    {
        moveFolderToContainer(folder, node->second.containerIndex);
        return;
    }

    if (std::find(_savedRoots.begin(), _savedRoots.end(), folder) == _savedRoots.end())
        _savedRoots.push_back(folder);

    writeWorkspaceFile();
    rebuildWorkspaceTree();
}

void QuickOpen::removeSelectedRoot()
{
    HTREEITEM selected = TreeView_GetSelection(_tree);
    if (!selected) return;
    auto it = _nodeData.find(selected);
    if (it == _nodeData.end()) return;

    if (it->second.type == NodeType::Container)
    {
        removeContainer(it->second.containerIndex);
        return;
    }
    if (it->second.type != NodeType::Root || it->second.fromNppWorkspace) return;

    const auto path = it->second.path;
    if (it->second.containerIndex < _containers.size())
    {
        auto& folders = _containers[it->second.containerIndex].folders;
        folders.erase(std::remove(folders.begin(), folders.end(), path), folders.end());
    }
    else
    {
        _savedRoots.erase(std::remove(_savedRoots.begin(), _savedRoots.end(), path), _savedRoots.end());
    }
    writeWorkspaceFile();
    rebuildWorkspaceTree();
}

void QuickOpen::newWorkspace()
{
    const int answer = MessageBoxW(
        _window,
        L"Deseja salvar a configuração atual da workspace antes de criar uma nova?",
        L"Nova Workspace",
        MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON1);

    if (answer == IDCANCEL) return;
    if (answer == IDYES && !saveWorkspace()) return;

    _savedRoots.clear();
    _containers.clear();
    _searchIncludedPaths.clear();
    _searchDisabledPaths.clear();
    _workspaceFile = getWorkspaceFilePath();
    clearSearchResults();
    writeWorkspaceFile();
    rebuildWorkspaceTree(false);
    saveSettings();
}

bool QuickOpen::saveWorkspace()
{
    // Explicit Save always asks where the .worknpp should be written.
    return saveWorkspaceAs();
}

bool QuickOpen::loadContainersFromJson(const std::wstring& json, std::vector<WorkspaceContainer>& containers)
{
    const std::wstring needle = L"\"containers\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::wstring::npos) return false;
    size_t open = json.find(L'[', keyPos + needle.size());
    if (open == std::wstring::npos) return false;

    bool inString = false;
    bool escaped = false;
    int depth = 0;
    size_t close = std::wstring::npos;
    for (size_t i = open; i < json.size(); ++i)
    {
        const wchar_t ch = json[i];
        if (inString)
        {
            if (escaped) escaped = false;
            else if (ch == L'\\') escaped = true;
            else if (ch == L'"') inString = false;
            continue;
        }
        if (ch == L'"') { inString = true; continue; }
        if (ch == L'[') ++depth;
        else if (ch == L']' && --depth == 0) { close = i; break; }
    }
    if (close == std::wstring::npos) return false;

    size_t pos = open + 1;
    while (pos < close)
    {
        while (pos < close && (iswspace(json[pos]) || json[pos] == L',')) ++pos;
        if (pos >= close) break;
        if (json[pos] != L'{') return false;

        const size_t objectStart = pos;
        int objectDepth = 0;
        inString = false;
        escaped = false;
        size_t objectEnd = std::wstring::npos;
        for (; pos < close; ++pos)
        {
            const wchar_t ch = json[pos];
            if (inString)
            {
                if (escaped) escaped = false;
                else if (ch == L'\\') escaped = true;
                else if (ch == L'"') inString = false;
                continue;
            }
            if (ch == L'"') { inString = true; continue; }
            if (ch == L'{') ++objectDepth;
            else if (ch == L'}' && --objectDepth == 0) { objectEnd = pos; ++pos; break; }
        }
        if (objectEnd == std::wstring::npos) return false;

        const std::wstring object = json.substr(objectStart, objectEnd - objectStart + 1);
        WorkspaceContainer container;
        if (!extractJsonString(object, L"name", container.name) || container.name.empty())
            continue;
        std::wstring colorText;
        if (!extractJsonString(object, L"color", colorText) || !parseHexColor(colorText, container.color))
            container.color = defaultContainerColor(containers.size());
        std::vector<std::wstring> folders;
        if (extractJsonStringArray(object, L"folders", folders))
        {
            for (const auto& folder : folders)
                if (!folder.empty()) container.folders.emplace_back(folder);
        }
        containers.push_back(std::move(container));
    }
    return true;
}

bool QuickOpen::writeWorkspaceFile()
{
    if (_workspaceFile.empty()) return false;

    const std::filesystem::path target(_workspaceFile);
    std::error_code ec;
    if (!target.parent_path().empty())
        std::filesystem::create_directories(target.parent_path(), ec);

    std::wstringstream json;
    json << L"{\n";
    json << L"  \"format\": \"NPPWorkSpace\",\n";
    json << L"  \"version\": 2,\n";
    json << L"  \"workspace\": {\n";
    json << L"    \"folders\": [\n";
    for (size_t i = 0; i < _savedRoots.size(); ++i)
    {
        json << L"      \"" << jsonEscape(_savedRoots[i].wstring()) << L"\"";
        if (i + 1 < _savedRoots.size()) json << L',';
        json << L"\n";
    }
    json << L"    ],\n";
    json << L"    \"containers\": [\n";
    for (size_t i = 0; i < _containers.size(); ++i)
    {
        const auto& container = _containers[i];
        const COLORREF color = container.color == CLR_INVALID ? defaultContainerColor(i) : container.color;
        json << L"      {\"name\": \"" << jsonEscape(container.name) << L"\", \"color\": \""
             << colorToHex(color) << L"\", \"folders\": [";
        for (size_t j = 0; j < container.folders.size(); ++j)
        {
            if (j) json << L", ";
            json << L"\"" << jsonEscape(container.folders[j].wstring()) << L"\"";
        }
        json << L"]}";
        if (i + 1 < _containers.size()) json << L',';
        json << L"\n";
    }
    json << L"    ],\n";
    json << L"    \"searchIncluded\": [\n";
    size_t includedWritten = 0;
    for (const auto& included : _searchIncludedPaths)
    {
        json << L"      \"" << jsonEscape(included) << L"\"";
        if (++includedWritten < _searchIncludedPaths.size()) json << L',';
        json << L"\n";
    }
    json << L"    ],\n";
    json << L"    \"searchDisabled\": [\n";
    size_t disabledWritten = 0;
    for (const auto& disabled : _searchDisabledPaths)
    {
        json << L"      \"" << jsonEscape(disabled) << L"\"";
        if (++disabledWritten < _searchDisabledPaths.size()) json << L',';
        json << L"\n";
    }
    json << L"    ],\n";
    json << L"    \"shortcuts\": {\n";
    json << L"      \"toggleWorkspace\": \"" << jsonEscape(NPPWorkSpace_GetToggleShortcut()) << L"\",\n";
    json << L"      \"search\": \"" << jsonEscape(NPPWorkSpace_GetSearchShortcut()) << L"\"\n";
    json << L"    }\n";
    json << L"  }\n";
    json << L"}\n";

    const std::string utf8 = utf8FromWide(json.str());
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    if (!out.good()) return false;

    saveSettings();
    return true;
}

bool QuickOpen::saveWorkspaceAs()
{
    wchar_t fileName[MAX_PATH * 4] = L"NPPWorkSpace.worknpp";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = _window;
    ofn.lpstrFilter = L"NPPWorkSpace (*.worknpp)\0*.worknpp\0Todos os arquivos (*.*)\0*.*\0\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
    ofn.lpstrDefExt = L"worknpp";
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return false;

    std::wstring path(fileName);
    if (path.size() < 8 || _wcsicmp(path.c_str() + path.size() - 8, L".worknpp") != 0)
        path += L".worknpp";
    _workspaceFile = path;
    return writeWorkspaceFile();
}

void QuickOpen::openWorkspaceFile()
{
    wchar_t fileName[MAX_PATH * 4]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = _window;
    ofn.lpstrFilter = L"NPPWorkSpace (*.worknpp)\0*.worknpp\0Todos os arquivos (*.*)\0*.*\0\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
    ofn.lpstrDefExt = L"worknpp";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    loadWorkspaceFile(fileName);
}

bool QuickOpen::loadWorkspaceFile(const std::wstring& filePath)
{
    std::ifstream in(std::filesystem::path(filePath), std::ios::binary);
    if (!in) return false;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::wstring json = wideFromUtf8(bytes);
    if (json.empty()) return false;

    std::vector<std::wstring> folders;
    if (!extractJsonStringArray(json, L"folders", folders)) return false;

    std::vector<WorkspaceContainer> containers;
    loadContainersFromJson(json, containers);
    std::vector<std::wstring> includedSearch;
    extractJsonStringArray(json, L"searchIncluded", includedSearch);
    std::vector<std::wstring> disabledSearch;
    extractJsonStringArray(json, L"searchDisabled", disabledSearch);

    std::wstring toggleShortcut;
    std::wstring searchShortcut;
    if (extractJsonString(json, L"toggleWorkspace", toggleShortcut))
        NPPWorkSpace_SetToggleShortcut(toggleShortcut);
    if (extractJsonString(json, L"search", searchShortcut))
        NPPWorkSpace_SetSearchShortcut(searchShortcut);

    std::vector<std::filesystem::path> loaded;
    for (const auto& folder : folders)
    {
        if (folder.empty()) continue;
        std::filesystem::path path(folder);
        if (std::find(loaded.begin(), loaded.end(), path) == loaded.end())
            loaded.push_back(path);
    }

    // Only replace the current workspace after the file was parsed successfully.
    _savedRoots = std::move(loaded);
    _containers = std::move(containers);
    _searchIncludedPaths.clear();
    for (const auto& path : includedSearch) if (!path.empty()) _searchIncludedPaths.insert(path);
    _searchDisabledPaths.clear();
    for (const auto& path : disabledSearch) if (!path.empty()) _searchDisabledPaths.insert(path);
    _workspaceFile = filePath;
    clearSearchResults();
    saveSettings();
    rebuildWorkspaceTree(false);
    disableNativeFolderWorkspace();
    return true;
}

void QuickOpen::loadWorkspace()
{
    _savedRoots.clear();
    _containers.clear();
    _searchIncludedPaths.clear();
    _searchDisabledPaths.clear();
    if (_workspaceFile.empty()) _workspaceFile = getWorkspaceFilePath();
    if (std::filesystem::exists(_workspaceFile))
    {
        loadWorkspaceFile(_workspaceFile);
    }
    else
    {
        writeWorkspaceFile();
        saveSettings();
    }
}

std::vector<std::filesystem::path> QuickOpen::getWorkspaceRootsForPanel() const
{
    std::vector<std::filesystem::path> roots;
    auto addUnique = [&](const std::filesystem::path& root)
    {
        if (!root.empty() && std::find(roots.begin(), roots.end(), root) == roots.end())
            roots.push_back(root);
    };
    for (const auto& root : _savedRoots) addUnique(root);
    for (const auto& container : _containers)
        for (const auto& root : container.folders) addUnique(root);
    return roots;
}

std::wstring QuickOpen::getSettingsPath() const
{
    wchar_t buffer[32768]{};
    if (_npp && SendMessageW(_npp, NPPM_GETNPPSETTINGSDIRPATH, 32768, reinterpret_cast<LPARAM>(buffer)))
        return std::wstring(buffer) + L"\\plugins\\config\\NPPWorkSpace.ini";
    return L"NPPWorkSpace.ini";
}

std::wstring QuickOpen::getWorkspaceFilePath() const
{
    wchar_t buffer[32768]{};
    if (_npp && SendMessageW(_npp, NPPM_GETNPPSETTINGSDIRPATH, 32768, reinterpret_cast<LPARAM>(buffer)))
        return std::wstring(buffer) + L"\\plugins\\config\\NPPWorkSpace.worknpp";
    return L"NPPWorkSpace.worknpp";
}

void QuickOpen::loadSettings()
{
    std::wstring registryWorkspace;
    if (readWorkspacePathFromRegistry(registryWorkspace))
    {
        _workspaceFile = registryWorkspace;
        return;
    }

    std::ifstream in(getSettingsPath(), std::ios::binary);
    std::string line;
    bool first = true;
    while (std::getline(in, line))
    {
        if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        first = false;
        if (line.rfind("WorkspaceFile=", 0) == 0)
        {
            const std::wstring value = wideFromUtf8(line.substr(14));
            if (!value.empty()) _workspaceFile = value;
        }
    }
}

void QuickOpen::saveSettings() const
{
    if (!_workspaceFile.empty())
        writeWorkspacePathToRegistry(_workspaceFile);

    const std::wstring path = getSettingsPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out)
    {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        out.write(reinterpret_cast<const char*>(bom), sizeof(bom));
        out << "Version=2\r\n";
        out << "WorkspaceFile=" << utf8FromWide(_workspaceFile) << "\r\n";
    }
}

void QuickOpen::applyTheme()
{
    if (!_npp || !_window) return;

    _darkMode = SendMessageW(_npp, NPPM_ISDARKMODEENABLED, 0, 0) != FALSE;

    // NPPM_GETDARKMODECOLORS is the Notepad++ dark-mode palette API. It must
    // not be used as the light-mode palette: on light mode older N++ builds
    // can leave the returned structure with dark-theme values. In light mode
    // the native Windows colors are the UI palette used by Notepad++.
    NppDarkMode::Colors colors{};
    if (_darkMode &&
        SendMessageW(_npp, NPPM_GETDARKMODECOLORS, sizeof(colors), reinterpret_cast<LPARAM>(&colors)))
    {
        // Keep the active N++ dark/custom-tone palette exactly as supplied.
    }
    else
    {
        colors.background = GetSysColor(COLOR_BTNFACE);
        colors.softerBackground = GetSysColor(COLOR_BTNFACE);
        colors.hotBackground = GetSysColor(COLOR_3DLIGHT);
        colors.pureBackground = GetSysColor(COLOR_WINDOW);
        colors.errorBackground = GetSysColor(COLOR_WINDOW);
        colors.text = GetSysColor(COLOR_WINDOWTEXT);
        colors.darkerText = GetSysColor(COLOR_WINDOWTEXT);
        colors.disabledText = GetSysColor(COLOR_GRAYTEXT);
        colors.linkText = GetSysColor(COLOR_HOTLIGHT);
        colors.edge = GetSysColor(COLOR_BTNSHADOW);
        colors.hotEdge = GetSysColor(COLOR_HIGHLIGHT);
        colors.disabledEdge = GetSysColor(COLOR_3DLIGHT);
    }

    _backgroundColor = colors.background;
    _textColor = colors.text;
    _edgeColor = colors.edge;
    if (_backgroundBrush) DeleteObject(_backgroundBrush);
    _backgroundBrush = CreateSolidBrush(_backgroundColor);

    const COLORREF contentBackground = colors.pureBackground;
    if (_tree)
    {
        TreeView_SetBkColor(_tree, contentBackground);
        TreeView_SetTextColor(_tree, colors.text);
    }
    if (_results)
    {
        ListView_SetBkColor(_results, contentBackground);
        ListView_SetTextBkColor(_results, contentBackground);
        ListView_SetTextColor(_results, colors.text);
    }
    if (_searchPopupResults)
    {
        ListView_SetBkColor(_searchPopupResults, contentBackground);
        ListView_SetTextBkColor(_searchPopupResults, contentBackground);
        ListView_SetTextColor(_searchPopupResults, colors.text);
    }

    // Initialize the whole panel through Notepad++ once, then refresh its
    // children. The native API knows how to switch both directions (dark ->
    // light and light -> dark) and keeps buttons/scrollbars consistent.
    SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                 static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(_window));

    for (HWND h : {_searchGroup, _workspaceGroup, _search, _runSearch, _contentSearchCheck, _searchProgress,
                   _searchProgressText, _cancelSearch, _searchScopeButton, _tree, _results,
                   _addFolder, _newWorkspace, _saveWorkspace, _openWorkspace, _removeFolder,
                   _expandAll, _collapseAll, _createContainer, _viewFolders, _viewSearch, _status})
    {
        if (!h) continue;
        if (_darkMode)
        {
            SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                         static_cast<WPARAM>(NppDarkMode::dmfHandleChange), reinterpret_cast<LPARAM>(h));
        }
        else
        {
            // Clear any stale dark-mode theme from controls when returning to
            // light mode. Reapply the standard Windows theme metrics.
            SetWindowTheme(h, L"Explorer", nullptr);
        }
    }

    if (_searchPopup)
    {
        SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                     static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(_searchPopup));
        if (_searchPopupResults)
        {
            ListView_SetBkColor(_searchPopupResults, contentBackground);
            ListView_SetTextBkColor(_searchPopupResults, contentBackground);
            ListView_SetTextColor(_searchPopupResults, colors.text);
        }
        if (_darkMode)
        {
            for (HWND h : {_searchPopupEdit, _searchPopupRunSearch, _searchPopupContentCheck, _searchPopupScopeButton, _searchPopupResults, _searchPopupHint})
                if (h) SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                                    static_cast<WPARAM>(NppDarkMode::dmfHandleChange), reinterpret_cast<LPARAM>(h));
        }
        else
        {
            for (HWND h : {_searchPopupEdit, _searchPopupRunSearch, _searchPopupContentCheck, _searchPopupScopeButton, _searchPopupResults, _searchPopupHint})
                if (h) SetWindowTheme(h, L"Explorer", nullptr);
        }
    }

    InvalidateRect(_window, nullptr, TRUE);
    UpdateWindow(_window);
    if (_searchPopup) { InvalidateRect(_searchPopup, nullptr, TRUE); UpdateWindow(_searchPopup); }
}

void QuickOpen::onDarkModeChanged()
{
    applyTheme();
    if (_npp && _window) SendMessageW(_npp, NPPM_DMMUPDATEDISPINFO, 0, reinterpret_cast<LPARAM>(_window));
}

std::wstring QuickOpen::getWindowText(HWND h)
{
    if (!h) return {};
    const int length = GetWindowTextLengthW(h);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(h, result.data(), length + 1);
    result.resize(wcslen(result.c_str()));
    return result;
}

LRESULT CALLBACK QuickOpen::windowProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<QuickOpen*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->handleMessage(h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

LRESULT QuickOpen::handleMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_ERASEBKGND)
    {
        if (_backgroundBrush)
        {
            RECT rc{}; GetClientRect(h, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, _backgroundBrush);
            return 1;
        }
    }
    if (msg == WM_CTLCOLORSTATIC)
    {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, _textColor);
        SetBkColor(dc, _backgroundColor);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(_backgroundBrush);
    }
    switch (msg)
    {
    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(l);
        // The panel itself is a child of Notepad++'s docking host. Applying
        // limits here keeps the layout usable in docked and floating modes.
        clampWorkspaceMinMaxInfo(h, info);
        return 0;
    }
    case WM_WINDOWPOSCHANGING:
    {
        clampWorkspaceWindowPos(h, reinterpret_cast<WINDOWPOS*>(l));
        break;
    }
    case WM_SIZE:
    {
        const int requestedWidth = static_cast<int>(LOWORD(l));
        const int requestedHeight = static_cast<int>(HIWORD(l));
        layoutControls(requestedWidth, requestedHeight);
        return 0;
    }

    case WM_SEARCH_INDEX_READY:
        finishSearchIndexBuild(reinterpret_cast<std::vector<SearchResult>*>(l));
        return 0;
    case WM_CONTENT_SEARCH_BATCH:
        applyContentSearchBatch(reinterpret_cast<ContentSearchBatch*>(l));
        return 0;
    case WM_CONTENT_SEARCH_PROGRESS:
        updateContentSearchProgress(reinterpret_cast<ContentSearchProgress*>(l));
        return 0;
    case WM_CONTENT_SEARCH_DONE:
        completeContentSearch(reinterpret_cast<ContentSearchComplete*>(l));
        return 0;

    case WM_TIMER:
        if (w == WORKSPACE_SYNC_TIMER)
        {
            refreshDockHost();
            refreshDockSplitters();
            syncNativeFolderWorkspace();
            return 0;
        }
        break;

    case WM_COMMAND:
    {
        const int id = LOWORD(w);
        const int code = HIWORD(w);
        if (id == ID_SEARCH_SCOPE && code == BN_CLICKED)
        { POINT pt{}; GetCursorPos(&pt); showSearchScopeMenu(h, pt); return 0; }
        if (id == ID_RUN_SEARCH && code == BN_CLICKED)
        {
            updateSearch();
            return 0;
        }
        if (id == ID_CONTENT_SEARCH && code == BN_CLICKED)
        {
            setContentSearchEnabled(isButtonChecked(_contentSearchCheck));
            return 0;
        }
        if (id == ID_CANCEL_SEARCH && code == BN_CLICKED)
        {
            cancelContentSearch();
            return 0;
        }
        if (id == ID_SEARCH && code == EN_CHANGE)
        {
            if (!_suppressSearch) markSearchPending(false);
            return 0;
        }
        if (id == ID_ADD) { addFolder(); return 0; }
        if (id == ID_NEW) { newWorkspace(); return 0; }
        if (id == ID_SAVE) { saveWorkspace(); return 0; }
        if (id == ID_OPEN) { openWorkspaceFile(); return 0; }
        if (id == ID_OPEN_SELECTED) {
            if (_resultsViewVisible) openSelectedResults(_results);
            else openSelectedTreeFiles();
            return 0;
        }
        if (id == ID_REMOVE) { removeSelectedRoot(); return 0; }
        if (id == ID_EXPAND_ALL) { if (_resultsViewVisible) expandAllContentGroups(); else expandAllFolders(); return 0; }
        if (id == ID_COLLAPSE_ALL) { if (_resultsViewVisible) collapseAllContentGroups(); else collapseAllFolders(); return 0; }
        if (id == ID_CREATE_CONTAINER) { POINT pt{}; GetCursorPos(&pt); createContainer(pt); return 0; }
        if (id == ID_VIEW_FOLDERS && code == BN_CLICKED) { showFoldersPanel(); return 0; }
        if (id == ID_VIEW_SEARCH && code == BN_CLICKED) { showResultsPanel(); return 0; }
        if (id >= 50000 && id < 60000) { applySearchScopeCommand(static_cast<UINT>(id)); return 0; }
        break;
    }

    case WM_NOTIFY:
    {
        const auto* hdr = reinterpret_cast<const NMHDR*>(l);
        if (!hdr) break;

        if (hdr->idFrom == ID_TREE)
        {
            const auto* tv = reinterpret_cast<const NMTREEVIEWW*>(l);
            if (hdr->code == NM_DBLCLK) { handleTreeDoubleClick(const_cast<NMTREEVIEWW*>(tv)); return 0; }
            if (hdr->code == NM_CLICK)
            {
                POINT pt{};
                GetCursorPos(&pt);
                POINT client = pt;
                ScreenToClient(_tree, &client);
                TVHITTESTINFO hit{};
                hit.pt = client;
                HTREEITEM clicked = TreeView_HitTest(_tree, &hit);
                if (clicked && (hit.flags & TVHT_ONITEM))
                {
                    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

                    // Keep the native TreeView focus/selection synchronized with
                    // our custom multi-selection. Ctrl+click preserves the other
                    // selected files; a normal click starts a new selection.
                    TreeView_SelectItem(_tree, clicked);
                    toggleTreeFileSelection(clicked, ctrlDown);
                }
                return 0;
            }
            if (hdr->code == NM_CUSTOMDRAW)
            {
                auto* cd = reinterpret_cast<NMTVCUSTOMDRAW*>(const_cast<NMHDR*>(hdr));
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                {
                    const HTREEITEM item = reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec);
                    if (isTreeFileSelected(item))
                    {
                        cd->clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
                        cd->clrText = GetSysColor(COLOR_HIGHLIGHTTEXT);
                    }
                    else if (TreeView_GetSelection(_tree) != item)
                    {
                        auto it = _nodeData.find(item);
                        if (it != _nodeData.end() && it->second.type == NodeType::Container &&
                            it->second.containerIndex < _containers.size())
                        {
                            const COLORREF color = _containers[it->second.containerIndex].color;
                            if (color != CLR_INVALID)
                                cd->clrText = color;
                        }
                    }
                    return CDRF_DODEFAULT;
                }
            }
            if (hdr->code == TVN_ITEMEXPANDINGW) { handleTreeItemExpanding(const_cast<NMTREEVIEWW*>(tv)); return 0; }
            if (hdr->code == TVN_KEYDOWN)
            {
                const auto* kd = reinterpret_cast<const NMTVKEYDOWN*>(l);
                if (kd && kd->wVKey == VK_RETURN)
                {
                    openTreeSelection();
                    return 0;
                }
            }
            if (hdr->code == NM_RCLICK)
            {
                POINT pt{};
                GetCursorPos(&pt);
                POINT client = pt;
                ScreenToClient(_tree, &client);
                TVHITTESTINFO hit{};
                hit.pt = client;
                HTREEITEM item = TreeView_HitTest(_tree, &hit);
                if (item && (hit.flags & TVHT_ONITEM))
                {
                    const bool alreadySelected = isTreeFileSelected(item);
                    if (GetKeyState(VK_CONTROL) & 0x8000)
                        toggleTreeFileSelection(item, true);
                    else if (!alreadySelected)
                    {
                        TreeView_SelectItem(_tree, item);
                        toggleTreeFileSelection(item, false);
                    }
                }
                showTreeContextMenu(item, pt);
                return 0;
            }
        }
        else if (hdr->idFrom == ID_RESULTS)
        {
            if (hdr->code == NM_CUSTOMDRAW)
                return handleResultsCustomDraw(_results, reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(hdr)));
            if (hdr->code == NM_CLICK)
            {
                const int row = ListView_GetNextItem(_results, -1, LVNI_SELECTED);
                if (row >= 0 && row < static_cast<int>(_searchResults.size()) &&
                    _searchResults[static_cast<size_t>(row)].rowType == SearchResult::RowType::ContentFile)
                {
                    toggleContentGroupFromRow(_results, row);
                    return 0;
                }
            }
            if (hdr->code == NM_DBLCLK || hdr->code == NM_RETURN || hdr->code == LVN_ITEMACTIVATE)
            {
                openSelectedResults(_results);
                return 0;
            }
            if (hdr->code == LVN_KEYDOWN)
            {
                const auto* kd = reinterpret_cast<const NMLVKEYDOWN*>(l);
                if (kd && kd->wVKey == VK_RETURN)
                {
                    openSelectedResults(_results);
                    return 0;
                }
            }
            if (hdr->code == NM_RCLICK)
            {
                POINT pt{}; GetCursorPos(&pt);
                showResultsContextMenu(_results, pt);
                return 0;
            }
        }
        break;
    }

    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
        {
            if (_searchOnly || GetWindowTextLengthW(_search) > 0) clearSearchResults();
            return 0;
        }
        if (w == VK_RETURN)
        {
            if (_resultsViewVisible) openSearchResult();
            else openTreeSelection();
            return 0;
        }
        break;

    case WM_SETFOCUS:
        if (_resultsViewVisible && _results) SetFocus(_results);
        else if (_tree) SetFocus(_tree);
        break;
    }

    return DefWindowProcW(h, msg, w, l);
}

LRESULT CALLBACK QuickOpen::editProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    return g_instance ? g_instance->handleEditMessage(h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

LRESULT QuickOpen::handleEditMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (h == _searchPopupEdit)
    {
        if (msg == WM_KEYDOWN)
        {
            if (w == VK_ESCAPE) { hideSearchPopup(); return 0; }
            if (w == VK_RETURN) { updatePopupSearch(); return 0; }
            if (w == VK_DOWN) { SetFocus(_searchPopupResults); return 0; }
        }
        return _oldPopupSearchProc ? CallWindowProcW(_oldPopupSearchProc, h, msg, w, l)
                                    : DefWindowProcW(h, msg, w, l);
    }

    if (msg == WM_KEYDOWN)
    {
        if (w == VK_ESCAPE) { clearSearchResults(); return 0; }
        if (w == VK_RETURN) { updateSearch(); return 0; }
        if (w == VK_DOWN && _resultsViewVisible) { SetFocus(_results); return 0; }
    }
    return _oldSearchProc ? CallWindowProcW(_oldSearchProc, h, msg, w, l)
                           : DefWindowProcW(h, msg, w, l);
}

void QuickOpen::addDroppedFolders(HDROP drop)
{
    if (!drop) return;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    bool changed = false;
    for (UINT i = 0; i < count; ++i)
    {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring value(len + 1, L'\0');
        DragQueryFileW(drop, i, value.data(), len + 1);
        value.resize(wcslen(value.c_str()));
        std::filesystem::path path(value);
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec)) continue;
        if (std::find(_savedRoots.begin(), _savedRoots.end(), path) == _savedRoots.end())
        {
            _savedRoots.push_back(path);
            changed = true;
        }
    }
    DragFinish(drop);
    if (changed)
    {
        writeWorkspaceFile();
        rebuildWorkspaceTree();
        disableNativeFolderWorkspace();
    }
}

LRESULT CALLBACK QuickOpen::nppProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (!g_instance) return DefWindowProcW(h, msg, w, l);
    return g_instance->handleNppMessage(h, msg, w, l);
}

LRESULT QuickOpen::handleNppMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_DROPFILES)
    {
        addDroppedFolders(reinterpret_cast<HDROP>(static_cast<UINT_PTR>(w)));
        return 0;
    }
    return _oldNppProc ? CallWindowProcW(_oldNppProc, h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

LRESULT CALLBACK QuickOpen::searchPopupProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<QuickOpen*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->handleSearchPopupMessage(h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

LRESULT QuickOpen::handleSearchPopupMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_SEARCH_INDEX_READY:
        finishSearchIndexBuild(reinterpret_cast<std::vector<SearchResult>*>(l));
        return 0;
    case WM_CONTENT_SEARCH_BATCH:
        applyContentSearchBatch(reinterpret_cast<ContentSearchBatch*>(l));
        return 0;
    case WM_CONTENT_SEARCH_PROGRESS:
        updateContentSearchProgress(reinterpret_cast<ContentSearchProgress*>(l));
        return 0;
    case WM_CONTENT_SEARCH_DONE:
        completeContentSearch(reinterpret_cast<ContentSearchComplete*>(l));
        return 0;
    case WM_SIZE:
        layoutSearchPopup();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
        {
            hideSearchPopup();
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == ID_SEARCH_SCOPE && HIWORD(w) == BN_CLICKED)
        { POINT pt{}; GetCursorPos(&pt); showSearchScopeMenu(h, pt); return 0; }
        if (LOWORD(w) == ID_POPUP_RUN_SEARCH && HIWORD(w) == BN_CLICKED)
        {
            updatePopupSearch();
            return 0;
        }
        if (LOWORD(w) == ID_POPUP_CONTENT_SEARCH && HIWORD(w) == BN_CLICKED)
        {
            setContentSearchEnabled(isButtonChecked(_searchPopupContentCheck));
            return 0;
        }
        if (LOWORD(w) == ID_POPUP_SEARCH && HIWORD(w) == EN_CHANGE)
        {
            markSearchPending(true);
            return 0;
        }
        if (LOWORD(w) == ID_OPEN_SELECTED)
        {
            openSelectedResults(_searchPopupResults, true);
            return 0;
        }
        if (LOWORD(w) >= 50000 && LOWORD(w) < 60000) { applySearchScopeCommand(static_cast<UINT>(LOWORD(w))); return 0; }
        break;
    case WM_NOTIFY:
    {
        const auto* hdr = reinterpret_cast<const NMHDR*>(l);
        if (hdr && hdr->idFrom == ID_POPUP_RESULTS)
        {
            if (hdr->code == NM_CUSTOMDRAW)
                return handleResultsCustomDraw(_searchPopupResults, reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(hdr)));
            if (hdr->code == NM_CLICK)
            {
                const int row = ListView_GetNextItem(_searchPopupResults, -1, LVNI_SELECTED);
                if (row >= 0 && row < static_cast<int>(_searchResults.size()) &&
                    _searchResults[static_cast<size_t>(row)].rowType == SearchResult::RowType::ContentFile)
                {
                    toggleContentGroupFromRow(_searchPopupResults, row);
                    return 0;
                }
            }
            if (hdr->code == NM_DBLCLK || hdr->code == NM_RETURN || hdr->code == LVN_ITEMACTIVATE)
            {
                openSelectedResults(_searchPopupResults, true);
                return 0;
            }
            if (hdr->code == NM_RCLICK)
            {
                POINT pt{}; GetCursorPos(&pt);
                showResultsContextMenu(_searchPopupResults, pt);
                return 0;
            }
        }
        break;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
        {
            hideSearchPopup();
            return 0;
        }
        if (w == VK_RETURN)
        {
            openPopupSearchResult();
            return 0;
        }
        break;
    case WM_CLOSE:
        hideSearchPopup();
        return 0;
    case WM_DESTROY:
        _searchPopup = nullptr;
        _searchPopupEdit = nullptr;
        _searchPopupResults = nullptr;
        _searchPopupScopeButton = nullptr;
        _searchPopupRunSearch = nullptr;
        _searchPopupContentCheck = nullptr;
        _searchPopupHint = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
