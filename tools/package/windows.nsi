; SPDX-License-Identifier: GPL-3.0-or-later
Unicode true
!include "MUI2.nsh"
Name "${PRODUCT_NAME}"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT_NAME}"
RequestExecutionLevel user
SetCompressor /SOLID zlib
!define MUI_ABORTWARNING
!ifdef APP_ICON
Icon "${APP_ICON}"
UninstallIcon "${APP_ICON}"
!endif
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\licenses\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "${PRODUCT_NAME}" SEC_APP
  SectionIn RO
  SetShellVarContext current
  IfFileExists "$INSTDIR\${PRODUCT_SLUG}.exe" 0 +3
    MessageBox MB_ICONSTOP "Uninstall the existing application before installing this version." /SD IDOK
    Abort
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
  CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" "$INSTDIR\${PRODUCT_SLUG}.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}" "Publisher" "${PRODUCT_VENDOR}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}" "UninstallString" '$\"$INSTDIR\uninstall.exe$\"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}" "QuietUninstallString" '$\"$INSTDIR\uninstall.exe$\" /S'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}" "NoRepair" 1
SectionEnd

; Per-user path from clap/include/clap/entry.h. Hosts search recursively.
Section /o "Stock plugins for other hosts (CLAP)" SEC_PLUGINS
  IfFileExists "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}\*" 0 +3
    MessageBox MB_ICONSTOP "The stock plugin folder already exists; it was left unchanged." /SD IDOK
    Goto plugin_done
  SetOutPath "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}"
  File /r "${STAGE}\plugins\clap\*"
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}" "StockPlugins" 1
plugin_done:
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  ReadRegDWORD $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}" "StockPlugins"
  StrCmp $0 1 0 +2
    RMDir /r "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}"
  Delete "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk"
  RMDir "$SMPROGRAMS\${PRODUCT_NAME}"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}"
  RMDir /r "$INSTDIR"
SectionEnd
