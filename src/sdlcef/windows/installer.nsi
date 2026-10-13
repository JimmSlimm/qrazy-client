Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"
!include "nsDialogs.nsh"
Var RegisteredDir
Var InstallerHome
Var InstallStep
Var ApprovedRepairDir
!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Qrazy"
Name "Qrazy"
OutFile "${QRAZY_OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\Qrazy"
RequestExecutionLevel user
SetCompressor /SOLID lzma
SetCompressorDictSize 32
ShowInstDetails show
ShowUninstDetails show
VIProductVersion "${QRAZY_VERSION}.0"
VIAddVersionKey "ProductName" "Qrazy"
VIAddVersionKey "FileDescription" "Qrazy installer"
VIAddVersionKey "FileVersion" "${QRAZY_VERSION}"
VIAddVersionKey "LegalCopyright" "Qrazy contributors"
!define MUI_ICON "${QRAZY_ICON}"
!define MUI_UNICON "${QRAZY_ICON}"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE CheckSelection
!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose a dedicated folder for Qrazy. Use a location your Windows account can write to so client updates can replace the application. Your saved profile is kept inside this folder."
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "Qrazy has been installed.$\r$\n$\r$\nUse the Start menu shortcut to play. Qrazy updates itself.$\r$\n$\r$\nYour remembered login, settings and downloaded assets are kept."
!insertmacro MUI_PAGE_FINISH
!define MUI_UNCONFIRMPAGE_TEXT_TOP "Remove the installed Qrazy client and its shortcuts?$\r$\n$\r$\nYour remembered login, settings and downloaded assets will be kept."
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

!macro CheckTree PREFIX
Function ${PREFIX}CheckTree
  Exch $R0
  Push $R1
  Push $R2
  Push $R3
  FindFirst $R1 $R2 "$R0\*"
  tree_next:
    StrCmp $R2 "" tree_done
    StrCmp $R2 "." tree_skip
    StrCmp $R2 ".." tree_skip
    System::Call 'kernel32::GetFileAttributesW(w "$R0\$R2") i.r3'
    IntOp $R3 $3 & 0x400
    IntCmp $R3 0 tree_directory
      FindClose $R1
      MessageBox MB_OK|MB_ICONSTOP "The Qrazy runtime contains linked files or folders. No files were changed."
      Abort
    tree_directory:
    IntOp $R3 $3 & 0x10
    IntCmp $R3 0 tree_skip
      Push "$R0\$R2"
      Call ${PREFIX}CheckTree
    tree_skip:
    FindNext $R1 $R2
    Goto tree_next
  tree_done:
  FindClose $R1
  Pop $R3
  Pop $R2
  Pop $R1
  Pop $R0
FunctionEnd
!macroend
!insertmacro CheckTree ""
!insertmacro CheckTree "un."

!macro CheckAncestors PREFIX
Function ${PREFIX}CheckAncestors
  Exch $R0
  Push $R1
  ancestors_next:
    StrCmp $R0 "" ancestors_done
    System::Call 'kernel32::GetFileAttributesW(w "$R0") i.r1'
    IntCmp $1 -1 ancestors_parent
    IntOp $R1 $1 & 0x400
    IntCmp $R1 0 ancestors_parent
      MessageBox MB_OK|MB_ICONSTOP "Qrazy cannot install or uninstall through linked files or folders. No files were changed."
      Abort
    ancestors_parent:
    ${GetParent} "$R0" $R0
    Goto ancestors_next
  ancestors_done:
  Pop $R1
  Pop $R0
FunctionEnd
!macroend
!insertmacro CheckAncestors ""
!insertmacro CheckAncestors "un."

!macro CheckPaths PREFIX
Function ${PREFIX}CheckPaths
  ; Always resolve and check the final target before any file replacement/removal.
  System::Call 'kernel32::GetFullPathNameW(w "$INSTDIR", i ${NSIS_MAX_STRLEN}, w .r0, p 0) i.r1'
  ${If} $1 == 0
  ${OrIf} $1 >= ${NSIS_MAX_STRLEN}
    MessageBox MB_OK|MB_ICONSTOP "The installation path is invalid or too long."
    Abort
  ${EndIf}
  StrCpy $INSTDIR $0
  StrCpy $0 $INSTDIR 1 1
  StrCmp $0 ":" +3
    MessageBox MB_OK|MB_ICONSTOP "The installation path is invalid. No files were changed."
    Abort
  ${GetRoot} "$INSTDIR" $0
  StrCmp $INSTDIR "$0\" 0 +3
    MessageBox MB_OK|MB_ICONSTOP "Choose a dedicated Qrazy folder, not a drive root."
    Abort
  StrCmp $INSTDIR $0 0 +3
    MessageBox MB_OK|MB_ICONSTOP "Choose a dedicated Qrazy folder, not a drive root."
    Abort
  Push "$INSTDIR\runtime"
  Call ${PREFIX}CheckAncestors
  Push "$InstallerHome"
  Call ${PREFIX}CheckAncestors
  IfFileExists "$INSTDIR\runtime\*" 0 tree_checked
    Push "$INSTDIR\runtime"
    Call ${PREFIX}CheckTree
  tree_checked:
  IfFileExists "$INSTDIR\updater\*" 0 updater_checked
    Push "$INSTDIR\updater"
    Call ${PREFIX}CheckTree
  updater_checked:
