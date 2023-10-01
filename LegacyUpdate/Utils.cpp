#include "stdafx.h"
#include <comdef.h>
#include <atlstr.h>
#include <Wbemidl.h>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "advapi32.lib")

// Defined as being Vista+, older versions ignore the flag.
#ifndef EWX_RESTARTAPPS
#define EWX_RESTARTAPPS 0x00000040
#endif

extern "C" IMAGE_DOS_HEADER __ImageBase;

#define OwnModule ((HMODULE)&__ImageBase)

static BOOL _loadedVersionInfo = FALSE;
static OSVERSIONINFOEX _versionInfo;

static BOOL _loadedProductName = FALSE;
static CComVariant _productName;

static BOOL _loadedOwnVersion = FALSE;
static LPWSTR _version;
static UINT _versionSize;

OSVERSIONINFOEX *GetVersionInfo() {
	if (!_loadedVersionInfo) {
		_loadedVersionInfo = true;
		ZeroMemory(&_versionInfo, sizeof(OSVERSIONINFOEX));
		_versionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
		GetVersionEx((LPOSVERSIONINFO)&_versionInfo);
	}
	return &_versionInfo;
}

void GetOwnFileName(LPWSTR *filename, LPDWORD size) {
	*filename = (LPWSTR)malloc(MAX_PATH);
	*size = GetModuleFileName(OwnModule, *filename, MAX_PATH);
}

HRESULT GetOwnVersion(LPWSTR *version, LPDWORD size) {
	if (!_loadedOwnVersion) {
		LPWSTR filename;
		DWORD filenameSize;
		GetOwnFileName(&filename, &filenameSize);

		DWORD verHandle;
		DWORD verInfoSize = GetFileVersionInfoSize(filename, &verHandle);
		if (verInfoSize == 0) {
			return HRESULT_FROM_WIN32(GetLastError());
		}

		LPVOID verInfo = new BYTE[verInfoSize];
		if (!GetFileVersionInfo(filename, verHandle, verInfoSize, verInfo)) {
			return HRESULT_FROM_WIN32(GetLastError());
		}

		if (!VerQueryValue(verInfo, L"\\StringFileInfo\\040904B0\\ProductVersion", (LPVOID *)&_version, &_versionSize)) {
			return HRESULT_FROM_WIN32(GetLastError());
		}
	}

	*version = _version;
	*size = _versionSize;
	return _version == NULL ? E_FAIL : NOERROR;
}

HRESULT GetRegistryString(HKEY key, LPCWSTR subkeyPath, LPCWSTR valueName, LPDWORD type, LPWSTR *data, LPDWORD size) {
	HKEY subkey;
	HRESULT hr = HRESULT_FROM_WIN32(RegOpenKeyEx(key, subkeyPath, 0, KEY_READ, &subkey));
	if (!SUCCEEDED(hr)) {
		goto end;
	}

	if (data != NULL && size != NULL) {
		DWORD length = 512 * sizeof(WCHAR);
		LPWSTR buffer = (LPWSTR)malloc(length);
		LSTATUS status;
		do {
			status = RegQueryValueEx(subkey, valueName, NULL, type, (BYTE *)buffer, &length);
			if (status == ERROR_MORE_DATA) {
				length += 256 * sizeof(WCHAR);
				buffer = (LPWSTR)realloc(buffer, length);
			} else if (status != ERROR_SUCCESS) {
				hr = HRESULT_FROM_WIN32(status);
				goto end;
			}
		} while (status == ERROR_MORE_DATA);

		*data = buffer;
		*size = length / sizeof(WCHAR);
	}

end:
	if (subkey != NULL) {
		RegCloseKey(subkey);
	}
	if (!SUCCEEDED(hr)) {
		if (data != NULL) {
			*data = NULL;
		}
		if (size != NULL) {
			*size = 0;
		}
	}
	return hr;
}

