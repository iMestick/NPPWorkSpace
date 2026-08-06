#include "RemoteSsh.h"

#include <windows.h>
#include <wincrypt.h>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

#pragma comment(lib, "crypt32.lib")

namespace
{
std::once_flag g_libraryInitFlag;
bool g_libraryReady = false;

struct LibraryFinalizer
{
    ~LibraryFinalizer()
    {
        if (g_libraryReady)
            ssh_finalize();
    }
};

LibraryFinalizer g_libraryFinalizer;

std::string utf8FromWide(const std::wstring& value)
{
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                              static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) <= 0)
        return {};
    return result;
}

std::wstring wideFromUtf8(const std::string& value)
{
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                              static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), required) <= 0)
        return {};
    return result;
}

std::wstring base64Encode(const BYTE* data, DWORD size)
{
    if (!data || size == 0) return {};
    DWORD required = 0;
    if (!CryptBinaryToStringW(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &required))
        return {};
    std::wstring output(required, L'\0');
    if (!CryptBinaryToStringW(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              output.data(), &required))
        return {};
    if (!output.empty() && output.back() == L'\0') output.pop_back();
    return output;
}

bool base64Decode(const std::wstring& text, std::vector<BYTE>& bytes)
{
    bytes.clear();
    if (text.empty()) return true;
    DWORD required = 0;
    if (!CryptStringToBinaryW(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                              nullptr, &required, nullptr, nullptr))
        return false;
    bytes.resize(required);
    if (!CryptStringToBinaryW(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                              bytes.data(), &required, nullptr, nullptr))
        return false;
    bytes.resize(required);
    return true;
}

bool ensureLibraries(std::wstring& error)
{
    std::call_once(g_libraryInitFlag, []
    {
        g_libraryReady = ssh_init() == SSH_OK;
    });
    if (!g_libraryReady)
    {
        error = L"Não foi possível inicializar a biblioteca SSH.";
        return false;
    }
    return true;
}

std::wstring sessionError(ssh_session session, const std::wstring& fallback)
{
    if (!session) return fallback;
    const char* raw = ssh_get_error(session);
    const int code = ssh_get_error_code(session);
    if (raw && *raw)
    {
        const std::wstring converted = wideFromUtf8(raw);
        if (!converted.empty())
            return converted + L" (código " + std::to_wstring(code) + L")";
    }
    return fallback + L" (código " + std::to_wstring(code) + L")";
}

std::wstring sftpError(sftp_session sftp, ssh_session session, const std::wstring& fallback)
{
    const int sftpCode = sftp ? sftp_get_error(sftp) : 0;
    std::wstring message = sessionError(session, fallback);
    if (sftpCode != 0)
        message += L" [SFTP " + std::to_wstring(sftpCode) + L"]";
    return message;
}