FunctionEnd
!macroend
!insertmacro CheckPaths ""
!insertmacro CheckPaths "un."

!macro CheckClosed PREFIX
Function ${PREFIX}CheckClosed
  IfFileExists "$INSTDIR\Qrazy.exe" 0 check_runtime
  System::Call 'kernel32::CreateFileW(w "$INSTDIR\Qrazy.exe", i 0x40000000, i 1, p 0, i 3, i 0, p 0) p.r0'
  IntCmp $0 -1 locked
  System::Call 'kernel32::CloseHandle(p r0)'
  check_runtime:
  IfFileExists "$INSTDIR\runtime\Qrazy.exe" 0 closed
  System::Call 'kernel32::CreateFileW(w "$INSTDIR\runtime\Qrazy.exe", i 0x40000000, i 1, p 0, i 3, i 0, p 0) p.r0'
  IntCmp $0 -1 locked
  System::Call 'kernel32::CloseHandle(p r0)'
  Goto closed
  locked:
    IfSilent silent_locked
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "Please close Qrazy before continuing. Setup will not close it for you." IDRETRY retry
    Abort
  silent_locked:
    SetErrorLevel 2
    Abort
  retry:
    Call ${PREFIX}CheckClosed
  closed:
FunctionEnd
!macroend
!insertmacro CheckClosed ""
!insertmacro CheckClosed "un."

Function .onInit
  SetRegView 64
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "Qrazy requires 64-bit Windows."
    Abort
  ${EndIf}
  SetShellVarContext current
  StrCpy $INSTDIR "$LOCALAPPDATA\Programs\Qrazy"
  StrCpy $InstallerHome "$LOCALAPPDATA\QrazyInstaller"
  ReadRegStr $RegisteredDir HKCU "${REGKEY}" "InstallLocation"
  StrCmp $RegisteredDir "" +2
    StrCpy $INSTDIR $RegisteredDir
FunctionEnd

Function CheckSelection
  Call CheckPaths
  StrCmp $RegisteredDir "" selection_contents
  StrCmp $INSTDIR $RegisteredDir selection_contents
    MessageBox MB_OK|MB_ICONEXCLAMATION "To move an existing installation, uninstall it first, then choose the new folder. Your saved profile will be kept."
    Abort
  selection_contents:
  StrCmp $INSTDIR $RegisteredDir selection_done
  FindFirst $0 $1 "$INSTDIR\*"
  selection_next:
    StrCmp $1 "" selection_done
    StrCmp $1 "." selection_skip
    StrCmp $1 ".." selection_skip
    FindClose $0
    IfFileExists "$INSTDIR\Qrazy.exe" 0 selection_not_qrazy
    IfFileExists "$INSTDIR\runtime\Qrazy.dll" 0 selection_not_qrazy
    StrCmp $INSTDIR $ApprovedRepairDir selection_repair
    IfSilent selection_not_qrazy
    MessageBox MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION "Existing Qrazy files were found. Repair or install Qrazy in this folder?$\r$\n$\r$\nYour saved profile will be kept." IDYES selection_repair
    Abort
    selection_repair:
    StrCpy $ApprovedRepairDir $INSTDIR
    Return
    selection_not_qrazy:
    MessageBox MB_OK|MB_ICONEXCLAMATION "Choose an empty, dedicated folder for Qrazy. Existing files will not be replaced."
    Abort
  selection_skip:
    FindNext $0 $1
    Goto selection_next
  selection_done:
    FindClose $0
FunctionEnd

Function un.onInit
  SetRegView 64
  SetShellVarContext current
  StrCpy $InstallerHome "$LOCALAPPDATA\QrazyInstaller"
  ReadRegStr $INSTDIR HKCU "${REGKEY}" "InstallLocation"
  StrCmp $INSTDIR "" 0 +3
    MessageBox MB_OK|MB_ICONSTOP "The installed Qrazy location could not be found. No files were removed."
    Abort
  Call un.CheckPaths
  Call un.CheckClosed
