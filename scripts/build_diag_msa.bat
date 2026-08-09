@echo off
cd /d D:\WorkDir\Facelogin\scripts
set VCDIR=D:\Code\IDE\VisualStudio\2022\VC\Tools\MSVC\14.44.35207
set KITDIR=D:\Windows Kits\10
set KITVER=10.0.26100.0
"%VCDIR%\bin\Hostx64\x64\cl.exe" /nologo /EHsc /DUNICODE /D_UNICODE /I"%VCDIR%\include" /I"%KITDIR%\Include\%KITVER%\ucrt" /I"%KITDIR%\Include\%KITVER%\um" /I"%KITDIR%\Include\%KITVER%\shared" diag_msa.cpp /link /LIBPATH:"%KITDIR%\Lib\%KITVER%\um\x64" /LIBPATH:"%KITDIR%\Lib\%KITVER%\ucrt\x64" /LIBPATH:"%VCDIR%\lib\x64" advapi32.lib /out:diag_msa.exe
echo EXITCODE=%ERRORLEVEL%
