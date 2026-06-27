; NSIS installer for BASS PlAIer.
; Built in CI; expects the payload staged in the "release\" folder.
; Version and output name are passed on the command line:
;   makensis /DAPPVERSION=1.2.3 /DOUTFILE=BASSPlAIer-Setup.exe installer.nsi

Unicode true
!include "MUI2.nsh"

!ifndef APPVERSION
  !define APPVERSION "0.0.0"
!endif
!ifndef OUTFILE
  !define OUTFILE "BASSPlAIer-Setup.exe"
!endif

!define APPNAME   "BASS PlAIer"
!define PUBLISHER "MarcroSoft"
!define EXENAME   "BASSPlAIer.exe"
!define UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

Name "${APPNAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APPNAME}"
InstallDirRegKey HKLM "Software\${APPNAME}" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\${EXENAME}"

!insertmacro MUI_PAGE_LICENSE "LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Section "BASS PlAIer (required)" SEC_APP
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "release\BASSPlAIer.exe"
  File "release\bass.dll"
  File "release\bass_fx.dll"
  File "release\bassenc.dll"
  File "release\README.html"

  CreateShortcut "$SMPROGRAMS\${APPNAME}.lnk" "$INSTDIR\${EXENAME}"

  WriteRegStr HKLM "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayName"     "${APPNAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayVersion"  "${APPVERSION}"
  WriteRegStr HKLM "${UNINSTKEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr HKLM "${UNINSTKEY}" "DisplayIcon"     "$INSTDIR\${EXENAME}"
  WriteRegStr HKLM "${UNINSTKEY}" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINSTKEY}" "NoRepair" 1
  WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

Section /o "Desktop shortcut" SEC_DESKTOP
  CreateShortcut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\${EXENAME}"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\BASSPlAIer.exe"
  Delete "$INSTDIR\bass.dll"
  Delete "$INSTDIR\bass_fx.dll"
  Delete "$INSTDIR\bassenc.dll"
  Delete "$INSTDIR\README.html"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir  "$INSTDIR"
  Delete "$SMPROGRAMS\${APPNAME}.lnk"
  Delete "$DESKTOP\${APPNAME}.lnk"
  DeleteRegKey HKLM "${UNINSTKEY}"
  DeleteRegKey HKLM "Software\${APPNAME}"
SectionEnd
