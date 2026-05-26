@echo off
set BUILDLOG=C:\Users\Howar\MatsuMonsterMesh\reconfig_log.txt
echo Starting build... > %BUILDLOG%

set PYTHONIOENCODING=utf-8
set IDF_PATH=C:\Users\Howar\esp-idf-5.5.1
set ESP_IDF_VERSION=5.5
set IDF_TOOLS_PATH=C:\Espressif
set IDF_PYTHON_ENV_PATH=C:\Espressif\python_env\idf5.5_py3.14_env
set PATH=C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20241119\riscv32-esp-elf\bin;C:\Espressif\tools\ccache\4.11.2\ccache-4.11.2-windows-x86_64;C:\Espressif\tools\idf-git\2.44.0\cmd;%IDF_PYTHON_ENV_PATH%\Scripts;%PATH%

cd /d C:\Users\Howar\MatsuMonsterMesh
echo python=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe >> %BUILDLOG%
echo IDF_PATH=%IDF_PATH% >> %BUILDLOG%

echo === Building === >> %BUILDLOG%
python.exe %IDF_PATH%\tools\idf.py -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 >> %BUILDLOG% 2>&1
if %ERRORLEVEL% EQU 0 (
    echo === BUILD SUCCEEDED === >> %BUILDLOG%
) else (
    echo === BUILD FAILED (exit %ERRORLEVEL%) === >> %BUILDLOG%
)
