; MoltBrowser Windows Installer (NSIS Script)
; Copyright 2025 GenEye AI Labs Inc.
;
; Creates a professional Windows installer with:
;   - Custom install directory
;   - Desktop + Start Menu shortcuts
;   - File associations (HTTP/HTTPS)
;   - Default browser registration
;   - Uninstaller with proper cleanup
;   - WinSparkle auto-update DLL
;
; Build with: makensis -DVERSION=0.1.0 moltbrowser.nsi
; Requires NSIS 3.x: https://nsis.sourceforge.io/

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

; ---- Configuration ----
!ifndef VERSION
  !define VERSION "0.1.0"
!endif

Name "MoltBrowser ${VERSION}"
OutFile "..\..\dist\MoltBrowser-${VERSION}-win-x64-setup.exe"
InstallDir "$PROGRAMFILES64\MoltBrowser"
InstallDirRegKey HKLM "Software\MoltBrowser" "InstallDir"
RequestExecutionLevel admin
Unicode True

; ---- Branding ----
!define MUI_ICON "..\..\branding\icon.ico"
!define MUI_UNICON "..\..\branding\icon.ico"
!define MUI_HEADERIMAGE
!define MUI_WELCOMEFINISHPAGE_BITMAP_STRETCH FitControl
!define MUI_ABORTWARNING

; ---- Installer Pages ----
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

; Finish page — launch browser option
!define MUI_FINISHPAGE_RUN "$INSTDIR\MoltBrowser.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Launch MoltBrowser"
!define MUI_FINISHPAGE_LINK "Visit moltbrowser.com"
!define MUI_FINISHPAGE_LINK_LOCATION "https://moltbrowser.com"
!insertmacro MUI_PAGE_FINISH

; ---- Uninstaller Pages ----
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; ---- Languages ----
!insertmacro MUI_LANGUAGE "English"

; ---- Version Info ----
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "MoltBrowser"
VIAddVersionKey "CompanyName" "GenEye AI Labs Inc."
VIAddVersionKey "FileDescription" "MoltBrowser AI-Native Privacy Browser"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "Copyright 2025 GenEye AI Labs Inc."

; ---- Install Section ----
Section "MoltBrowser" SecMain
  SectionIn RO ; Read-only (required)
  SetOutPath $INSTDIR

  ; Copy all build artifacts
  File /r "..\..\dist\MoltBrowser-${VERSION}-win-x64\*.*"

  ; Store install directory
  WriteRegStr HKLM "Software\MoltBrowser" "InstallDir" "$INSTDIR"
  WriteRegStr HKLM "Software\MoltBrowser" "Version" "${VERSION}"

  ; Create uninstaller
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Start Menu shortcuts
  CreateDirectory "$SMPROGRAMS\MoltBrowser"
  CreateShortcut "$SMPROGRAMS\MoltBrowser\MoltBrowser.lnk" "$INSTDIR\MoltBrowser.exe" \
    "" "$INSTDIR\MoltBrowser.exe" 0
  CreateShortcut "$SMPROGRAMS\MoltBrowser\Uninstall MoltBrowser.lnk" "$INSTDIR\uninstall.exe"

  ; Desktop shortcut
  CreateShortcut "$DESKTOP\MoltBrowser.lnk" "$INSTDIR\MoltBrowser.exe" \
    "" "$INSTDIR\MoltBrowser.exe" 0

  ; Register as available browser (Add/Remove Programs)
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "DisplayName" "MoltBrowser"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "DisplayIcon" "$\"$INSTDIR\MoltBrowser.exe$\""
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "Publisher" "GenEye AI Labs Inc."
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "URLInfoAbout" "https://moltbrowser.com"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "URLUpdateInfo" "https://moltbrowser.com"

  ; Calculate installed size
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser" \
    "EstimatedSize" "$0"

  ; Register as HTTP/HTTPS handler (user can set as default in Windows Settings)
  WriteRegStr HKLM "Software\RegisteredApplications" "MoltBrowser" \
    "Software\Clients\StartMenuInternet\MoltBrowser\Capabilities"

  WriteRegStr HKLM "Software\Clients\StartMenuInternet\MoltBrowser" "" "MoltBrowser"
  WriteRegStr HKLM "Software\Clients\StartMenuInternet\MoltBrowser\DefaultIcon" "" \
    "$INSTDIR\MoltBrowser.exe,0"
  WriteRegStr HKLM "Software\Clients\StartMenuInternet\MoltBrowser\shell\open\command" "" \
    "$\"$INSTDIR\MoltBrowser.exe$\""

  ; URL Associations capability
  WriteRegStr HKLM "Software\Clients\StartMenuInternet\MoltBrowser\Capabilities" \
    "ApplicationDescription" "AI-Native Privacy Browser with on-device LLM inference"
  WriteRegStr HKLM "Software\Clients\StartMenuInternet\MoltBrowser\Capabilities" \
    "ApplicationName" "MoltBrowser"

  WriteRegStr HKLM "Software\Clients\StartMenuInternet\MoltBrowser\Capabilities\URLAssociations" \
    "http" "MoltBrowserURL"
  WriteRegStr HKLM "Software\Clients\StartMenuInternet\MoltBrowser\Capabilities\URLAssociations" \
    "https" "MoltBrowserURL"

  ; File type associations capability
  WriteRegStr HKLM "Software\Clients\StartMenuInternet\MoltBrowser\Capabilities\FileAssociations" \
    ".htm" "MoltBrowserHTML"
  WriteRegStr HKLM "Software\Clients\StartMenuInternet\MoltBrowser\Capabilities\FileAssociations" \
    ".html" "MoltBrowserHTML"

  ; URL handler registration
  WriteRegStr HKLM "Software\Classes\MoltBrowserURL" "" "MoltBrowser URL"
  WriteRegStr HKLM "Software\Classes\MoltBrowserURL\shell\open\command" "" \
    "$\"$INSTDIR\MoltBrowser.exe$\" $\"%1$\""

  WriteRegStr HKLM "Software\Classes\MoltBrowserHTML" "" "MoltBrowser HTML Document"
  WriteRegStr HKLM "Software\Classes\MoltBrowserHTML\shell\open\command" "" \
    "$\"$INSTDIR\MoltBrowser.exe$\" $\"%1$\""

  ; Notify Windows of changes
  System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'
SectionEnd

; ---- Uninstall Section ----
Section "Uninstall"
  ; Remove files
  RMDir /r "$INSTDIR"

  ; Remove shortcuts
  RMDir /r "$SMPROGRAMS\MoltBrowser"
  Delete "$DESKTOP\MoltBrowser.lnk"

  ; Remove registry entries
  DeleteRegKey HKLM "Software\MoltBrowser"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\MoltBrowser"
  DeleteRegKey HKLM "Software\Clients\StartMenuInternet\MoltBrowser"
  DeleteRegValue HKLM "Software\RegisteredApplications" "MoltBrowser"
  DeleteRegKey HKLM "Software\Classes\MoltBrowserURL"
  DeleteRegKey HKLM "Software\Classes\MoltBrowserHTML"

  ; Notify Windows
  System::Call 'Shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'
SectionEnd
