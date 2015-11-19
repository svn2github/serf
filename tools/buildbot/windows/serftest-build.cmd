@echo off
REM ================================================================
REM   Licensed to the Apache Software Foundation (ASF) under one
REM   or more contributor license agreements.  See the NOTICE file
REM   distributed with this work for additional information
REM   regarding copyright ownership.  The ASF licenses this file
REM   to you under the Apache License, Version 2.0 (the
REM   "License"); you may not use this file except in compliance
REM   with the License.  You may obtain a copy of the License at
REM
REM     http://www.apache.org/licenses/LICENSE-2.0
REM
REM   Unless required by applicable law or agreed to in writing,
REM   software distributed under the License is distributed on an
REM   "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
REM   KIND, either express or implied.  See the License for the
REM   specific language governing permissions and limitations
REM   under the License.
REM ================================================================

SETLOCAL ENABLEEXTENSIONS ENABLEDELAYEDEXPANSION

CALL ..\serf-config.cmd
IF ERRORLEVEL 1 EXIT /B 1

SET BB=%CD:~,-6%
SET INTDIR=%BB%\deps\build\release
SET INSTALL=%CD%\install

IF NOT EXIST "%INSTALL%\" MKDIR "%INSTALL%"

PATH %PATH%;%BB%\deps\release\bin;%BB%\deps\build\scons\scripts
SET PYTHONPATH=%BB%\deps\build\scons\Lib\site-packages\scons

SET LIB|find /i "\AMD64" > nul:
IF ERRORLEVEL 1 (
  SET SERF_ARCH=win32
) ELSE (
  SET SERF_ARCH=x64
)

SET SA=PREFIX=%INSTALL% OPENSSL=%INTDIR% ZLIB=%INTDIR% ZLIB=%INTDIR% APR=%INTDIR% APU=%INTDIR% SOURCE_LAYOUT=no TARGET_ARCH=%SERF_ARCH% MSVC_VERSION=10.0 APR_STATIC=yes

echo scons %SA%
CALL scons.bat %SA%
IF ERRORLEVEL 1 EXIT /B 1

EXIT /B 0
