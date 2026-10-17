; MoltBrowser Windows Installer (NSIS / MUI2 script)
; Copyright 2026 GenEye AI Labs Inc.
;
; Packages a STAGED Windows build directory (the runtime files produced by
; the release-windows-selfhosted workflow's "Stage portable zip" step:
; chrome.exe + *.dll + *.pak + *.bin + *.dat, locales/, resources/, and the
; bundled tor/ocr/whisper dirs when present) into a per-machine setup .exe:
;   MoltBrowser-Windows-x64-Setup.exe
;
; Produces:
;   - Install to $PROGRAMFILES64\MoltBrowser (per-machine, admin)
;   - Start Menu shortcut + optional Desktop shortcut to MoltBrowser.exe
;   - Uninstaller + Add/Remove Programs registry entries
;   - App icon from branding\ when an .ico is available
;
; The workflow passes everything in as /D defines, e.g.:
;   makensis /DVERSION=0.2.1 ^
;            /DSTAGE_DIR=C:\cr\src\out\MoltBrowser\MoltBrowser-Windows-x64 ^
;            /DOUT_FILE=C:\...\MoltBrowser-Windows-x64-Setup.exe ^
;            installer\windows\moltbrowser.nsi
;
; Requires NSIS 3.x: https://nsis.sourceforge.io/

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

; ----------------------------------------------------------------------------
; Configurable defines (override on the makensis command line with /D...)
; ----------------------------------------------------------------------------

; Version string shown in the UI and Add/Remove Programs.
!ifndef VERSION
  !define VERSION "0.0.0"
!endif

; Directory holding the staged runtime files to package. Defaults to the
; portable-zip layout relative to this .nsi so a local `makensis` works too,
; but the workflow always passes an absolute /DSTAGE_DIR.
!ifndef STAGE_DIR
  !define STAGE_DIR "..\..\dist\MoltBrowser-Windows-x64"
!endif

; Output installer path. Default lands next to the staged dir's parent.
!ifndef OUT_FILE
  !define OUT_FILE "MoltBrowser-Windows-x64-Setup.exe"
!endif

; App icon: resolved from branding\ when present. Override with /DAPP_ICON.
!ifndef APP_ICON
  !define APP_ICON "..\..\branding\icon.ico"
!endif

; ----------------------------------------------------------------------------
; Constants
; ----------------------------------------------------------------------------
!define APP_NAME      "MoltBrowser"
!define APP_EXE       "MoltBrowser.exe"
!define PUBLISHER     "GenEye AI Labs Inc."
!define WEBSITE       "https://github.com/OneManNoCode/MoltBrowser"
!define UNINST_KEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser"

Name "${APP_NAME} ${VERSION}"
OutFile "${OUT_FILE}"
InstallDir "$PROGRAMFILES64\MoltBrowser"
InstallDirRegKey HKLM "Software\MoltBrowser" "InstallDir"
RequestExecutionLevel admin
Unicode True
; zlib (fast), NOT /SOLID lzma: the staged runtime tree is ~2GB and solid
; LZMA compression takes hours on the RAM-constrained self-hosted runner.
SetCompressor zlib

; ----------------------------------------------------------------------------
; Modern UI 2
; ----------------------------------------------------------------------------
!define MUI_ABORTWARNING

; Use the branding icon only when it actually exists, so a missing .ico does
; not abort the build (branding\ ships PNGs today, not an .ico).
!if /FileExists "${APP_ICON}"
  !define MUI_ICON   "${APP_ICON}"
  !define MUI_UNICON "${APP_ICON}"
!endif

; Pages
!insertmacro MUI_PAGE_WELCOME
!if /FileExists "..\..\LICENSE"
  !insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!endif
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

; Finish page: offer to launch the browser.
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Launch ${APP_NAME}"
!define MUI_FINISHPAGE_LINK "MoltBrowser on GitHub"
!define MUI_FINISHPAGE_LINK_LOCATION "${WEBSITE}"
!insertmacro MUI_PAGE_FINISH

; Uninstaller pages
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ----------------------------------------------------------------------------
; Version info (embedded in the setup .exe resources)
; ----------------------------------------------------------------------------
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName"    "${APP_NAME}"
VIAddVersionKey "CompanyName"    "${PUBLISHER}"
VIAddVersionKey "FileDescription" "MoltBrowser AI-Native Privacy Browser Setup"
VIAddVersionKey "FileVersion"    "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright 2026 ${PUBLISHER}"