std::string normalizeRemotePath(const std::wstring& path)
{
    std::string result = utf8FromWide(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    if (result.empty()) result = "/";
    if (result.front() != '/') result.insert(result.begin(), '/');
    while (result.size() > 1 && result.back() == '/') result.pop_back();
    return result;
}

std::string joinRemote(const std::string& left, const std::string& right)
{
    if (left.empty() || left == "/") return "/" + right;
    return left + "/" + right;
}

bool isSafeRemoteName(const std::string& name)
{
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
           name.find('\0') == std::string::npos;
}

bool isValidWindowsName(const std::wstring& name)
{
    if (name.empty() || name.back() == L'.' || name.back() == L' ') return false;
    for (const wchar_t ch : name)
    {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*')
            return false;
    }

    std::wstring base = name.substr(0, name.find(L'.'));
    std::transform(base.begin(), base.end(), base.begin(), [](wchar_t ch)
    {
        return static_cast<wchar_t>(std::towupper(ch));
    });
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL") return false;
    if (base.size() == 4 && ((base.rfind(L"COM", 0) == 0) || (base.rfind(L"LPT", 0) == 0)) &&
        base[3] >= L'1' && base[3] <= L'9')
        return false;
    return true;
}

unsigned long long fileTimeToUnixSeconds(const FILETIME& value)
{
    ULARGE_INTEGER ticks{};
    ticks.LowPart = value.dwLowDateTime;
    ticks.HighPart = value.dwHighDateTime;
    constexpr unsigned long long epoch = 11644473600ULL;
    const unsigned long long seconds = ticks.QuadPart / 10000000ULL;
    return seconds > epoch ? seconds - epoch : 0;
}

bool attributeIsDirectory(const sftp_attributes attributes)
{
    if (!attributes) return false;
    if (attributes->type == SSH_FILEXFER_TYPE_DIRECTORY) return true;
    return (attributes->flags & SSH_FILEXFER_ATTR_PERMISSIONS) != 0 &&
           (attributes->permissions & SSH_S_IFMT) == SSH_S_IFDIR;
}

bool attributeIsRegularFile(const sftp_attributes attributes)
{
    if (!attributes) return false;
    if (attributes->type == SSH_FILEXFER_TYPE_REGULAR) return true;
    if (attributes->type == SSH_FILEXFER_TYPE_SYMLINK ||
        attributes->type == SSH_FILEXFER_TYPE_DIRECTORY ||
        attributes->type == SSH_FILEXFER_TYPE_SPECIAL)
        return false;
    return (attributes->flags & SSH_FILEXFER_ATTR_PERMISSIONS) == 0 ||
           (attributes->permissions & SSH_S_IFMT) == SSH_S_IFREG;
}

unsigned long attributeModificationTime(const sftp_attributes attributes)
{
    if (!attributes) return 0;
    if (attributes->flags & SSH_FILEXFER_ATTR_ACMODTIME)
        return attributes->mtime;
    if (attributes->flags & SSH_FILEXFER_ATTR_MODIFYTIME)
    {
        const uint64_t capped = (std::min)(attributes->mtime64,
                                           static_cast<uint64_t>((std::numeric_limits<unsigned long>::max)()));
        return static_cast<unsigned long>(capped);
    }
    return 0;
}

struct ConnectedClient
{
    ssh_session session{nullptr};
    sftp_session sftp{nullptr};
    std::wstring fingerprint;
    bool compatibilityMode{false};

    ConnectedClient() = default;
    ConnectedClient(const ConnectedClient&) = delete;
    ConnectedClient& operator=(const ConnectedClient&) = delete;

    ConnectedClient(ConnectedClient&& other) noexcept
        : session(other.session), sftp(other.sftp), fingerprint(std::move(other.fingerprint)),
          compatibilityMode(other.compatibilityMode)
    {
        other.session = nullptr;
        other.sftp = nullptr;
        other.compatibilityMode = false;
    }

    ConnectedClient& operator=(ConnectedClient&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            session = other.session;
            sftp = other.sftp;
            fingerprint = std::move(other.fingerprint);
            compatibilityMode = other.compatibilityMode;
            other.session = nullptr;
            other.sftp = nullptr;
            other.compatibilityMode = false;
        }
        return *this;
    }

    ~ConnectedClient()
    {
        reset();
    }

    void reset()
    {
        if (sftp)
        {
            sftp_free(sftp);
            sftp = nullptr;
        }
        if (session)
        {
            if (ssh_is_connected(session)) ssh_disconnect(session);
            ssh_free(session);
            session = nullptr;
        }
        fingerprint.clear();
        compatibilityMode = false;
    }
};

struct SftpFileHandle
{
    sftp_file value{nullptr};
    explicit SftpFileHandle(sftp_file file = nullptr) : value(file) {}
    ~SftpFileHandle() { reset(); }
    SftpFileHandle(const SftpFileHandle&) = delete;
    SftpFileHandle& operator=(const SftpFileHandle&) = delete;
    sftp_file get() const { return value; }
    explicit operator bool() const { return value != nullptr; }
    void reset(sftp_file file = nullptr)
    {
        if (value) sftp_close(value);
        value = file;
    }
};

struct SftpDirectoryHandle
{
    sftp_dir value{nullptr};
    explicit SftpDirectoryHandle(sftp_dir directory = nullptr) : value(directory) {}
    ~SftpDirectoryHandle() { if (value) sftp_closedir(value); }
    SftpDirectoryHandle(const SftpDirectoryHandle&) = delete;
    SftpDirectoryHandle& operator=(const SftpDirectoryHandle&) = delete;
    sftp_dir get() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};

struct SftpAttributesHandle
{
    sftp_attributes value{nullptr};
    explicit SftpAttributesHandle(sftp_attributes attributes = nullptr) : value(attributes) {}
    ~SftpAttributesHandle() { if (value) sftp_attributes_free(value); }
    SftpAttributesHandle(const SftpAttributesHandle&) = delete;
    SftpAttributesHandle& operator=(const SftpAttributesHandle&) = delete;
    sftp_attributes get() const { return value; }
    sftp_attributes operator->() const { return value; }
    explicit operator bool() const { return value != nullptr; }
};

struct SshKeyHandle
{
    ssh_key value{nullptr};
    SshKeyHandle() = default;
    ~SshKeyHandle() { if (value) ssh_key_free(value); }
    SshKeyHandle(const SshKeyHandle&) = delete;
    SshKeyHandle& operator=(const SshKeyHandle&) = delete;
};

std::wstring authenticationMethodsText(int methods)
{
    std::wstring text;
    auto append = [&text](const wchar_t* value)
    {
        if (!text.empty()) text += L", ";
        text += value;
    };
    if (methods & SSH_AUTH_METHOD_PUBLICKEY) append(L"chave pública");
    if (methods & SSH_AUTH_METHOD_PASSWORD) append(L"senha");
    if (methods & SSH_AUTH_METHOD_INTERACTIVE) append(L"keyboard-interactive/PAM");
    if (methods & SSH_AUTH_METHOD_HOSTBASED) append(L"host-based");
    if (methods & SSH_AUTH_METHOD_GSSAPI_MIC) append(L"GSSAPI");
    return text;
}

