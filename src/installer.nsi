Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"
!include "nsDialogs.nsh"
Var RegisteredDir
Var AllUsers
Var ModeDialog
Var JustMeRadio
Var EveryoneRadio
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
VIAddVersionKey "FileDescription" "Qrazy per-user installer"
VIAddVersionKey "FileVersion" "${QRAZY_VERSION}"
VIAddVersionKey "LegalCopyright" "Qrazy contributors"
!define MUI_ICON "${QRAZY_ROOT}\..\..\src\assets\qrazy.ico"
!define MUI_UNICON "${QRAZY_ROOT}\..\..\src\assets\qrazy.ico"
!insertmacro MUI_PAGE_WELCOME
Page custom ChooseMode LeaveMode
!insertmacro MUI_PAGE_COMPONENTS
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE CheckSelection
!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose a dedicated folder for Qrazy. Use a location your Windows account can write to so client updates can replace the application. Your saved profile stays in its existing location."
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_TEXT "Qrazy has been installed.$\r$\n$\r$\nUse the Start menu shortcut to play. Client updates are available from the game menu.$\r$\n$\r$\nYour remembered login, settings and downloaded assets are kept in your existing profile."
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
FunctionEnd
!macroend
!insertmacro CheckPaths ""
!insertmacro CheckPaths "un."

!macro CheckClosed PREFIX
Function ${PREFIX}CheckClosed
  IfFileExists "$INSTDIR\Qrazy.exe" 0 check_electron
  System::Call 'kernel32::CreateFileW(w "$INSTDIR\Qrazy.exe", i 0x40000000, i 1, p 0, i 3, i 0, p 0) p.r0'
  IntCmp $0 -1 locked
  System::Call 'kernel32::CloseHandle(p r0)'
  check_electron:
  IfFileExists "$INSTDIR\runtime\electron.exe" 0 closed
  System::Call 'kernel32::CreateFileW(w "$INSTDIR\runtime\electron.exe", i 0x40000000, i 1, p 0, i 3, i 0, p 0) p.r0'
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
  StrCpy $AllUsers 0
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/ALLUSERS" $1
  ${IfNot} ${Errors}
    StrCpy $AllUsers 1
    Call EnsureAdmin
  ${EndIf}
  Call InitializeMode
FunctionEnd

Function EnsureAdmin
  System::Call 'shell32::IsUserAnAdmin() i.r0'
  ${If} $0 == 0
    System::Call 'shell32::ShellExecuteW(p 0, w "runas", w "$EXEPATH", w "/ALLUSERS", p 0, i 1) p.r0'
    ${If} $0 <= 32
      StrCpy $AllUsers 0
      MessageBox MB_OK|MB_ICONEXCLAMATION "Administrator approval was cancelled or could not be requested."
      Abort
    ${EndIf}
    Quit
  ${EndIf}
FunctionEnd

Function InitializeMode
  ${If} $AllUsers == 1
    SetShellVarContext all
    StrCpy $INSTDIR "$PROGRAMFILES64\Qrazy"
    StrCpy $InstallerHome "$COMMONFILES64\QrazyInstaller"
  ${Else}
    SetShellVarContext current
    StrCpy $INSTDIR "$LOCALAPPDATA\Programs\Qrazy"
    StrCpy $InstallerHome "$LOCALAPPDATA\QrazyInstaller"
  ${EndIf}
  ReadRegStr $RegisteredDir SHCTX "${REGKEY}" "InstallLocation"
  StrCmp $RegisteredDir "" +2
    StrCpy $INSTDIR $RegisteredDir
FunctionEnd

Function ChooseMode
  ${If} $AllUsers == 1
    Abort
  ${EndIf}
  !insertmacro MUI_HEADER_TEXT "Who should be able to use Qrazy?" "Choose how Qrazy is installed on this computer."
  nsDialogs::Create 1018
  Pop $ModeDialog
  ${NSD_CreateRadioButton} 0 15u 100% 18u "Just me"
  Pop $JustMeRadio
  ${NSD_Check} $JustMeRadio
  ${NSD_CreateLabel} 12u 35u 90% 25u "Install for your Windows account. No administrator approval needed."
  Pop $0
  ${NSD_CreateRadioButton} 0 72u 100% 18u "Everyone"
  Pop $EveryoneRadio
  ${NSD_CreateLabel} 12u 92u 90% 32u "Install for all Windows accounts. Installing, updating and uninstalling require administrator approval."
  Pop $0
  nsDialogs::Show
FunctionEnd

Function LeaveMode
  ${NSD_GetState} $EveryoneRadio $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $AllUsers 1
    Call EnsureAdmin
  ${Else}
    StrCpy $AllUsers 0
  ${EndIf}
  Call InitializeMode