FunctionEnd

Section "Qrazy"
  SectionIn RO
  Call CheckSelection
  Call CheckClosed
  ClearErrors
  StrCpy $InstallStep "creating the installation folder"
  SetOverwrite on
  SetOutPath "$INSTDIR"
  IfErrors install_failed
  ; Replace only the owned application folders so a repair leaves no stale files.
  ; Profiles live beside them and are never touched.
  RMDir /r "$INSTDIR\runtime"
  RMDir /r "$INSTDIR\updater"
  ClearErrors
  StrCpy $InstallStep "extracting Qrazy.exe"
  File "${QRAZY_ROOT}\Qrazy.exe"
  IfErrors install_failed
  StrCpy $InstallStep "extracting the license"
  File "${QRAZY_LICENSE}"
  IfErrors install_failed
  StrCpy $InstallStep "extracting the runtime files"
  SetOutPath "$INSTDIR\runtime"
  File /r "${QRAZY_ROOT}\runtime\*"
  IfErrors install_failed
  StrCpy $InstallStep "extracting the updater files"
  SetOutPath "$INSTDIR\updater"
  File /r "${QRAZY_ROOT}\updater\*"
  IfErrors install_failed
  StrCpy $InstallStep "creating the uninstaller folder"
  CreateDirectory "$InstallerHome"
  IfErrors install_failed
  StrCpy $InstallStep "writing the uninstaller"
  WriteUninstaller "$InstallerHome\Uninstall.exe"
  IfErrors install_failed
  StrCpy $InstallStep "creating the Start menu shortcut"
  SetOutPath "$INSTDIR"
  CreateShortcut "$SMPROGRAMS\Qrazy.lnk" "$INSTDIR\Qrazy.exe" "" "$INSTDIR\Qrazy.exe"
  IfErrors install_failed
  StrCpy $InstallStep "registering the installation"
  ; No DisplayVersion: Qrazy updates itself in place, so a stored number would go stale.
  WriteRegStr HKCU "${REGKEY}" "DisplayName" "Qrazy"
  WriteRegStr HKCU "${REGKEY}" "Publisher" "Qrazy"
  WriteRegStr HKCU "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${REGKEY}" "DisplayIcon" "$INSTDIR\Qrazy.exe"
  WriteRegStr HKCU "${REGKEY}" "UninstallString" '$\"$InstallerHome\Uninstall.exe$\"'
  WriteRegStr HKCU "${REGKEY}" "QuietUninstallString" '$\"$InstallerHome\Uninstall.exe$\" /S'
  WriteRegDWORD HKCU "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${REGKEY}" "NoRepair" 1
  IfErrors install_failed
  Goto install_done
  install_failed:
    MessageBox MB_OK|MB_ICONSTOP "Setup could not finish $InstallStep.$\r$\n$\r$\nInstallation folder: $INSTDIR$\r$\n$\r$\nClose Qrazy if it is running, check this folder's permissions and free space, then retry. Your saved profile has been kept."
    SetErrorLevel 1
    Abort
  install_done:
SectionEnd

Section /o "Desktop shortcut"
  CreateShortcut "$DESKTOP\Qrazy.lnk" "$INSTDIR\Qrazy.exe" "" "$INSTDIR\Qrazy.exe"
  WriteRegDWORD HKCU "${REGKEY}" "DesktopShortcut" 1
SectionEnd

Section "Uninstall"
  Call un.CheckPaths
  Call un.CheckClosed
  ; Remove only the owned package files, never the profile folders beside them.
  Delete "$INSTDIR\Qrazy.exe"
  Delete "$INSTDIR\LICENSE"
  RMDir /r "$INSTDIR\runtime"
  RMDir /r "$INSTDIR\updater"
  IfFileExists "$INSTDIR\Qrazy.exe" uninstall_failed
  IfFileExists "$INSTDIR\runtime\*" uninstall_failed
  IfFileExists "$INSTDIR\updater\*" uninstall_failed
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Qrazy.lnk"
  StrCpy $0 0
  ReadRegDWORD $0 HKCU "${REGKEY}" "DesktopShortcut"
  IntCmp $0 1 0 desktop_done desktop_done
    Delete "$DESKTOP\Qrazy.lnk"
  desktop_done:
  DeleteRegKey HKCU "${REGKEY}"
  Delete "$InstallerHome\Uninstall.exe"
  RMDir "$InstallerHome"
  Goto uninstall_done
  uninstall_failed:
    MessageBox MB_OK|MB_ICONSTOP "Some Qrazy files could not be removed. Close Qrazy and try uninstalling again. Your profile has been kept."
    SetErrorLevel 1
    Abort
  uninstall_done:
SectionEnd
