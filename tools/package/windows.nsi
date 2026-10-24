; SPDX-License-Identifier: GPL-3.0-or-later
Unicode true
!include "MUI2.nsh"
!include "Sections.nsh"
!include "FileFunc.nsh"
!include "${MANIFEST_DIR}\app-files.nsh"
!include "${MANIFEST_DIR}\plugin-files.nsh"
!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_SLUG}"
Var PreviousInstall
Var PreviousPlugins
Var SafetyRoot
Var SafetyPath
Var SafetyOK
Var InventoryFile
Var InventoryEntry
Var InventoryOwned
Var CandidatePath
Var CandidateKind
Var DestinationOK
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

; Only private payload directories are guarded. Check the root as well as
; every directory between it and a payload file; never traverse a junction.
!macro DirectorySafety prefix
Function ${prefix}CheckDirectoryChain
  Push $0
  Push $1
  StrCpy $0 $SafetyPath
  StrCpy $SafetyOK 1
check_directory:
  System::Call 'kernel32::GetFileAttributesW(w r0) i .r1'
  IntCmp $1 -1 parent_directory
  IntOp $1 $1 & 0x410 ; DIRECTORY plus REPARSE_POINT
  IntCmp $1 0x10 parent_directory unsafe_directory unsafe_directory
parent_directory:
  StrCmp $0 $SafetyRoot directory_done
  ${GetParent} "$0" $1
  StrCmp $0 $1 unsafe_directory
  StrCpy $0 $1
  Goto check_directory
unsafe_directory:
  StrCpy $SafetyOK 0
directory_done:
  Pop $1
  Pop $0
FunctionEnd
!macroend
!insertmacro DirectorySafety ""
!insertmacro DirectorySafety "un."

Function InventoryContains
  Push $0
  Push $1
  StrCpy $InventoryOwned 0
  ClearErrors
  FileOpen $0 "$InventoryFile" r
  IfErrors inventory_done
inventory_next:
  ClearErrors
  FileReadUTF16LE $0 $1
  IfErrors inventory_close
  StrCmp $1 $InventoryEntry 0 inventory_next
  StrCpy $InventoryOwned 1
inventory_close:
  FileClose $0
inventory_done:
  Pop $1
  Pop $0
FunctionEnd

Function CheckDestination
  Push $0
  Push $1
  StrCpy $DestinationOK 1
  System::Call 'kernel32::GetFileAttributesW(w "$CandidatePath") i .r0'
  IntCmp $0 -1 destination_done
  IntOp $1 $0 & 0x10
  StrCmp $CandidateKind "D" destination_directory
  IntCmp $1 0 destination_ownership destination_bad destination_bad
destination_directory:
  IntCmp $1 0 destination_bad destination_ownership destination_ownership
destination_ownership:
  Call InventoryContains
  StrCmp $InventoryOwned 1 destination_done
destination_bad:
  StrCpy $DestinationOK 0
destination_done:
  Pop $1
  Pop $0
FunctionEnd

Function ValidateTarget
  ; A populated folder is allowed only for this registered installation.
  StrCmp $INSTDIR $PreviousInstall 0 check_empty
  StrCmp $PreviousInstall "" check_empty
  IfFileExists "$INSTDIR\uninstall.exe" target_valid
check_empty:
  ClearErrors
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
  ClearErrors
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
  Call PreflightUpgrade
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
  SetOutPath "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}"
  ClearErrors
  File /r "${STAGE}\plugins\clap\*"
  IfErrors plugin_failed
  WriteRegDWORD HKCU "${REGKEY}" "StockPlugins" 1
  Goto plugin_done
plugin_failed:
  MessageBox MB_ICONSTOP "Could not restore the selected stock plugins. Check folder permissions and retry." /SD IDOK
  SetErrorLevel 1
  Abort
plugin_done:
SectionEnd

Function PreflightUpgrade
  !insertmacro CheckAppDirectories "$INSTDIR" preflight_refused ""
  StrCpy $InventoryFile ""
  StrCmp $PreviousInstall "" app_preflight
  !insertmacro CheckAppDirectories "$PreviousInstall" preflight_refused ""
  StrCmp $PreviousInstall $INSTDIR 0 app_preflight
  StrCpy $InventoryFile "$PreviousInstall\.installed-files"
app_preflight:
  !insertmacro PreflightApp "$INSTDIR" "$InventoryFile" preflight_refused
  SectionGetFlags ${SEC_PLUGINS} $0
  IntOp $0 $0 & ${SF_SELECTED}
  StrCmp $0 0 preflight_done
  !insertmacro CheckPluginDirectories "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}" preflight_refused ""
  StrCpy $InventoryFile ""
  StrCmp $PreviousPlugins 1 0 plugin_preflight
  StrCpy $InventoryFile "$PreviousInstall\.installed-plugins"
plugin_preflight:
  !insertmacro PreflightPlugin "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}" "$InventoryFile" preflight_refused
preflight_done:
  Return
preflight_refused:
  MessageBox MB_ICONSTOP "Upgrade refused: a payload path is unowned, has the wrong type, or uses a directory link. The existing installation was left intact." /SD IDOK
  SetErrorLevel 1
  Abort
FunctionEnd

Function .onInit
  ReadRegStr $PreviousInstall HKCU "${REGKEY}" "InstallLocation"
  ReadRegDWORD $PreviousPlugins HKCU "${REGKEY}" "StockPlugins"
  StrCmp $PreviousInstall "" read_switch
  StrCpy $INSTDIR $PreviousInstall
  StrCmp $PreviousPlugins 1 0 read_switch
  SectionSetFlags ${SEC_PLUGINS} ${SF_SELECTED}
read_switch:
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} "$0" "/PLUGINS" $1
  IfErrors init_done
  SectionSetFlags ${SEC_PLUGINS} ${SF_SELECTED}
init_done:
FunctionEnd

Function un.onInit
  ReadRegStr $0 HKCU "${REGKEY}" "InstallLocation"
  StrCmp $0 $INSTDIR 0 uninstall_unregistered
  !insertmacro CheckAppDirectories "$INSTDIR" uninstall_unsafe "un."
  !insertmacro CheckAppUnlocked "$INSTDIR" uninstall_locked app
  ReadRegDWORD $PreviousPlugins HKCU "${REGKEY}" "StockPlugins"
  StrCmp $PreviousPlugins 1 0 uninstall_ready
  !insertmacro CheckPluginDirectories "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}" uninstall_unsafe "un."
  !insertmacro CheckPluginUnlocked "$LOCALAPPDATA\Programs\Common\CLAP\${PRODUCT_SLUG}" uninstall_locked optional
uninstall_ready:
  Return
uninstall_unregistered:
  MessageBox MB_ICONSTOP "This folder is not the registered installation; no files were removed." /SD IDOK
  SetErrorLevel 1
  Abort
uninstall_unsafe:
  MessageBox MB_ICONSTOP "Uninstall refused a directory link or reparse point. Restore the original payload directories and retry; ownership metadata was kept." /SD IDOK
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
  StrCpy $SafetyRoot "$INSTDIR"
  StrCpy $SafetyPath "$INSTDIR"
  Call un.CheckDirectoryChain
  StrCmp $SafetyOK 1 0 uninstall_failed
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