void enableCompatibilityAlgorithms(ssh_session session)
{
    // libssh 0.12 already advertises current OpenSSH algorithms. These additions
    // are used only after a normal negotiation failed, for older Ubuntu/OpenSSH,
    // Dropbear and appliance configurations. Unsupported names are ignored.
    const char* kex = "+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1";
    const char* hostKeys = "+ssh-rsa";
    const char* ciphers = "+aes256-cbc,aes192-cbc,aes128-cbc,3des-cbc";
    const char* macs = "+hmac-sha1";
    (void)ssh_options_set(session, SSH_OPTIONS_KEY_EXCHANGE, kex);
    (void)ssh_options_set(session, SSH_OPTIONS_HOSTKEYS, hostKeys);
    (void)ssh_options_set(session, SSH_OPTIONS_PUBLICKEY_ACCEPTED_TYPES, hostKeys);
    (void)ssh_options_set(session, SSH_OPTIONS_CIPHERS_C_S, ciphers);
    (void)ssh_options_set(session, SSH_OPTIONS_CIPHERS_S_C, ciphers);
    (void)ssh_options_set(session, SSH_OPTIONS_HMAC_C_S, macs);
    (void)ssh_options_set(session, SSH_OPTIONS_HMAC_S_C, macs);
}

bool createAndConnectSession(const RemoteWorkspaceFolder& config, bool compatibilityMode,
                             ssh_session& session, std::wstring& error)
{
    session = ssh_new();
    if (!session)
    {
        error = L"Não foi possível criar a sessão SSH.";
        return false;
    }

    const std::string host = utf8FromWide(config.host);
    const std::string username = utf8FromWide(config.username);
    if (host.empty())
    {
        error = L"Informe o endereço do servidor SSH.";
        return false;
    }
    if (username.empty())
    {
        error = L"Informe o usuário SSH.";
        return false;
    }

    unsigned int port = config.port == 0 ? 22u : static_cast<unsigned int>(config.port);
    long timeoutSeconds = 15;
    int enabled = 1;
    int logLevel = SSH_LOG_NOLOG;

    if (ssh_options_set(session, SSH_OPTIONS_HOST, host.c_str()) != SSH_OK ||
        ssh_options_set(session, SSH_OPTIONS_USER, username.c_str()) != SSH_OK ||
        ssh_options_set(session, SSH_OPTIONS_PORT, &port) != SSH_OK ||
        ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeoutSeconds) != SSH_OK)
    {
        error = L"Não foi possível configurar a sessão SSH: " + sessionError(session, L"opção inválida");
        return false;
    }

    (void)ssh_options_set(session, SSH_OPTIONS_NODELAY, &enabled);
    (void)ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &logLevel);
    ssh_set_blocking(session, 1);

    if (compatibilityMode)
        enableCompatibilityAlgorithms(session);

    if (ssh_connect(session) != SSH_OK)
    {
        error = sessionError(session, L"conexão recusada");
        return false;
    }
    return true;
}

bool calculateHostFingerprint(ssh_session session, std::wstring& fingerprint, std::wstring& error)
{
    ssh_key serverKey = nullptr;
    if (ssh_get_server_publickey(session, &serverKey) != SSH_OK || !serverKey)
    {
        error = L"O servidor não forneceu uma chave pública válida: " +
                sessionError(session, L"chave do servidor indisponível");
        return false;
    }

    unsigned char* hash = nullptr;
    size_t hashLength = 0;
    const int hashResult = ssh_get_publickey_hash(serverKey, SSH_PUBLICKEY_HASH_SHA256,
                                                   &hash, &hashLength);
    ssh_key_free(serverKey);
    if (hashResult != SSH_OK || !hash || hashLength == 0)
    {
        if (hash) ssh_clean_pubkey_hash(&hash);
        error = L"O servidor não forneceu uma impressão SHA-256 válida.";
        return false;
    }

    fingerprint = base64Encode(hash, static_cast<DWORD>(hashLength));
    ssh_clean_pubkey_hash(&hash);
    while (!fingerprint.empty() && fingerprint.back() == L'=') fingerprint.pop_back();
    fingerprint.insert(0, L"SHA256:");
    return true;
}

int authenticateKeyboardInteractive(ssh_session session, const std::string& password)
{
    int result = ssh_userauth_kbdint(session, nullptr, nullptr);
    while (result == SSH_AUTH_INFO)
    {
        const int prompts = ssh_userauth_kbdint_getnprompts(session);
        if (prompts < 0) return SSH_AUTH_ERROR;
        for (int index = 0; index < prompts; ++index)
        {
            char echo = 0;
            (void)ssh_userauth_kbdint_getprompt(session, static_cast<unsigned int>(index), &echo);
            if (ssh_userauth_kbdint_setanswer(session, static_cast<unsigned int>(index),
                                               password.c_str()) < 0)
                return SSH_AUTH_ERROR;
        }
        result = ssh_userauth_kbdint(session, nullptr, nullptr);
    }
    return result;
}

