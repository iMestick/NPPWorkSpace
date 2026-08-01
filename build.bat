@echo off
echo ========================================
echo Configurando o projeto...
echo ========================================

cmake -S . -B build -A x64

if errorlevel 1 (
    echo.
    echo [ERRO] Falha na configuracao do CMake!
    pause
    exit /b 1
)

echo.
echo ========================================
echo Compilando Release...
echo ========================================

cmake --build build --config Release

if errorlevel 1 (
    echo.
    echo [ERRO] Falha na compilacao!
    pause
    exit /b 1
)

echo.
echo ========================================
echo BUILD CONCLUIDO COM SUCESSO!
echo ========================================

pause