FunctionEnd
Function CheckSelection
  Call CheckPaths
  ${If} $AllUsers == 1
    StrLen $0 "$PROGRAMFILES64\"
    StrCpy $1 $INSTDIR $0
    StrCmp $1 "$PROGRAMFILES64\" everyone_path_ok
    StrLen $0 "$PROGRAMFILES\"
    StrCpy $1 $INSTDIR $0
    StrCmp $1 "$PROGRAMFILES\" everyone_path_ok
    MessageBox MB_OK|MB_ICONEXCLAMATION "For Everyone, choose a dedicated folder inside Program Files so application and update files are protected. Choose Just me to use another writable location."
    Abort
    everyone_path_ok:
  ${EndIf}
  StrCmp $RegisteredDir "" selection_contents
  StrCmp $INSTDIR $RegisteredDir selection_contents
    MessageBox MB_OK|MB_ICONEXCLAMATION "To move an existing installation, uninstall it first, then choose the new folder. Your saved profile will be kept."
    Abort
  selection_contents:
  FindFirst $0 $1 "$INSTDIR\*"
  selection_next:
    StrCmp $1 "" selection_done
    StrCmp $1 "." selection_skip
    StrCmp $1 ".." selection_skip
    StrCmp $INSTDIR $RegisteredDir selection_done
    FindClose $0
    IfFileExists "$INSTDIR\Qrazy.exe" 0 selection_not_qrazy
    IfFileExists "$INSTDIR\runtime\electron.exe" 0 selection_not_qrazy
    IfFileExists "$INSTDIR\runtime\resources\app.asar" 0 selection_not_qrazy
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
  StrCpy $AllUsers 0
  ${GetParameters} $0
  ClearErrors
  ${GetOptions} $0 "/ALLUSERS" $1
  ${IfNot} ${Errors}
    StrCpy $AllUsers 1
    System::Call 'shell32::IsUserAnAdmin() i.r0'
    ${If} $0 == 0
      System::Call 'shell32::ShellExecuteW(p 0, w "runas", w "$EXEPATH", w "/ALLUSERS", p 0, i 1) p.r0'
      ${If} $0 <= 32
        Abort
      ${EndIf}
      Quit
    ${EndIf}
    SetShellVarContext all
    StrCpy $InstallerHome "$COMMONFILES64\QrazyInstaller"
  ${Else}
    SetShellVarContext current
    StrCpy $InstallerHome "$LOCALAPPDATA\QrazyInstaller"
  ${EndIf}
  ReadRegStr $INSTDIR SHCTX "${REGKEY}" "InstallLocation"
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
  ; FindFirst/FindNext and missing registry entries set a sticky error flag.
  ; Only failures from the installation operations below count as failures.
  ClearErrors
  StrCpy $InstallStep "creating the installation folder"
  SetOverwrite on
  SetOutPath "$INSTDIR"
  IfErrors install_failed
  StrCpy $InstallStep "extracting Qrazy.exe"
  File "${QRAZY_ROOT}\Qrazy.exe"
  IfErrors install_failed
  StrCpy $InstallStep "extracting the license"
  File "${QRAZY_ROOT}\LICENSE"
  IfErrors install_failed
  StrCpy $InstallStep "creating the runtime folder"
  SetOutPath "$INSTDIR\runtime"
  IfErrors install_failed
  StrCpy $InstallStep "extracting the runtime files"
  File /r "${QRAZY_ROOT}\runtime\*"
  IfErrors install_failed
  ; This folder is deliberately outside the application swap used by updates.
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
  WriteRegStr SHCTX "${REGKEY}" "DisplayName" "Qrazy"
  WriteRegStr SHCTX "${REGKEY}" "DisplayVersion" "${QRAZY_VERSION}"
  WriteRegStr SHCTX "${REGKEY}" "Publisher" "Qrazy"
  WriteRegStr SHCTX "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr SHCTX "${REGKEY}" "DisplayIcon" "$INSTDIR\Qrazy.exe"
  ${If} $AllUsers == 1
    WriteRegStr SHCTX "${REGKEY}" "UninstallString" '$\"$InstallerHome\Uninstall.exe$\" /ALLUSERS'
    WriteRegStr SHCTX "${REGKEY}" "QuietUninstallString" '$\"$InstallerHome\Uninstall.exe$\" /ALLUSERS /S'
  ${Else}
    WriteRegStr SHCTX "${REGKEY}" "UninstallString" '$\"$InstallerHome\Uninstall.exe$\"'
    WriteRegStr SHCTX "${REGKEY}" "QuietUninstallString" '$\"$InstallerHome\Uninstall.exe$\" /S'
  ${EndIf}
  WriteRegDWORD SHCTX "${REGKEY}" "NoModify" 1
  WriteRegDWORD SHCTX "${REGKEY}" "NoRepair" 1
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
  WriteRegDWORD SHCTX "${REGKEY}" "DesktopShortcut" 1
SectionEnd

Section "Uninstall"
  Call un.CheckPaths
  Call un.CheckClosed
  ; Remove only the owned package files, never the shared user profile.
  Delete "$INSTDIR\Qrazy.exe"
  Delete "$INSTDIR\LICENSE"
  RMDir /r "$INSTDIR\runtime"
  IfFileExists "$INSTDIR\Qrazy.exe" uninstall_failed
  IfFileExists "$INSTDIR\LICENSE" uninstall_failed
  IfFileExists "$INSTDIR\runtime\*" uninstall_failed
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Qrazy.lnk"
  StrCpy $0 0
  ReadRegDWORD $0 SHCTX "${REGKEY}" "DesktopShortcut"
  IntCmp $0 1 0 desktop_done desktop_done
    Delete "$DESKTOP\Qrazy.lnk"
  desktop_done:
  DeleteRegKey SHCTX "${REGKEY}"
  Delete "$InstallerHome\Uninstall.exe"
  RMDir "$InstallerHome"
  Goto uninstall_done
  uninstall_failed:
    MessageBox MB_OK|MB_ICONSTOP "Some Qrazy files could not be removed. Close Qrazy and try uninstalling again. Your profile has been kept."
    SetErrorLevel 1
    Abort
  uninstall_done:
SectionEnd
