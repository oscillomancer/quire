; SPDX-License-Identifier: GPL-3.0-or-later
Unicode true
!include "MUI2.nsh"
!include "Sections.nsh"
!include "${MANIFEST_DIR}\app-files.nsh"
!include "${MANIFEST_DIR}\plugin-files.nsh"
!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}"
Var PreviousInstall
Var PreviousPlugins
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

Function ValidateTarget
  ; A populated folder is allowed only for this registered installation.
  StrCmp $INSTDIR $PreviousInstall 0 check_empty
  StrCmp $PreviousInstall "" check_empty
  IfFileExists "$INSTDIR\uninstall.exe" target_valid
check_empty:
  FindFirst $2 $3 "$INSTDIR\*"
  IfErrors target_valid
check_next:
  StrCmp $3 "." next_entry
  StrCmp $3 ".." next_entry
  StrCmp $3 "" target_empty
  FindClose $2
  MessageBox MB_ICONSTOP "Choose an empty folder. This folder contains unrelated files." /SD IDOK
  SetErrorLevel 1
  Abort
next_entry:
  FindNext $2 $3
  IfErrors target_empty
  Goto check_next
target_empty:
  FindClose $2
target_valid:
FunctionEnd

Section "${PRODUCT_NAME}" SEC_APP
  SectionIn RO
  SetShellVarContext current
  Call ValidateTarget
  StrCmp $PreviousInstall "" install_new
  IfFileExists "$PreviousInstall\uninstall.exe" 0 install_new
  ; Check all mapped executable/DLL/plugin files before changing anything.
  !insertmacro CheckAppUnlocked "$PreviousInstall" install_locked upgrade
  ClearErrors
  ExecWait '$\"$PreviousInstall\uninstall.exe$\" /S _?=$PreviousInstall' $1
  IfErrors install_locked
  StrCmp $1 0 0 install_locked
  Delete "$PreviousInstall\uninstall.exe"
install_new:
  SetOutPath "$INSTDIR"
  ClearErrors
  File /r "${STAGE}\*"
  IfErrors install_failed
  WriteUninstaller "$INSTDIR\uninstall.exe"
  IfErrors install_failed
  CreateDirectory "$SMPROGRAMS\${PRODUCT_NAME}"
  CreateShortcut "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk" "$INSTDIR\${PRODUCT_SLUG}.exe"
  WriteRegStr HKCU "${REGKEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKCU "${REGKEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKCU "${REGKEY}" "Publisher" "${PRODUCT_VENDOR}"
  WriteRegStr HKCU "${REGKEY}" "UninstallString" '$\"$INSTDIR\uninstall.exe$\"'
  WriteRegStr HKCU "${REGKEY}" "QuietUninstallString" '$\"$INSTDIR\uninstall.exe$\" /S'
  WriteRegStr HKCU "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${REGKEY}" "NoRepair" 1
  Goto install_done
install_locked:
  MessageBox MB_ICONSTOP "Close ${PRODUCT_NAME} and any hosts using its plugins, then retry. Nothing has been replaced." /SD IDOK
  SetErrorLevel 1
  Abort
install_failed:
  MessageBox MB_ICONSTOP "Installation could not write all files. Close programs using this folder, check its permissions, then retry." /SD IDOK
  SetErrorLevel 1
  Abort
install_done:
SectionEnd

; Per-user path from clap/include/clap/entry.h. Hosts search recursively.
Section /o "Stock plugins for other hosts (CLAP)" SEC_PLUGINS
  IfFileExists "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}\*" 0 +3
    MessageBox MB_ICONSTOP "The stock plugin folder already exists; it was left unchanged." /SD IDOK
    Goto plugin_done
  SetOutPath "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}"
  File /r "${STAGE}\plugins\clap\*"
  WriteRegDWORD HKCU "${REGKEY}" "StockPlugins" 1
plugin_done:
SectionEnd

Function .onInit
  ReadRegStr $PreviousInstall HKCU "${REGKEY}" "InstallLocation"
  ReadRegDWORD $PreviousPlugins HKCU "${REGKEY}" "StockPlugins"
  StrCmp $PreviousInstall "" init_done
  StrCpy $INSTDIR $PreviousInstall
  StrCmp $PreviousPlugins 1 0 init_done
  SectionSetFlags ${SEC_PLUGINS} ${SF_SELECTED}
init_done:
FunctionEnd

Function un.onInit
  ReadRegStr $0 HKCU "${REGKEY}" "InstallLocation"
  StrCmp $0 $INSTDIR 0 uninstall_unregistered
  !insertmacro CheckAppUnlocked "$INSTDIR" uninstall_locked app
  ReadRegDWORD $PreviousPlugins HKCU "${REGKEY}" "StockPlugins"
  StrCmp $PreviousPlugins 1 0 uninstall_ready
  !insertmacro CheckPluginUnlocked "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}" uninstall_locked optional
uninstall_ready:
  Return
uninstall_unregistered:
  MessageBox MB_ICONSTOP "This folder is not the registered installation; no files were removed." /SD IDOK
  SetErrorLevel 1
  Abort
uninstall_locked:
  MessageBox MB_ICONSTOP "Close ${PRODUCT_NAME} and any hosts using its plugins, then retry uninstalling." /SD IDOK
  SetErrorLevel 1
  Abort
FunctionEnd

Section "Uninstall"
  SetShellVarContext current
  ; A running uninstaller cannot remove itself with _?=; upgrade/CI callers
  ; delete that one executable after ExecWait. Normal uninstall self-copies.
  StrCmp $PreviousPlugins 1 0 remove_app
  !insertmacro DeletePluginFiles "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}" uninstall_failed
remove_app:
  SetOutPath "$TEMP"
  !insertmacro DeleteAppFiles "$INSTDIR" uninstall_failed
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\${PRODUCT_NAME}\${PRODUCT_NAME}.lnk"
  RMDir "$SMPROGRAMS\${PRODUCT_NAME}"
  DeleteRegKey HKCU "${REGKEY}"
  Goto uninstall_done
uninstall_failed:
  MessageBox MB_ICONSTOP "Some installed files could not be removed. Close programs using them and retry." /SD IDOK
  SetErrorLevel 1
  Abort
uninstall_done:
SectionEnd