; ----------------------------------------------------------------------------
; Install
; ----------------------------------------------------------------------------
Section "MoltBrowser (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"

  ; On-device AI runs in a sandboxed utility process that Windows confines
  ; with LPAC (Chromium's kOnDeviceModelExecution sandbox). An LPAC process
  ; can load MoltBrowser's own DLLs only if the install directory grants
  ; read + execute to the two capability SIDs Chrome's installer grants
  ; (chrome/installer/setup/configure_app_container_sandbox.cc,
  ; ConfigureAppContainerSandbox, at Chromium 152):
  ;   chromeInstallFiles      S-1-15-3-1024-3424233489-972189580-2057154623-
  ;                           747635277-1604371224-316187997-3786583170-
  ;                           1043257646
  ;   lpacChromeInstallFiles  S-1-15-3-1024-2302894289-466761758-1166120688-
  ;                           1039016420-2430351297-4240214049-4028510897-
  ;                           3317428798
  ; with FILE_GENERIC_READ | FILE_GENERIC_EXECUTE (icacls "RX", 0x1200A9),
  ; inherited by files and folders (CONTAINER_INHERIT_ACE |
  ; OBJECT_INHERIT_ACE = "(OI)(CI)"), as base::win::GrantAccessToPath does
  ; there. Like Chrome, before the files are extracted, so they inherit it;
  ; icacls also propagates it to anything already in the directory (an
  ; update). Best effort, as in Chrome: if it fails the browser works, but
  ; local models can't start. The portable ZIP can't do this; see
  ; docs/design/SANDBOXED_INFERENCE.md.
  DetailPrint "Allowing MoltBrowser's on-device AI sandbox to read $INSTDIR"
  nsExec::ExecToLog '"$SYSDIR\icacls.exe" "$INSTDIR" /grant "*S-1-15-3-1024-3424233489-972189580-2057154623-747635277-1604371224-316187997-3786583170-1043257646:(OI)(CI)(RX)" "*S-1-15-3-1024-2302894289-466761758-1166120688-1039016420-2430351297-4240214049-4028510897-3317428798:(OI)(CI)(RX)"'
  Pop $0
  StrCmp $0 "0" lpacAclDone 0
    DetailPrint "Warning: icacls returned $0; on-device AI may not start."
  lpacAclDone:

  ; Recursively copy the entire staged runtime tree. /r captures top-level
  ; files (exe/dll/pak/bin/dat) plus locales\, resources\, and the bundled
  ; tor\ ocr\ whisper\ dirs whenever the stage step includes them.
  File /r "${STAGE_DIR}\*.*"

  ; The portable-zip stage ships the raw chrome.exe. Shortcuts and registry
  ; below all point at MoltBrowser.exe, so normalize the name here: if the
  ; staged tree did not already rename it, rename in place after extraction.
  ; (Harmless no-op when MoltBrowser.exe is already present.)
  IfFileExists "$INSTDIR\${APP_EXE}" haveExe 0
    IfFileExists "$INSTDIR\chrome.exe" 0 haveExe
      Rename "$INSTDIR\chrome.exe" "$INSTDIR\${APP_EXE}"
  haveExe:

  ; Remember where we installed.
  WriteRegStr HKLM "Software\MoltBrowser" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\MoltBrowser" "Version" "${VERSION}"

  ; Uninstaller.
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Start Menu shortcuts.
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall ${APP_NAME}.lnk" "$INSTDIR\uninstall.exe"

  ; ---- Add/Remove Programs entry ----
  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayName"     "${APP_NAME}"
  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKLM "${UNINST_KEY}" "Publisher"       "${PUBLISHER}"
  WriteRegStr   HKLM "${UNINST_KEY}" "DisplayIcon"     "$\"$INSTDIR\${APP_EXE}$\""
  WriteRegStr   HKLM "${UNINST_KEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr   HKLM "${UNINST_KEY}" "QuietUninstallString" "$\"$INSTDIR\uninstall.exe$\" /S"
  WriteRegStr   HKLM "${UNINST_KEY}" "InstallLocation" "$\"$INSTDIR$\""
  WriteRegStr   HKLM "${UNINST_KEY}" "URLInfoAbout"    "${WEBSITE}"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1

  ; EstimatedSize (in KB) for Add/Remove Programs.
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

; Optional Desktop shortcut (user can untick it on the components page).
Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

; Component descriptions.
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain}    "The MoltBrowser application files (required)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Place a MoltBrowser shortcut on the Desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ----------------------------------------------------------------------------
; Uninstall
; ----------------------------------------------------------------------------
Section "Uninstall"
  ; Remove the whole install tree (files + locales/resources/tor/ocr/whisper).
  RMDir /r "$INSTDIR"

  ; Shortcuts.
  RMDir /r "$SMPROGRAMS\${APP_NAME}"
  Delete "$DESKTOP\${APP_NAME}.lnk"

  ; Registry.
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "Software\MoltBrowser"
SectionEnd