HRESULT GetRegistryDword(HKEY key, LPCWSTR subkeyPath, LPCWSTR valueName, LPDWORD type, LPDWORD data) {
	HKEY subkey;
	HRESULT hr = HRESULT_FROM_WIN32(RegOpenKeyEx(key, subkeyPath, 0, KEY_READ, &subkey));
	if (!SUCCEEDED(hr)) {
		goto end;
	}

	if (data != NULL) {
		DWORD length = sizeof(DWORD);
		hr = HRESULT_FROM_WIN32(RegQueryValueEx(subkey, valueName, NULL, type, (LPBYTE)data, &length));
		if (!SUCCEEDED(hr)) {
			goto end;
		}
	}

end:
	if (subkey != NULL) {
		RegCloseKey(subkey);
	}
	return hr;
}

LPWSTR GetMessageForHresult(HRESULT hr) {
	_com_error *error = new _com_error(hr);
	CString message = error->ErrorMessage();
	BSTR outMessage = message.AllocSysString();
	return outMessage;
}

HRESULT GetOSProductName(VARIANT *pProductName) {
	if (_loadedProductName) {
		VariantCopy(pProductName, &_productName);
		return S_OK;
	}

	VariantInit(&_productName);
	_loadedProductName = true;

	CComPtr<IWbemLocator> locator;
	HRESULT hr = locator.CoCreateInstance(CLSID_WbemLocator);
	if (!SUCCEEDED(hr)) {
		return hr;
	}

	CComPtr<IWbemServices> services;
	hr = locator->ConnectServer(L"ROOT\\CIMV2", NULL, NULL, 0, NULL, 0, 0, &services);
	if (!SUCCEEDED(hr)) {
		return hr;
	}

	hr = CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
	if (!SUCCEEDED(hr)) {
		return hr;
	}

	CComPtr<IEnumWbemClassObject> enumerator;
	hr = services->ExecQuery(L"WQL", L"SELECT Caption FROM Win32_OperatingSystem", WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &enumerator);
	if (!SUCCEEDED(hr)) {
		return hr;
	}

	CComPtr<IWbemClassObject> object;
	ULONG uReturn = 0;
	hr = enumerator->Next(WBEM_INFINITE, 1, &object, &uReturn);
	if (!SUCCEEDED(hr)) {
		return hr;
	}

	hr = object->Get(L"Caption", 0, &_productName, NULL, NULL);
	if (!SUCCEEDED(hr)) {
		return hr;
	}

	VariantCopy(pProductName, &_productName);
	return S_OK;
}

HRESULT Reboot() {
	HRESULT hr = E_FAIL;

	// Make sure we have permission to shut down.
	HANDLE token;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
		LUID shutdownLuid;
		if (LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &shutdownLuid) != 0) {
			// Ask the system nicely to give us shutdown privilege.
			TOKEN_PRIVILEGES privileges;
			privileges.PrivilegeCount = 1;
			privileges.Privileges[0].Luid = shutdownLuid;
			privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
			if (!AdjustTokenPrivileges(token, FALSE, &privileges, 0, NULL, NULL)) {
				hr = HRESULT_FROM_WIN32(GetLastError());
				TRACE("AdjustTokenPrivileges() failed: %ls\n", GetMessageForHresult(hr));
			}
		}
	} else {
		hr = HRESULT_FROM_WIN32(GetLastError());
		TRACE("OpenProcessToken() failed: %ls\n", GetMessageForHresult(hr));
	}

	// Reboot with reason "Operating System: Security fix (Unplanned)"
	if (!InitiateSystemShutdownEx(NULL, NULL, 0, FALSE, TRUE, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_SECURITYFIX)) {
		hr = HRESULT_FROM_WIN32(GetLastError());
		TRACE("InitiateSystemShutdownExW() failed: %ls\n", GetMessageForHresult(hr));

		// Try ExitWindowsEx instead
		// Win2k: Use ExitWindowsEx, which is only guaranteed to work for the current logged in user
		if (!ExitWindowsEx(EWX_REBOOT | EWX_RESTARTAPPS | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_SECURITYFIX)) {
			hr = HRESULT_FROM_WIN32(GetLastError());
			TRACE("ExitWindowsEx() failed: %ls\n", GetMessageForHresult(hr));
		}
	} else {
		hr = S_OK;
	}

	return hr;
}
