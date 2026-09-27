@echo off
setlocal EnableExtensions
set "PATH=%SystemRoot%\system32;%SystemRoot%"
set "ROOT=%~dp0.."
pushd "%ROOT%"
if errorlevel 1 exit /b 1

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere was not found. Install Visual Studio 2022 with the C++ tools.
  popd
  exit /b 1
)
set "VSINSTALL="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -all -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  echo %%I | findstr /I /C:"\2022\\" >nul
  if not errorlevel 1 set "VSINSTALL=%%I"
)
if not defined VSINSTALL (
  echo Visual Studio 2022 C++ x64 tools were not found.
  popd
  exit /b 1
)
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
  echo vcvars64.bat failed.
  popd
  exit /b 1
)

set "SRC=%CD%\XReceiver"
set "GEN=%SRC%\Generated Files"
set "OUT=%CD%\tools\selftest-out"
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"
pushd "%OUT%"
if errorlevel 1 exit /b 1

> cl.rsp echo /nologo
>> cl.rsp echo /std:c++17
>> cl.rsp echo /await
>> cl.rsp echo /EHsc
>> cl.rsp echo /W4
>> cl.rsp echo /O2
>> cl.rsp echo /MD
>> cl.rsp echo /DWIN32_LEAN_AND_MEAN
>> cl.rsp echo /DWINRT_LEAN_AND_MEAN
>> cl.rsp echo /wd4002
>> cl.rsp echo /I"%SRC%"
if exist "%GEN%" echo /I"%GEN%">>cl.rsp
>> cl.rsp echo "%~dp0selftest_main.cpp"
>> cl.rsp echo "%SRC%\SelfTest.cpp"
>> cl.rsp echo "%SRC%\SincResampler.cpp"
>> cl.rsp echo "%SRC%\RtpPacket.cpp"
>> cl.rsp echo "%SRC%\JitterBuffer.cpp"
>> cl.rsp echo "%SRC%\H264Bitstream.cpp"
>> cl.rsp echo "%SRC%\H264Depacketizer.cpp"
>> cl.rsp echo "%SRC%\H264KeyframeGate.cpp"
>> cl.rsp echo "%SRC%\VideoTimeline.cpp"
>> cl.rsp echo "%SRC%\VideoDeliveryCore.cpp"
>> cl.rsp echo "%SRC%\MediaClock.cpp"
>> cl.rsp echo "%SRC%\AudioReceiver.cpp"
>> cl.rsp echo "%SRC%\ReceiverSettings.cpp"
rem Link and run by full path: cmd may be told not to search the current directory
rem (NoDefaultCurrentDirectoryInExePath), and this runner must not depend on that.
set "EXE=%OUT%\selftest.exe"
>> cl.rsp echo /Fe:"%EXE%"
>> cl.rsp echo /link
>> cl.rsp echo WindowsApp.lib

cl "@%OUT%\cl.rsp"
set "CLERR=%ERRORLEVEL%"
if not "%CLERR%"=="0" (
  popd
  popd
  exit /b %CLERR%
)
if not exist "%EXE%" (
  echo Build reported success but "%EXE%" was not produced.
  popd
  popd
  exit /b 1
)
"%EXE%"
set "TESTERR=%ERRORLEVEL%"
popd
popd
exit /b %TESTERR%