bool authenticateSession(const RemoteWorkspaceFolder& config, ssh_session session,
                         std::wstring& error)
{
    const std::string username = utf8FromWide(config.username);

    int noneResult = ssh_userauth_none(session, nullptr);
    if (noneResult == SSH_AUTH_SUCCESS) return true;

    const int methods = ssh_userauth_list(session, nullptr);
    int authResult = SSH_AUTH_DENIED;

    if (config.authMode == RemoteWorkspaceFolder::AuthMode::PrivateKey)
    {
        if (config.privateKeyPath.empty())
        {
            error = L"Informe o arquivo da chave privada.";
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(config.privateKeyPath, ec))
        {
            error = L"A chave privada não foi encontrada: " + config.privateKeyPath;
            return false;
        }
        if (methods != 0 && !(methods & SSH_AUTH_METHOD_PUBLICKEY))
        {
            error = L"O servidor não oferece autenticação por chave pública para este usuário.";
            const std::wstring methodText = authenticationMethodsText(methods);
            if (!methodText.empty()) error += L" Métodos disponíveis: " + methodText;
            return false;
        }

        const std::string privateKeyPath = utf8FromWide(config.privateKeyPath);
        const std::string passphrase = utf8FromWide(config.privateKeyPassphrase);
        SshKeyHandle key;
        const int importResult = ssh_pki_import_privkey_file(
            privateKeyPath.c_str(), passphrase.empty() ? nullptr : passphrase.c_str(),
            nullptr, nullptr, &key.value);
        if (importResult != SSH_OK || !key.value)
        {
            error = L"Não foi possível abrir a chave privada. Verifique o formato e a frase-chave: " +
                    config.privateKeyPath;
            return false;
        }
        authResult = ssh_userauth_publickey(session, nullptr, key.value);
    }
    else
    {
        const std::string password = utf8FromWide(config.password);
        if (password.empty())
        {
            error = L"Informe a senha SSH.";
            return false;
        }

        if (methods == 0 || (methods & SSH_AUTH_METHOD_PASSWORD))
            authResult = ssh_userauth_password(session, nullptr, password.c_str());

        // Ubuntu often routes passwords through PAM keyboard-interactive.
        if (authResult != SSH_AUTH_SUCCESS &&
            (methods == 0 || (methods & SSH_AUTH_METHOD_INTERACTIVE)))
            authResult = authenticateKeyboardInteractive(session, password);
    }

    if (authResult == SSH_AUTH_SUCCESS) return true;

    error = L"Falha na autenticação SSH: " + sessionError(session, L"credenciais recusadas");
    const std::wstring methodText = authenticationMethodsText(methods);
    if (!methodText.empty()) error += L"\nMétodos aceitos pelo servidor: " + methodText;
    if (authResult == SSH_AUTH_PARTIAL)
        error += L"\nO servidor exige um segundo fator de autenticação.";
    return false;
}

bool connectClient(const RemoteWorkspaceFolder& config, ConnectedClient& client, std::wstring& error)
{
    if (!ensureLibraries(error)) return false;
    client.reset();

    std::wstring normalError;
    ssh_session rawSession = nullptr;
    if (!createAndConnectSession(config, false, rawSession, normalError))
    {
        if (rawSession)
        {
            ssh_free(rawSession);
            rawSession = nullptr;
        }

        std::wstring compatibilityError;
        if (!createAndConnectSession(config, true, rawSession, compatibilityError))
        {
            if (rawSession) ssh_free(rawSession);
            error = L"Falha no handshake SSH. Tentativa padrão: " + normalError +
                    L"\nTentativa de compatibilidade: " + compatibilityError;
            return false;
        }
        client.compatibilityMode = true;
    }
    client.session = rawSession;

    if (!calculateHostFingerprint(client.session, client.fingerprint, error)) return false;
    if (!config.hostKeySha256.empty() && config.hostKeySha256 != client.fingerprint)
    {
        error = L"A chave do servidor SSH mudou. Esperada: " + config.hostKeySha256 +
                L"\nRecebida: " + client.fingerprint +
                L"\nA conexão foi bloqueada para evitar acesso a um servidor diferente.";
        return false;
    }

    if (!authenticateSession(config, client.session, error)) return false;

    client.sftp = sftp_new(client.session);
    if (!client.sftp)
    {
        error = L"O servidor não disponibilizou o subsistema SFTP: " +
                sessionError(client.session, L"SFTP indisponível");
        return false;
    }
    if (sftp_init(client.sftp) != SSH_OK)
    {
        error = L"Não foi possível inicializar o SFTP: " +
                sftpError(client.sftp, client.session, L"SFTP recusado");
        return false;
    }
    return true;
}

