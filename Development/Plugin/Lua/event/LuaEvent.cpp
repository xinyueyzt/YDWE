#include <cstdlib>
#include <cstring>
#include <string>
#include <algorithm>
#include <functional>
#include <regex>
#include <boost/algorithm/string.hpp>
#include <base/path/service.h>
#include <base/util/unicode.h>
#include <base/hook/inline.h>
#include <base/hook/iat.h>
#include <base/hook/fp_call.h>	  
#include <base/com/guard.h>

#include "YDWEEvent.h"
#include "YDWELogger.h"

#include <Mss.h>

namespace NYDWE {

	TEvent event_array[EVENT_MAXIMUM];

	HWND  gWeMainWindowHandle;
	HMENU gWeMainMenuHandle;

	bool isWeWinMainHookInstalled;
	uintptr_t pgTrueWeWinMain;
	int32_t WINAPI DetourWeWinMain(HINSTANCE instance, HINSTANCE prevInstance, LPSTR commandLine, int32_t showCommand)
	{
		LOGGING_INFO(lg) << "Entering main program.";
		LOGGING_DEBUG(lg) << "Command line: " << (commandLine ? commandLine : "NULL");

		// Initialize COM	   
		base::com::guard com;

		TEventData eventData;
		event_array[EVENT_WE_START](eventData);

		int32_t result = base::std_call<int32_t>(pgTrueWeWinMain, instance, prevInstance, commandLine, showCommand);

		eventData.clear();
		event_array[EVENT_WE_EXIT](eventData);

		LOGGING_INFO(lg) << "Main program exit.";

		// Finish engine here to prevent .net module crash
		// Before we stop the lua engine, we should stop hook
		// UninstallHooks();

		//ShutdownLua();

		return result;
	}

