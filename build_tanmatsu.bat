@echo off
set BUILDLOG=C:\Users\Howar\MatsuMonsterMesh\build_log.txt
echo Starting build... > %BUILDLOG%
set IDF_TOOLS_PATH=C:\Espressif
echo Calling idf_cmd_init... >> %BUILDLOG%
call C:\Espressif\idf_cmd_init.bat esp-idf-v5.5.3 >> %BUILDLOG% 2>&1
echo idf_cmd_init returned %ERRORLEVEL% >> %BUILDLOG%
cd /d C:\Users\Howar\MatsuMonsterMesh
where idf.py >> %BUILDLOG% 2>&1
echo === Building MatsuMonsterMesh for Tanmatsu === >> %BUILDLOG%
idf.py -B build/tanmatsu build -DDEVICE=tanmatsu -DSDKCONFIG_DEFAULTS="sdkconfigs/general;sdkconfigs/tanmatsu" -DSDKCONFIG=sdkconfig_tanmatsu -DIDF_TARGET=esp32p4 >> %BUILDLOG% 2>&1
if %ERRORLEVEL% EQU 0 (
    echo === BUILD SUCCEEDED === >> %BUILDLOG%
) else (
    echo === BUILD FAILED (exit %ERRORLEVEL%) === >> %BUILDLOG%
)
