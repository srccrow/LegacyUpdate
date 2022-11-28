!define REGPATH_RUNONCE "Software\Microsoft\Windows\CurrentVersion\RunOnce"

!define EWX_REBOOT 0x02

!define IsNativeIA64 '${IsNativeMachineArchitecture} ${IMAGE_FILE_MACHINE_IA64}'

Function GetArch
	${If} ${IsNativeIA32}
		Push "x86"
	${ElseIf} ${IsNativeAMD64}
		Push "x64"
	${ElseIf} ${IsNativeIA64}
		Push "ia64"
	${Else}
		Push ""
	${EndIf}
FunctionEnd

!macro DetailPrint text
	SetDetailsPrint both
	DetailPrint "${text}"
	SetDetailsPrint listonly
!macroend

!macro Download name url filename
	!insertmacro DetailPrint "Downloading ${name}..."
	inetc::get \
		/bgcolor FFFFFF /textcolor 000000 \
		"${url}" "${filename}" \
		/end
	Pop $0
	${If} $0 != "OK"
		${If} $0 != "Cancelled"
			MessageBox MB_OK|MB_USERICON "${name} failed to download.$\r$\n$\r$\n$0" /SD IDOK
		${EndIf}
		SetErrorLevel 1
		Abort
	${EndIf}
!macroend

!macro ExecWithErrorHandling name command iswusa
	ExecWait '${command}' $0
	${If} $0 == ${ERROR_SUCCESS_REBOOT_REQUIRED}
		SetRebootFlag true
	${ElseIf} $0 == ${ERROR_INSTALL_USEREXIT}
		SetErrorLevel ${ERROR_INSTALL_USEREXIT}
		Abort
	${ElseIf} ${iswusa} == 1
	${AndIf} $0 == 1
		; wusa exits with 1 if the patch is already installed. Treat this as success.
		Return
	${ElseIf} $0 != 0
		MessageBox MB_OK|MB_USERICON "${name} failed to install.$\r$\n$\r$\nError code: $0" /SD IDOK
		SetErrorLevel $0
		Abort
	${EndIf}
!macroend

!macro DownloadAndInstall name url filename args
	${If} ${FileExists} "$EXEDIR\${filename}"
		StrCpy $0 "$EXEDIR\${filename}"
	${Else}
		!insertmacro Download '${name}' '${url}' '${filename}'
		StrCpy $0 "${filename}"
	${EndIf}

	!insertmacro DetailPrint "Installing ${name}..."
	!insertmacro ExecWithErrorHandling '${name}' '$0 ${args}' 0
!macroend

!macro DownloadAndInstallSP name url filename
	${If} ${FileExists} "$EXEDIR\${filename}.exe"
		StrCpy $0 "$EXEDIR\${filename}.exe"
	${Else}
		!insertmacro Download '${name}' '${url}' '${filename}.exe'
		StrCpy $0 "${filename}.exe"
	${EndIf}

	; SPInstall.exe /norestart seems to be broken. We let it do a delayed restart, then cancel it.
	!insertmacro DetailPrint "Extracting ${name}..."
	!insertmacro ExecWithErrorHandling '${name}' '$0 /X:"$PLUGINSDIR\${filename}"' 0
	!insertmacro DetailPrint "Installing ${name}..."
	!insertmacro ExecWithErrorHandling '${name}' '${filename}\spinstall.exe /unattend /nodialog /warnrestart:600' 0

	; If we successfully abort a shutdown, we'll get exit code 0, so we know a reboot is required.
	ExecWait "shutdown.exe /a" $0
	${If} $0 == 0
		SetRebootFlag true
	${EndIf}
!macroend

!macro DownloadAndInstallMSU name url
	${If} ${FileExists} "$EXEDIR\${name}.msu"
		StrCpy $0 "$EXEDIR\${name}.msu"
	${Else}
		!insertmacro Download '${name}' '${url}' '${name}.msu'
		StrCpy $0 "${name}.msu"
	${EndIf}

	; Stop AU service before running wusa so it doesn't try checking for updates online first (which
	; may never complete before we install our patches).
	!insertmacro DetailPrint "Installing ${name}..."
	SetDetailsPrint none
	ExecShellWait "" "net" "stop wuauserv" SW_HIDE
	SetDetailsPrint listonly
	!insertmacro ExecWithErrorHandling '${name}' 'wusa.exe /quiet /norestart $0' 1
!macroend

!macro EnsureAdminRights
	UserInfo::GetAccountType
	Pop $0
	${If} $0 != "admin" ; Require admin rights on NT4+
		MessageBox MB_USERICON "Log on as an administrator to install Legacy Update." /SD IDOK
		SetErrorLevel ERROR_ELEVATION_REQUIRED
		Quit
	${EndIf}
!macroend

!macro DeleteFileOrAskAbort path
	ClearErrors
	Delete "${path}"
	IfErrors 0 +3
		MessageBox MB_RETRYCANCEL|MB_USERICON 'Unable to delete "${path}".$\r$\n$\r$\nIf Internet Explorer is open, close it and click Retry.' /SD IDCANCEL IDRETRY -3
		Abort
!macroend

!macro -PromptReboot
	SetErrorLevel ${ERROR_SUCCESS_REBOOT_REQUIRED}

	${GetParameters} $0
	ClearErrors
	${GetOptions} $0 /norestart $1
	${If} ${Errors}
		; Reboot immediately
		Reboot
	${Else}
		; Prompt for reboot
		${IfNot} ${Silent}
			System::Call 'Shell32::RestartDialog(p $HWNDPARENT, \
				t "Windows will be restarted to complete installation of prerequisite components. Setup will resume after the restart.", \
				i ${EWX_REBOOT})'
		${EndIf}
	${EndIf}
!macroend

!macro -RebootIfRequired
	${If} ${RebootFlag}
		; Copy to a local path, just in case the installer is on a network share, or the user next logs
		; in as a different user.
		CreateDirectory "$INSTDIR"
		CopyFiles /SILENT "$EXEPATH" "$INSTDIR\LegacyUpdateSetup.exe"
		${GetParameters} $0
		WriteRegStr HKLM "${REGPATH_RUNONCE}" "Legacy Update" '"$INSTDIR\LegacyUpdateSetup.exe" $0 /runonce'
		!insertmacro -PromptReboot
		Quit
	${EndIf}
!macroend

Function RebootIfRequired
	${MementoSectionSave}
	!insertmacro -RebootIfRequired
FunctionEnd

Function un.RebootIfRequired
	!insertmacro -RebootIfRequired
FunctionEnd
