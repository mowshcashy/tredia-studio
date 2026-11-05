; Tredia Studio installer (NSIS). Built by .github/workflows/tredia-windows.yaml:
;   makensis /DVERSION=1.0.0 /DSRCDIR=<install dir> /DOUTFILE=<setup.exe> /DROOT=<repo root> tredia-studio.nsi
Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"

!define APPNAME "Tredia Studio"
!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\TrediaStudio"
!define EXE "$INSTDIR\bin\64bit\tredia-studio.exe"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\Tredia Studio"
InstallDirRegKey HKLM "Software\Tredia Studio" "InstallDir"
RequestExecutionLevel admin
BrandingText "Tredia Studio ${VERSION} · built on OBS Studio"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "CompanyName" "Tredia"
VIAddVersionKey "FileDescription" "Tredia Studio installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "GPL-2.0-or-later. OBS Studio (C) Lain Bailey and contributors"

!define MUI_ICON "${ROOT}\frontend\cmake\windows\obs-studio.ico"
!define MUI_UNICON "${ROOT}\frontend\cmake\windows\obs-studio.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "${EXE}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${ROOT}\COPYING"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Arabic"
!insertmacro MUI_LANGUAGE "English"

Section "Tredia Studio" SecMain
  SectionIn RO
  ; Replace an older install cleanly (settings live in %APPDATA%\tredia-studio and are kept).
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\data"
  RMDir /r "$INSTDIR\core"
  SetOutPath "$INSTDIR"
  File /r "${SRCDIR}\*.*"
  File "${ROOT}\COPYING"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "Software\Tredia Studio" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${REGKEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKLM "${REGKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${REGKEY}" "Publisher" "Tredia"
  WriteRegStr HKLM "${REGKEY}" "URLInfoAbout" "https://tredia.cloud/tredia-studio"
  WriteRegStr HKLM "${REGKEY}" "DisplayIcon" "${EXE}"
  WriteRegStr HKLM "${REGKEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REGKEY}" "NoRepair" 1

  ; Shortcuts start in the program's folder, where it finds its data.
  SetOutPath "$INSTDIR\bin\64bit"
  CreateDirectory "$SMPROGRAMS\Tredia Studio"
  CreateShortCut "$SMPROGRAMS\Tredia Studio\Tredia Studio.lnk" "${EXE}"
  CreateShortCut "$SMPROGRAMS\Tredia Studio\Uninstall Tredia Studio.lnk" "$INSTDIR\uninstall.exe"
  CreateShortCut "$DESKTOP\Tredia Studio.lnk" "${EXE}"
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\Tredia Studio.lnk"
  RMDir /r "$SMPROGRAMS\Tredia Studio"
  RMDir /r "$INSTDIR"
  DeleteRegKey HKLM "${REGKEY}"
  DeleteRegKey HKLM "Software\Tredia Studio"
SectionEnd
