@echo off
setlocal EnableExtensions

cd /d "%~dp0"
set "LOCAL_VCPKG=%~dp0.deps\vcpkg"

if defined VCPKG_ROOT (
    set "VCPKG_DIR=%VCPKG_ROOT%"
) else (
    set "VCPKG_DIR=%LOCAL_VCPKG%"
)

echo ========================================
echo Preparando OpenSSL estatico...
echo ========================================

if not exist "%VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake" (
    where git >nul 2>nul
    if errorlevel 1 (
        echo.
        echo [ERRO] Git nao foi encontrado no PATH.
        echo Instale o Git for Windows ou defina VCPKG_ROOT para uma instalacao existente.
        pause
        exit /b 1
    )

    echo Baixando vcpkg em:
    echo %VCPKG_DIR%
    git clone --depth 1 https://github.com/microsoft/vcpkg.git "%VCPKG_DIR%"
    if errorlevel 1 (
        echo.
        echo [ERRO] Nao foi possivel baixar o vcpkg.
        pause
        exit /b 1
    )
)

if not exist "%VCPKG_DIR%\vcpkg.exe" (
    call "%VCPKG_DIR%\bootstrap-vcpkg.bat" -disableMetrics
    if errorlevel 1 (
        echo.
        echo [ERRO] Falha ao preparar o vcpkg.
        pause
        exit /b 1
    )
)

rem Esta versao troca completamente o backend SSH. Uma build incremental de
rem qualquer pacote anterior pode manter objetos do backend SSH antigo.
rem O cache do CMake e recriado; o download do vcpkg/OpenSSL permanece em .deps.
if exist "build" (
    echo.
    echo Removendo cache antigo de compilacao SSH...
    rmdir /s /q "build"
)

echo.
echo ========================================
echo Configurando libssh 0.12.2 + OpenSSL...
echo ========================================

cmake -S . -B build -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_DIR%\scripts\buildsystems\vcpkg.cmake" ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-md

if errorlevel 1 (
    echo.
    echo [ERRO] Falha na configuracao do CMake!
    echo Confira a conexao com a internet e o log exibido acima.
    pause
    exit /b 1
)

echo.
echo ========================================
echo Compilando Release...
echo ========================================

cmake --build build --config Release --parallel

if errorlevel 1 (
    echo.
    echo [ERRO] Falha na compilacao!
    pause
    exit /b 1
)

echo.
echo ========================================
echo BUILD CONCLUIDO COM SUCESSO!
echo Backend SSH: libssh 0.12.2 + OpenSSL
echo DLL unica: build\Release\NPPWorkSpace.dll
echo ========================================

pause
exit /b 0