bool ensureRemoteParentDirectories(sftp_session sftp, ssh_session session,
                                   const std::string& remoteFile, std::wstring& error)
{
    const size_t slash = remoteFile.find_last_of('/');
    if (slash == std::string::npos || slash == 0) return true;
    const std::string parent = remoteFile.substr(0, slash);
    std::string current;
    size_t position = 1;
    while (position <= parent.size())
    {
        const size_t next = parent.find('/', position);
        const std::string part = parent.substr(position, next == std::string::npos
            ? std::string::npos : next - position);
        if (!part.empty())
        {
            current += "/" + part;
            SftpAttributesHandle attributes(sftp_stat(sftp, current.c_str()));
            if (!attributes)
            {
                if (sftp_mkdir(sftp, current.c_str(), 0755) != SSH_OK)
                {
                    SftpAttributesHandle retry(sftp_stat(sftp, current.c_str()));
                    if (!retry)
                    {
                        error = L"Não foi possível criar a pasta remota: " + wideFromUtf8(current) +
                                L"\n" + sftpError(sftp, session, L"criação recusada");
                        return false;
                    }
                    if (!attributeIsDirectory(retry.get()))
                    {
                        error = L"Um componente do caminho remoto não é uma pasta: " + wideFromUtf8(current);
                        return false;
                    }
                }
            }
            else if (!attributeIsDirectory(attributes.get()))
            {
                error = L"Um componente do caminho remoto não é uma pasta: " + wideFromUtf8(current);
                return false;
            }
        }
        if (next == std::string::npos) break;
        position = next + 1;
    }
    return true;
}