	volatile bool gIsInCompileProcess = false;
	uintptr_t pgTrueCreateFileA;
	HANDLE WINAPI DetourStormCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
	{
		std::wstring fileName = base::a2w(std::string_view(lpFileName), base::conv_method::replace | '?');
		if (gIsInCompileProcess && (
			boost::iends_with(fileName, L".w3x") ||
			boost::iends_with(fileName, L".w3m")/* ||
											   boost::iends_with(fileName, L".w3n") */ // Disabled because does not support campaign
											   ))
		{
			TEventData eventData;
			eventData["map_path"] = fileName;
			event_array[EVENT_SAVE_MAP](eventData);

			gIsInCompileProcess = false;
		}

		return base::std_call<HANDLE>(pgTrueCreateFileA, lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
	}

	HANDLE WINAPI DetourWeCreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
	{
		std::string fileName(lpFileName);
		if (boost::iends_with(fileName, "war3mapMap.blp"))
		{
			LOGGING_TRACE(lg) << "WE is about to compile maps.";
			gIsInCompileProcess = true;
		}
		else if (gIsInCompileProcess && (
			boost::iends_with(fileName, ".w3x") ||
			boost::iends_with(fileName, ".w3m")))
		{
			try {
				fs::path p(fileName);
				p = p.parent_path().remove_filename() / p.filename();

				TEventData eventData;
				eventData["map_path"] = p.wstring();
				event_array[EVENT_PRE_SAVE_MAP](eventData);
			}
			catch (...) {				
			}
		}

		return base::std_call<HANDLE>(pgTrueCreateFileA, lpFileName, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
	}

	/// Regex for extracting file path
	static const std::regex gRegexCommandLine(".*-loadfile\\s+([\\s\\S]+\\.[wW]3[xmXM])");

	uintptr_t pgTrueCreateProcessA;
	BOOL WINAPI DetourWeCreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo, LPPROCESS_INFORMATION lpProcessInformation)
	{
		std::cmatch matcher;
		if (lpCommandLine && std::regex_match(lpCommandLine, matcher, gRegexCommandLine))
		{
			fs::path currentWarcraftMap = base::path::get(base::path::DIR_EXE).remove_filename() / matcher.str(1);
			LOGGING_TRACE(lg) << L"Executing map " << currentWarcraftMap.wstring();

			TEventData eventData;
			if (gIsInCompileProcess)
			{
				LOGGING_TRACE(lg) << "Need to compile...";

				eventData["map_path"] = currentWarcraftMap.wstring();

				int results = event_array[EVENT_SAVE_MAP](eventData);
				gIsInCompileProcess = false;
				if (results < 0)
				{
					LOGGING_TRACE(lg) << "Save failed. Abort testing.";
					memset(lpProcessInformation, 0, sizeof(PROCESS_INFORMATION));
					return FALSE;
				}
			}
			else
			{
				LOGGING_TRACE(lg) << "No need to compile.";
			}

			eventData.clear();
			eventData["map_path"] = currentWarcraftMap.wstring();
			if (lpApplicationName)
				eventData["application_name"] = base::a2w(std::string_view(lpApplicationName), base::conv_method::replace | '?');
			if (lpCommandLine)
				eventData["command_line"] = base::a2w(std::string_view(lpCommandLine), base::conv_method::replace | '?');

			int results = event_array[EVENT_TEST_MAP](eventData);
			return results >= 0;
		}
		else
		{
			// Retain original
			return base::std_call<BOOL>(pgTrueCreateProcessA,
				lpApplicationName,
				lpCommandLine,
				lpProcessAttributes,
				lpThreadAttributes,
				bInheritHandles,
				dwCreationFlags,
				lpEnvironment,
				lpCurrentDirectory,
				lpStartupInfo,
				lpProcessInformation
				);
		}
	}


	uintptr_t pgTrueWeWindowProc;
	LRESULT CALLBACK DetourWeWindowProc(HWND windowHandle, UINT message, WPARAM wParam, LPARAM lParam)
	{
		TEventData eventData;
		eventData["handle"] = windowHandle;
		eventData["message"] = message;
		eventData["wparam"] = wParam;
		eventData["lparam"] = lParam;

		int results = event_array[EVENT_WINDOW_MESSAGE](eventData);
		if (results < 0)
		{
			return 0;
		}
		return base::std_call<LRESULT>(pgTrueWeWindowProc, windowHandle, message, wParam, lParam);
	}

	uintptr_t pgTrueCreateWindowExA;
	HWND WINAPI DetourWeCreateWindowExA(DWORD dwExStyle, LPCSTR lpClassName, LPCSTR lpWindowName, DWORD dwStyle, int X, int Y, int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam)
	{
		HWND result = base::std_call<HWND>(pgTrueCreateWindowExA,
			dwExStyle,
			lpClassName,
			lpWindowName,
			dwStyle,
			X,
			Y,
			nWidth,
			nHeight,
			hWndParent,
			hMenu,
			hInstance,
			lpParam
			);

		if (strcmp("Warcraft III", lpClassName) == 0)
		{
			gWeMainWindowHandle = result;
		}

		return result;
	}

	uintptr_t pgTrueSetMenu;
	BOOL WINAPI DetourWeSetMenu(HWND hWnd, HMENU hMenu)
	{
		if (hWnd == gWeMainWindowHandle)
		{
			// Main window menu
			gWeMainMenuHandle = hMenu;

			// Call menu initialization
			TEventData eventData;
			eventData["main_window_handle"] = hWnd;
			eventData["main_menu_handle"] = hMenu;
			event_array[EVENT_INIT_MENU](eventData);

			// Hook window
			pgTrueWeWindowProc = (decltype(pgTrueWeWindowProc))GetWindowLongPtrA(gWeMainWindowHandle, GWL_WNDPROC);
			SetWindowLongA(gWeMainWindowHandle, GWL_WNDPROC, reinterpret_cast<LONG>(DetourWeWindowProc));
		}

		return base::std_call<BOOL>(pgTrueSetMenu, hWnd, hMenu);
	}

	uintptr_t pgTrueWeDialogProc;
	INT_PTR CALLBACK DetourWeDialogProc(HWND dialogHandle, UINT message, WPARAM wParam, LPARAM lParam)
	{
		INT_PTR ret = base::std_call<INT_PTR>(pgTrueWeDialogProc, dialogHandle, message, wParam, lParam);

		if (message == WM_SETTEXT)
		{
			if (lParam)
			{
				TEventData eventData;
				eventData["handle"] = dialogHandle;
				eventData["message"] = message;
				eventData["wparam"] = wParam;
				eventData["lparam"] = base::a2u((const char*)lParam);
				int results = event_array[EVENT_DIALOG_MESSAGE](eventData);
				if (results < 0)
				{
					return 0;
				}
			}
		}
		else if (message == WM_COMMAND)
		{
			TEventData eventData;
			eventData["handle"] = dialogHandle;
			eventData["message"] = message;
			eventData["wparam"] = wParam;
			eventData["lparam"] = lParam;
			int results = event_array[EVENT_DIALOG_MESSAGE](eventData);
			if (results < 0)
			{
				return 0;
			}
		}

		return ret;
	}

	uintptr_t pgTrueCreateDialogIndirectParamA;
	HWND WINAPI DetourWeCreateDialogIndirectParamA(HINSTANCE hInstance, LPCDLGTEMPLATE lpTemplate, HWND hWndParent, DLGPROC lpDialogFunc, LPARAM lParamInit)
	{
		if (!pgTrueWeDialogProc)
		{
			pgTrueWeDialogProc = (uintptr_t)lpDialogFunc;
		}

		return base::std_call<HWND>(pgTrueCreateDialogIndirectParamA, hInstance, lpTemplate, hWndParent, DetourWeDialogProc, lParamInit);
	}

	uintptr_t pgTrueWeNewObjectId;
	bool isWeNewObjectIdHookInstalled;

	int32_t __fastcall PromptUserForNewId(uintptr_t this_, int32_t default_id)
	{
		uint32_t object_type = ((uint32_t(__fastcall*)(uintptr_t))(*(uintptr_t*)(*(uintptr_t*)this_ + 0x18)))(this_);

		TEventData eventData;
		eventData["class"] = this_;
		eventData["object_type"] = object_type;
		eventData["default_id"] = default_id;
		int results = event_array[EVENT_NEW_OBJECT_ID](eventData);
		if (results < 0)
		{
			return default_id;
		}
		return results;
	}

	static void __declspec(naked) DetourWeNewObjectId()
	{
		__asm
		{
			push	eax;
			push	ecx;
			mov		ecx, ebx
			call	PromptUserForNewId;
			mov		edx, eax
			pop		ecx;
			pop		eax;
			mov     [eax+4], edx;
			mov     [eax], ecx;
			mov     dword ptr [ebx+24h], 1;
			mov     eax, edx;
			pop     ebx;
			mov     esp, ebp;
			pop     ebp;
			retn    4;
		}
	}

	bool isMssRIBLoadProviderLibraryHookInstalled;
	uintptr_t pgTrueMssRIBLoadProviderLibrary;
	static HPROVIDER AILCALL DetourMssRIBLoadProviderLibrary(C8 const FAR *fileName)
	{
		TEventData eventData;
		eventData["library_name"] = std::string(fileName);
		int results = event_array[EVENT_MSS_LOAD](eventData);
		if (results < 0)
			return 0;
		return base::std_call<HPROVIDER>(pgTrueMssRIBLoadProviderLibrary, fileName);
	}

#define INSTALL_INLINE_HOOK(name) if (!is##name##HookInstalled) { if (pgTrue##name##) { is##name##HookInstalled = base::hook::inline_install(&pgTrue##name##, (uintptr_t)Detour##name##); }}

	void SetupEvent()
	{
		pgTrueCreateFileA     = base::hook::iat(L"storm.dll",             "kernel32.dll", "CreateFileA",     (uintptr_t)DetourStormCreateFileA);
		pgTrueCreateFileA     = base::hook::iat(::GetModuleHandleW(NULL), "kernel32.dll", "CreateFileA",     (uintptr_t)DetourWeCreateFileA);
		pgTrueCreateProcessA  = base::hook::iat(::GetModuleHandleW(NULL), "kernel32.dll", "CreateProcessA",  (uintptr_t)DetourWeCreateProcessA);
		pgTrueCreateWindowExA = base::hook::iat(::GetModuleHandleW(NULL), "user32.dll",   "CreateWindowExA", (uintptr_t)DetourWeCreateWindowExA);
		pgTrueSetMenu         = base::hook::iat(::GetModuleHandleW(NULL), "user32.dll",   "SetMenu",         (uintptr_t)DetourWeSetMenu);
		pgTrueCreateDialogIndirectParamA = base::hook::iat(::GetModuleHandleW(NULL), "user32.dll",   "CreateDialogIndirectParamA",  (uintptr_t)DetourWeCreateDialogIndirectParamA);

		pgTrueWeWinMain     = (uintptr_t)0x004021A0;
		INSTALL_INLINE_HOOK(WeWinMain);

		pgTrueWeNewObjectId = (uintptr_t)0x005B645A;
		INSTALL_INLINE_HOOK(WeNewObjectId)

		pgTrueMssRIBLoadProviderLibrary = (uintptr_t)GetProcAddress(GetModuleHandleA(MSSDLLNAME), "RIB_load_provider_library");
		INSTALL_INLINE_HOOK(MssRIBLoadProviderLibrary);
	}
}