bool downloadFile(sftp_session sftp, ssh_session session, const std::string& remotePath,
                  const std::filesystem::path& localPath, unsigned long mtime,
                  unsigned long long remoteSize, RemoteSshResult& result)
{
    WIN32_FILE_ATTRIBUTE_DATA localData{};
    if (GetFileAttributesExW(localPath.c_str(), GetFileExInfoStandard, &localData) &&
        !(localData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        ULARGE_INTEGER size{};
        size.LowPart = localData.nFileSizeLow;
        size.HighPart = localData.nFileSizeHigh;
        const unsigned long long localMtime = fileTimeToUnixSeconds(localData.ftLastWriteTime);
        if (remoteSize == size.QuadPart && (mtime == 0 || localMtime == mtime)) return true;
        if (mtime != 0 && localMtime > static_cast<unsigned long long>(mtime)) return true;
    }

    SftpFileHandle handle(sftp_open(sftp, remotePath.c_str(), O_RDONLY, 0));
    if (!handle)
    {
        result.error = L"Não foi possível abrir o arquivo remoto: " + wideFromUtf8(remotePath) +
                       L"\n" + sftpError(sftp, session, L"abertura recusada");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(localPath.parent_path(), ec);
    if (ec)
    {
        result.error = L"Não foi possível criar a pasta de cache: " + localPath.parent_path().wstring();
        return false;
    }

    const std::filesystem::path temporary = localPath.wstring() + L".nppworkspace.download";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        result.error = L"Não foi possível gravar no cache local: " + temporary.wstring();
        return false;
    }

    std::array<char, 64 * 1024> buffer{};
    while (true)
    {
        const auto read = sftp_read(handle.get(), buffer.data(), buffer.size());
        if (read > 0)
        {
            out.write(buffer.data(), static_cast<std::streamsize>(read));
            result.bytesTransferred += static_cast<unsigned long long>(read);
            if (!out.good())
            {
                result.error = L"Falha ao gravar o arquivo no cache local: " + localPath.wstring();
                out.close();
                std::filesystem::remove(temporary, ec);
                return false;
            }
        }
        else if (read == 0)
        {
            break;
        }
        else
        {
            result.error = L"Falha durante o download de: " + wideFromUtf8(remotePath) +
                           L"\n" + sftpError(sftp, session, L"leitura SFTP recusada");
            out.close();
            std::filesystem::remove(temporary, ec);
            return false;
        }
    }
    out.close();

    SetFileAttributesW(localPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    std::filesystem::remove(localPath, ec);
    ec.clear();
    std::filesystem::rename(temporary, localPath, ec);
    if (ec)
    {
        result.error = L"Não foi possível concluir o arquivo no cache local: " + localPath.wstring();
        std::filesystem::remove(temporary, ec);
        return false;
    }

    SetFileAttributesW(localPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (mtime != 0)
    {
        HANDLE file = CreateFileW(localPath.c_str(), FILE_WRITE_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            ULARGE_INTEGER ticks{};
            ticks.QuadPart = (static_cast<unsigned long long>(mtime) + 11644473600ULL) * 10000000ULL;
            FILETIME ft{ticks.LowPart, ticks.HighPart};
            SetFileTime(file, nullptr, nullptr, &ft);
            CloseHandle(file);
        }
    }

    ++result.filesProcessed;
    return true;
}

bool downloadDirectory(sftp_session sftp, ssh_session session,
                       const std::string& remoteDirectory,
                       const std::filesystem::path& localDirectory,
                       RemoteSshResult& result, const RemoteSsh::ProgressCallback& progress)
{
    struct PendingEntry
    {
        std::string name;
        std::wstring localName;
        bool isDirectory{false};
        bool isRegularFile{false};
        unsigned long mtime{0};
        unsigned long long size{0};
    };

    std::error_code ec;
    std::filesystem::create_directories(localDirectory, ec);
    if (ec)
    {
        result.error = L"Não foi possível criar a pasta de cache: " + localDirectory.wstring();
        return false;
    }

    // Read the complete directory before entering any child. Some SFTP servers
    // do not preserve a parent READDIR cursor reliably when another directory
    // handle is opened recursively on the same SFTP channel. In that case the
    // old implementation stopped after the first child (often the last-created
    // folder, such as "scene"). Closing the parent handle before recursion also
    // keeps the number of simultaneously open remote handles bounded.
    std::vector<PendingEntry> pending;
    {
        SftpDirectoryHandle directory(sftp_opendir(sftp, remoteDirectory.c_str()));
        if (!directory)
        {
            result.error = L"Não foi possível listar a pasta remota: " + wideFromUtf8(remoteDirectory) +
                           L"\n" + sftpError(sftp, session, L"listagem recusada");
            return false;
        }

        while (true)
        {
            SftpAttributesHandle attributes(sftp_readdir(sftp, directory.get()));
            if (!attributes)
            {
                if (!sftp_dir_eof(directory.get()))
                {
                    result.error = L"Falha ao ler a pasta remota: " + wideFromUtf8(remoteDirectory) +
                                   L"\n" + sftpError(sftp, session, L"leitura recusada");
                    return false;
                }
                break;
            }

            const std::string name = attributes->name ? attributes->name : "";
            if (!isSafeRemoteName(name)) continue;

            const std::wstring localName = wideFromUtf8(name);
            if (localName.empty() || !isValidWindowsName(localName)) continue;

            PendingEntry entry;
            entry.name = name;
            entry.localName = localName;
            entry.isDirectory = attributeIsDirectory(attributes.get());
            entry.isRegularFile = attributeIsRegularFile(attributes.get());
            if (!entry.isDirectory && !entry.isRegularFile) continue;
            entry.mtime = attributeModificationTime(attributes.get());
            entry.size = (attributes->flags & SSH_FILEXFER_ATTR_SIZE) ? attributes->size : 0;
            pending.push_back(std::move(entry));
        }
    } // The parent directory handle is closed here, before recursion begins.

    std::sort(pending.begin(), pending.end(), [](const PendingEntry& left,
                                                  const PendingEntry& right)
    {
        if (left.isDirectory != right.isDirectory) return left.isDirectory > right.isDirectory;
        return _wcsicmp(left.localName.c_str(), right.localName.c_str()) < 0;
    });

    for (const PendingEntry& entry : pending)
    {
        const std::string remoteChild = joinRemote(remoteDirectory, entry.name);
        const std::filesystem::path localChild = localDirectory / entry.localName;

        if (entry.isDirectory)
        {
            if (!downloadDirectory(sftp, session, remoteChild, localChild, result, progress))
                return false;
        }
        else
        {
            if (progress) progress(wideFromUtf8(remoteChild), result.filesProcessed);
            if (!downloadFile(sftp, session, remoteChild, localChild,
                              entry.mtime, entry.size, result))
                return false;
        }
    }
    return true;
}

std::string remotePathForLocalFile(const RemoteWorkspaceFolder& config,
                                   const std::filesystem::path& localFile,
                                   std::wstring& error)
{
    std::error_code ec;
    const std::filesystem::path cache = std::filesystem::weakly_canonical(config.localCachePath, ec);
    if (ec)
    {
        error = L"Cache local inválido: " + config.localCachePath.wstring();
        return {};
    }
    ec.clear();
    const std::filesystem::path file = std::filesystem::weakly_canonical(localFile, ec);
    if (ec)
    {
        error = L"Arquivo local inválido: " + localFile.wstring();
        return {};
    }

    std::filesystem::path relative = std::filesystem::relative(file, cache, ec);
    if (ec || relative.empty())
    {
        error = L"O arquivo não pertence ao cache SSH configurado.";
        return {};
    }
    for (const auto& component : relative)
    {
        if (component == L"..")
        {
            error = L"O arquivo está fora da pasta SSH configurada.";
            return {};
        }
    }

    std::string remote = normalizeRemotePath(config.remotePath);
    for (const auto& component : relative)
    {
        const std::string part = utf8FromWide(component.wstring());
        if (part.empty() || part == ".") continue;
        remote = joinRemote(remote, part);
    }
    return remote;
}
}

namespace RemoteSsh
{
struct Client::Impl
{
    ConnectedClient client;
    RemoteWorkspaceFolder config;
};

Client::Client() = default;
Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

std::wstring protectSecret(const std::wstring& plainText)
{
    if (plainText.empty()) return {};
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(plainText.data()));
    input.cbData = static_cast<DWORD>(plainText.size() * sizeof(wchar_t));
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"NPPWorkSpace SSH", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        return {};
    const std::wstring protectedText = base64Encode(output.pbData, output.cbData);
    if (output.pbData)
    {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
    }
    return protectedText;
}

bool unprotectSecret(const std::wstring& protectedText, std::wstring& plainText)
{
    plainText.clear();
    if (protectedText.empty()) return true;
    std::vector<BYTE> encrypted;
    if (!base64Decode(protectedText, encrypted)) return false;

    DATA_BLOB input{};
    input.pbData = encrypted.data();
    input.cbData = static_cast<DWORD>(encrypted.size());
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output))
        return false;

    if (output.cbData % sizeof(wchar_t) != 0)
    {
        SecureZeroMemory(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return false;
    }
    plainText.assign(reinterpret_cast<const wchar_t*>(output.pbData),
                     output.cbData / sizeof(wchar_t));
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return true;
}

RemoteSshResult Client::connect(const RemoteWorkspaceFolder& config)
{
    disconnect();
    RemoteSshResult result;
    auto impl = std::make_unique<Impl>();
    if (!connectClient(config, impl->client, result.error)) return result;
    impl->config = config;
    result.hostKeySha256 = impl->client.fingerprint;
    result.success = true;
    _impl = std::move(impl);
    return result;
}

void Client::disconnect()
{
    _impl.reset();
}

bool Client::isConnected() const
{
    return _impl && _impl->client.session && _impl->client.sftp &&
           ssh_is_connected(_impl->client.session);
}

bool Client::keepAlive()
{
    if (!isConnected()) return false;
    return ssh_send_ignore(_impl->client.session, "NPPWorkSpace keep-alive") == SSH_OK &&
           ssh_is_connected(_impl->client.session) != 0;
}

std::wstring Client::hostKeySha256() const
{
    return isConnected() ? _impl->client.fingerprint : std::wstring{};
}

RemoteSshResult Client::listDirectory(const std::wstring& remotePath,
                                      std::vector<RemoteDirectoryEntry>& entries)
{
    entries.clear();
    RemoteSshResult result;
    if (!isConnected())
    {
        result.error = L"A conexão SSH não está ativa.";
        return result;
    }

    const std::string normalized = normalizeRemotePath(remotePath);
    SftpDirectoryHandle directory(sftp_opendir(_impl->client.sftp, normalized.c_str()));
    if (!directory)
    {
        result.error = L"Não foi possível listar a pasta remota: " + wideFromUtf8(normalized) +
                       L"\n" + sftpError(_impl->client.sftp, _impl->client.session, L"listagem recusada");
        return result;
    }

    while (true)
    {
        SftpAttributesHandle attributes(sftp_readdir(_impl->client.sftp, directory.get()));
        if (!attributes)
        {
            if (!sftp_dir_eof(directory.get()))
            {
                result.error = L"Falha ao ler a pasta remota: " + wideFromUtf8(normalized) +
                               L"\n" + sftpError(_impl->client.sftp, _impl->client.session,
                                                 L"leitura recusada");
                entries.clear();
                return result;
            }
            break;
        }

        const std::string name = attributes->name ? attributes->name : "";
        if (!isSafeRemoteName(name)) continue;

        RemoteDirectoryEntry entry;
        entry.name = wideFromUtf8(name);
        entry.fullPath = wideFromUtf8(joinRemote(normalized, name));
        entry.isDirectory = attributeIsDirectory(attributes.get());
        entry.isRegularFile = attributeIsRegularFile(attributes.get());
        entry.size = (attributes->flags & SSH_FILEXFER_ATTR_SIZE) ? attributes->size : 0;
        entry.modifiedTime = attributeModificationTime(attributes.get());
        if (!entry.isDirectory && !entry.isRegularFile) continue;
        entries.push_back(std::move(entry));
    }

    std::sort(entries.begin(), entries.end(), [](const RemoteDirectoryEntry& left,
                                                  const RemoteDirectoryEntry& right)
    {
        if (left.isDirectory != right.isDirectory) return left.isDirectory > right.isDirectory;
        return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
    });
    result.hostKeySha256 = _impl->client.fingerprint;
    result.filesProcessed = entries.size();
    result.success = true;
    return result;
}

RemoteSshResult Client::downloadTree(const RemoteWorkspaceFolder& config,
                                     const ProgressCallback& progress)
{
    RemoteSshResult result;
    if (!isConnected())
    {
        result.error = L"A conexão SSH não está ativa.";
        return result;
    }

    result.hostKeySha256 = _impl->client.fingerprint;
    const std::string remoteRoot = normalizeRemotePath(config.remotePath);
    if (!downloadDirectory(_impl->client.sftp, _impl->client.session, remoteRoot,
                           config.localCachePath, result, progress))
        return result;

    result.success = true;
    return result;
}

RemoteSshResult Client::uploadFile(const RemoteWorkspaceFolder& config,
                                   const std::filesystem::path& localFile)
{
    RemoteSshResult result;
    if (!isConnected())
    {
        result.error = L"A conexão SSH não está ativa.";
        return result;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(localFile, ec))
    {
        result.error = L"O arquivo local não existe: " + localFile.wstring();
        return result;
    }

    std::wstring mappingError;
    const std::string remotePath = remotePathForLocalFile(config, localFile, mappingError);
    if (remotePath.empty())
    {
        result.error = mappingError;
        return result;
    }

    result.hostKeySha256 = _impl->client.fingerprint;
    if (!ensureRemoteParentDirectories(_impl->client.sftp, _impl->client.session,
                                       remotePath, result.error))
        return result;

    unsigned long mode = 0644;
    {
        SftpAttributesHandle existing(sftp_stat(_impl->client.sftp, remotePath.c_str()));
        if (existing && (existing->flags & SSH_FILEXFER_ATTR_PERMISSIONS))
            mode = existing->permissions & 0777u;
    }

    const std::string temporaryPath = remotePath + ".nppworkspace-upload";
    SftpFileHandle remote(sftp_open(_impl->client.sftp, temporaryPath.c_str(),
                                    O_WRONLY | O_CREAT | O_TRUNC, mode));
    if (!remote)
    {
        result.error = L"Não foi possível criar o arquivo remoto temporário: " +
                       wideFromUtf8(temporaryPath) + L"\n" +
                       sftpError(_impl->client.sftp, _impl->client.session, L"criação recusada");
        return result;
    }

    std::ifstream input(localFile, std::ios::binary);
    if (!input)
    {
        result.error = L"Não foi possível ler o arquivo local: " + localFile.wstring();
        return result;
    }

    std::array<char, 64 * 1024> buffer{};
    while (input)
    {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = input.gcount();
        size_t writtenTotal = 0;
        while (writtenTotal < static_cast<size_t>(read))
        {
            const auto written = sftp_write(remote.get(), buffer.data() + writtenTotal,
                                            static_cast<size_t>(read) - writtenTotal);
            if (written <= 0)
            {
                result.error = L"Falha durante o envio do arquivo: " + localFile.wstring() +
                               L"\n" + sftpError(_impl->client.sftp, _impl->client.session,
                                                 L"escrita SFTP recusada");
                remote.reset();
                sftp_unlink(_impl->client.sftp, temporaryPath.c_str());
                return result;
            }
            writtenTotal += static_cast<size_t>(written);
            result.bytesTransferred += static_cast<unsigned long long>(written);
        }
    }
    if (input.bad())
    {
        result.error = L"Falha ao ler o arquivo local durante o envio: " + localFile.wstring();
        remote.reset();
        sftp_unlink(_impl->client.sftp, temporaryPath.c_str());
        return result;
    }

    (void)sftp_fsync(remote.get());
    remote.reset();

    if (sftp_rename(_impl->client.sftp, temporaryPath.c_str(), remotePath.c_str()) != SSH_OK)
    {
        (void)sftp_unlink(_impl->client.sftp, remotePath.c_str());
        if (sftp_rename(_impl->client.sftp, temporaryPath.c_str(), remotePath.c_str()) != SSH_OK)
        {
            sftp_unlink(_impl->client.sftp, temporaryPath.c_str());
            result.error = L"O upload terminou, mas não foi possível substituir o arquivo remoto: " +
                           wideFromUtf8(remotePath) + L"\n" +
                           sftpError(_impl->client.sftp, _impl->client.session, L"renomeação recusada");
            return result;
        }
    }

    result.filesProcessed = 1;
    result.success = true;
    return result;
}

RemoteSshResult testConnection(const RemoteWorkspaceFolder& config)
{
    Client client;
    RemoteSshResult result = client.connect(config);
    if (!result.success) return result;

    std::vector<RemoteDirectoryEntry> entries;
    RemoteSshResult listing = client.listDirectory(config.remotePath, entries);
    if (!listing.success) return listing;
    listing.hostKeySha256 = client.hostKeySha256();
    return listing;
}

RemoteSshResult downloadTree(const RemoteWorkspaceFolder& config, const ProgressCallback& progress)
{
    Client client;
    RemoteSshResult result = client.connect(config);
    if (!result.success) return result;
    return client.downloadTree(config, progress);
}

RemoteSshResult uploadFile(const RemoteWorkspaceFolder& config,
                           const std::filesystem::path& localFile)
{
    Client client;
    RemoteSshResult result = client.connect(config);
    if (!result.success) return result;
    return client.uploadFile(config, localFile);
}
}
