#!/usr/bin/env python3
"""Validate and render the reviewed title-runtime import capability policy."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
from runtime_profiles import profile



def module_paths(profile_name="p1w", gpu=False):
    return profile(profile_name, {"headless": True, "gpu": True} if gpu else None)["module_paths"]


def module_names(profile_name="p1w", gpu=False):
    return tuple(sorted(module_paths(profile_name, gpu)))


def policy_module_names(profile_name="p1w"):
    """One reviewed B1 policy covers every B1 build, so it lists the GPU modules too."""
    return module_names(profile_name, gpu=profile_name == "b1")

NATIVE_ROLE_IMPORTS = {
    "parent": {
        "sceSystemServiceAddLocalProcessForPsmKit": "libSceSystemService.prx",
        "sceSystemServiceKillLocalProcessForPsmKit": "libSceSystemService.prx",
    },
    "child": {},
}

B1_DESKTOP_NATIVE_IMPORTS = {
    "sceImeClose": "libSceIme.prx",
    "sceImeOpen": "libSceIme.prx",
    "sceImeParamInit": "libSceIme.prx",
    "sceImeUpdate": "libSceIme.prx",
    # libSceIme is loaded by path from the title's sandbox before first use.
    "sceKernelGetFsSandboxRandomWord": "libkernel.prx",
    "sceKernelLoadStartModule": "libkernel.prx",
    "sceKeyboardClose": "libSceKeyboard.prx",
    "sceKeyboardInit": "libSceKeyboard.prx",
    "sceKeyboardOpen": "libSceKeyboard.prx",
    "sceKeyboardRead": "libSceKeyboard.prx",
    "sceKeyboardReadState": "libSceKeyboard.prx",
    "sceKeyboardSetProcessPrivilege": "libSceKeyboard.prx",
    "sceMouseInit": "libSceMouse.prx",
    "sceMouseOpen": "libSceMouse.prx",
    "sceMouseRead": "libSceMouse.prx",
    "scePadInit": "libScePad.prx",
    "scePadOpen": "libScePad.prx",
    "scePadRead": "libScePad.prx",
    "sceSysmoduleLoadModule": "libSceSysmodule.prx",
    "sceSystemServiceHideSplashScreen": "libSceSystemService.prx",
    "sceUserServiceGetInitialUser": "libSceUserService.prx",
    "sceUserServiceGetLoginUserIdList": "libSceUserService.prx",
    "sceUserServiceInitialize": "libSceUserService.prx",
    "sceVideoOutIsFlipPending": "libSceVideoOut.prx",
    "sceVideoOutOpen": "libSceVideoOut.prx",
    "sceVideoOutRegisterBuffers2": "libSceVideoOut.prx",
    "sceVideoOutSetBufferAttribute2": "libSceVideoOut.prx",
    "sceVideoOutSetFlipRate": "libSceVideoOut.prx",
    "sceVideoOutSubmitFlip": "libSceVideoOut.prx",
    "sceVideoOutWaitVblank": "libSceVideoOut.prx",
}

B1_GPU_NATIVE_IMPORTS = {
    # GPU input reuses the product's device/inline-IME implementations only.
    **{name: provider for name, provider in B1_DESKTOP_NATIVE_IMPORTS.items()
       if not name.startswith("sceVideoOut") and name != "sceSystemServiceHideSplashScreen"},
    "sceAudioOutClose": "libSceAudioOut.prx",
    "sceAudioOutInit": "libSceAudioOut.prx",
    "sceAudioOutOpen": "libSceAudioOut.prx",
    "sceAudioOutOutput": "libSceAudioOut.prx",
    "scePadSetVibration": "libScePad.prx",
    "sceSystemServiceGetHdrToneMapLuminance": "libSceSystemService.prx",
    "_CurrentRuneLocale": "libSceLibcInternal.prx",
    "_DefaultRuneLocale": "libSceLibcInternal.prx",
    "__cxa_atexit": "libSceLibcInternal.prx",
    "__isfinite": "libSceLibcInternal.prx",
    "__isfinitef": "libSceLibcInternal.prx",
    "__isinff": "libSceLibcInternal.prx",
    "__isnormal": "libSceLibcInternal.prx",
    "__isnormalf": "libSceLibcInternal.prx",
    "__mb_sb_limit": "libSceLibcInternal.prx",
    "btowc": "libSceLibcInternal.prx",
    "cosf": "libSceLibcInternal.prx",
    "dlclose": "libkernel.prx",
    "dlerror": "libkernel.prx",
    "dlopen": "libkernel.prx",
    "dlsym": "libkernel.prx",
    "exp2": "libSceLibcInternal.prx",
    "exp2f": "libSceLibcInternal.prx",
    "flock": "libkernel.prx",
    "frexp": "libSceLibcInternal.prx",
    "frexpf": "libSceLibcInternal.prx",
    "ftell": "libSceLibcInternal.prx",
    "getegid": "libkernel.prx",
    "getgid": "libkernel.prx",
    "getprogname": "libSceLibcInternal.prx",
    "isalnum": "libSceLibcInternal.prx",
    "iswalnum": "libSceLibcInternal.prx",
    "iswalpha": "libSceLibcInternal.prx",
    "iswblank": "libSceLibcInternal.prx",
    "iswcntrl": "libSceLibcInternal.prx",
    "iswctype": "libSceLibcInternal.prx",
    "iswdigit": "libSceLibcInternal.prx",
    "iswgraph": "libSceLibcInternal.prx",
    "iswlower": "libSceLibcInternal.prx",
    "iswprint": "libSceLibcInternal.prx",
    "iswpunct": "libSceLibcInternal.prx",
    "iswspace": "libSceLibcInternal.prx",
    "iswupper": "libSceLibcInternal.prx",
    "iswxdigit": "libSceLibcInternal.prx",
    "ldexp": "libSceLibcInternal.prx",
    "ldexpf": "libSceLibcInternal.prx",
    "log10": "libSceLibcInternal.prx",
    "log2": "libSceLibcInternal.prx",
    "log2f": "libSceLibcInternal.prx",
    "lroundf": "libSceLibcInternal.prx",
    "mbrlen": "libSceLibcInternal.prx",
    "mbsrtowcs": "libSceLibcInternal.prx",
    "mbtowc": "libSceLibcInternal.prx",
    "powf": "libSceLibcInternal.prx",
    "printf": "libSceLibcInternal.prx",
    "pthread_barrier_destroy": "libkernel.prx",
    "pthread_barrier_init": "libkernel.prx",
    "pthread_barrier_wait": "libkernel.prx",
    "pthread_cond_timedwait": "libkernel.prx",
    "pthread_condattr_destroy": "libkernel.prx",
    "pthread_condattr_init": "libkernel.prx",
    "pthread_condattr_setclock": "libkernel.prx",
    "pthread_getcpuclockid": "libkernel.prx",
    "pthread_mutex_timedlock": "libkernel.prx",
    "pthread_mutex_trylock": "libkernel.prx",
    "pthread_set_name_np": "libkernel.prx",
    "putchar": "libSceLibcInternal.prx",
    "rand": "libSceLibcInternal.prx",
    "realpath": "libSceLibcInternal.prx",
    "rename": "libkernel.prx",
    "rewind": "libSceLibcInternal.prx",
    "sceAgcDriverSetHsOffchipParam": "libSceAgcDriver.prx",
    "sceAgcDriverSetTFRing": "libSceAgcDriver.prx",
    "sceAgcDriverSubmitDcb": "libSceAgcDriver.prx",
    "sceAgcInit": "libSceAgc.prx",
    "sceAgcSuspendPoint": "libSceAgc.prx",
    "sceVideoOutConfigureOutput": "libSceVideoOut.prx",
    "sceVideoOutCursorDisable": "libSceVideoOut.prx",
    "sceVideoOutCursorEnable": "libSceVideoOut.prx",
    "sceVideoOutCursorSetHotSpot": "libSceVideoOut.prx",
    "sceVideoOutCursorSetPosition": "libSceVideoOut.prx",
    "sceVideoOutConfigureOptionsInitialize_": "libSceVideoOut.prx",
    "sceVideoOutConfigureOutputModeEx_": "libSceVideoOut.prx",
    "sceVideoOutModeSetAny_": "libSceVideoOut.prx",
    "sceVideoOutGetCurrentOutputMode_": "libSceVideoOut.prx",
    "sceVideoOutGetOutputStatus": "libSceVideoOut.prx",
    "sceVideoOutGetFlipStatus": "libSceVideoOut.prx",
    "sceVideoOutIsOutputSupported": "libSceVideoOut.prx",
    "sceVideoOutOpen": "libSceVideoOut.prx",
    "sceVideoOutRegisterBuffers2": "libSceVideoOut.prx",
    "sceVideoOutSetBufferAttribute2": "libSceVideoOut.prx",
    "sceVideoOutSetFlipRate": "libSceVideoOut.prx",
    "sceVideoOutSubmitChangeBufferAttribute2": "libSceVideoOut.prx",
    "sceVideoOutSubmitFlip": "libSceVideoOut.prx",
    "sceVideoOutWaitVblank": "libSceVideoOut.prx",
    "sinf": "libSceLibcInternal.prx",
    "strcoll": "libSceLibcInternal.prx",
    "strerror_r": "libSceLibcInternal.prx",
    "strndup": "libSceLibcInternal.prx",
    "strspn": "libSceLibcInternal.prx",
    "strtod": "libSceLibcInternal.prx",
    "strtof": "libSceLibcInternal.prx",
    "strtold": "libSceLibcInternal.prx",
    "strtoll": "libSceLibcInternal.prx",
    "strxfrm": "libSceLibcInternal.prx",
    "swprintf": "libSceLibcInternal.prx",
    "syslog": "libSceLibcInternal.prx",
    "system": "libkernel.prx",
    "towlower": "libSceLibcInternal.prx",
    "towupper": "libSceLibcInternal.prx",
    "unlink": "libkernel.prx",
    "utime": "libSceLibcInternal.prx",
    "vsprintf": "libSceLibcInternal.prx",
    "vsscanf": "libSceLibcInternal.prx",
    "wcscoll": "libSceLibcInternal.prx",
    "wcstod": "libSceLibcInternal.prx",
    "wcstof": "libSceLibcInternal.prx",
    "wcstol": "libSceLibcInternal.prx",
    "wcstold": "libSceLibcInternal.prx",
    "wcstoll": "libSceLibcInternal.prx",
    "wcstoul": "libSceLibcInternal.prx",
    "wcstoull": "libSceLibcInternal.prx",
    "wcsxfrm": "libSceLibcInternal.prx",
    "wctob": "libSceLibcInternal.prx",
    "wctype": "libSceLibcInternal.prx",
    "wmemchr": "libSceLibcInternal.prx",
    "wmemcmp": "libSceLibcInternal.prx",
}


def native_role_imports(profile_name="p1w", *, headless=False, gpu=False):
    if gpu and (profile_name != "b1" or not headless):
        raise ValueError("GPU native imports require a headless B1 profile")
    roles = {role: dict(imports) for role, imports in NATIVE_ROLE_IMPORTS.items()}
    if profile_name == "b1":
        networking = {
            "sceNetCtlInit": "libSceNetCtl.prx",
            "sceNetCtlGetInfo": "libSceNetCtl.prx",
            "sceSystemServiceParamGetString": "libSceSystemService.prx",
        }
        networking.update({name: "libSceNet.prx" for name in (
            "sceNetErrnoLoc", "sceNetPoolCreate", "sceNetPoolDestroy",
            "sceNetResolverCreate", "sceNetResolverDestroy",
            "sceNetResolverStartAton", "sceNetResolverStartNtoa",
        )})
        # Only the parent imports them: a PsmKit child does not load these
        # modules, so its wrappers forward the calls (B1_NATIVE_FORWARDED).
        roles["parent"].update(networking)
        roles["parent"]["sceSystemServiceLoadExec"] = "libSceSystemService.prx"
        # B1 parents use public socket transport for payload launch and the
        # bounded write-broker elfldr autostart. Common providers are reviewed
        # by symbol, not as whole-provider role scopes.
        roles["parent"].update({
            "connect": "libkernel.prx", "setsockopt": "libkernel.prx",
            "shutdown": "libkernel.prx",
        })
        roles["parent"].update({
            "sceSystemServiceLaunchWebBrowser": "libSceSystemService.prx",
            "sceUserServiceInitialize": "libSceUserService.prx",
        })
        roles["parent"].update({
            "sceHttp2Init": "libSceHttp2.prx",
            "sceHttp2Term": "libSceHttp2.prx",
            "sceHttp2CreateTemplate": "libSceHttp2.prx",
            "sceHttp2DeleteTemplate": "libSceHttp2.prx",
            "sceHttp2CreateRequestWithURL": "libSceHttp2.prx",
            "sceHttp2DeleteRequest": "libSceHttp2.prx",
            "sceHttp2SendRequest": "libSceHttp2.prx",
            "sceHttp2GetStatusCode": "libSceHttp2.prx",
            "sceHttp2GetResponseContentLength": "libSceHttp2.prx",
            "sceHttp2ReadData": "libSceHttp2.prx",
            "sceSslInit": "libSceSsl.prx",
            "sceSslTerm": "libSceSsl.prx",
            "sceNetInit": "libSceNet.prx",
        })
        if gpu:
            # Game mode never downloads or launches the Battle.net installer.
            roles["parent"] = {name: provider for name, provider in roles["parent"].items()
                               if provider not in {"libSceHttp2.prx", "libSceSsl.prx"} and
                               name != "sceNetInit"}
            roles["parent"].update(B1_GPU_NATIVE_IMPORTS)
        if not headless:
            roles["parent"].update(B1_DESKTOP_NATIVE_IMPORTS)
    return roles
DL_OVERRIDES = {
    "dlopen": "pw_public_dlopen", "dlsym": "pw_public_dlsym",
    "dlclose": "pw_public_dlclose", "dlerror": "pw_public_dlerror",
    "dladdr": "pw_public_dladdr",
    "__dlopen": "pw_public_dlopen", "__dlsym": "pw_public_dlsym",
    "__dlclose": "pw_public_dlclose", "__dlerror": "pw_public_dlerror",
    "__dladdr": "pw_public_dladdr",
}
FAIL_CLOSED = {
    "syscall": "pw_native_fail_syscall",
    "__syscall": "pw_native_fail_syscall",
    "fork": "pw_native_fail_fork",
    "setsid": "pw_native_fail_setsid",
}
FAIL_MINUS_ONE = {
    "extattr_delete_fd", "extattr_get_fd", "extattr_get_file", "extattr_set_fd",
    "execv", "fchdir", "thr_kill2", "thr_set_name",
    # libkernel exports no faccessat; GnuTLS's gnulib realpath treats -1 as
    # "not accessible" (a title refuses access() as well).
    "faccessat",
}
FAIL_ZERO = {
    "if_nametoindex": "pw_native_fail_if_nametoindex",
    "getmntinfo": "pw_native_fail_getmntinfo",
}
FAIL_POSIX_ERROR = {"posix_fadvise"}
PLATFORM_OVERRIDES = {
    "futimens": "ps5_futimens",
    "posix_fallocate": "ps5_posix_fallocate",
    "realpath": "ps5_realpath",
    "umask": "ps5_umask",
    "isatty": "ps5_isatty",
    "link": "ps5_link",
    "openat": "ps5_openat",
    "fstatat": "ps5_fstatat",
    "fstatfs": "ps5_fstatfs",
    "readlink": "ps5_readlink",
    "symlink": "ps5_symlink",
}
SPECIAL = {
    "__tls_get_addr": ("custom_tls", "pw_native_tls_get_addr"),
    "dup": ("custom_fd", "pw_native_dup_fd"),
}
# Exact imports supplied by admitted ntdll/win32u modules, never by the FSELF.
TITLE_NTDLL_EXPORTS = {
    'KeAddSystemServiceTable', 'KeUserModeCallback', 'NtAddAtom',
    'NtAllocateLocallyUniqueId', 'NtAllocateVirtualMemory', 'NtClose',
    'NtCreateKey', 'NtCreateMutant', 'NtCreateUserProcess', 'NtCurrentTeb',
    'NtDelayExecution', 'NtDeleteKey', 'NtDeleteValueKey', 'NtDeviceIoControlFile',
    'NtDuplicateObject', 'NtEnumerateKey', 'NtEnumerateValueKey',
    'NtFreeVirtualMemory', 'NtGetContextThread', 'NtGetNlsSectionPtr',
    'NtGetTickCount', 'NtInitializeNlsFiles', 'NtMapViewOfSection',
    'NtOpenDirectoryObject', 'NtOpenFile', 'NtOpenKeyEx', 'NtOpenSection',
    'NtQueryDefaultLocale', 'NtQueryDirectoryFile', 'NtQueryFullAttributesFile',
    'NtQueryInformationAtom', 'NtQueryInformationToken', 'NtQueryKey',
    'NtQueryObject', 'NtQueryPerformanceCounter', 'NtQuerySystemInformation',
    'NtQuerySystemTime', 'NtQueryValueKey', 'NtRaiseException', 'NtReleaseMutant',
    'NtResumeThread', 'NtSetEvent', 'NtSetValueKey', 'NtTerminateProcess',
    'NtUnmapViewOfSection', 'NtWaitForMultipleObjects', 'NtWaitForSingleObject',
    'NtYieldExecution', 'RtlCustomCPToUnicodeN', 'RtlDowncaseUnicodeChar',
    'RtlGetLastWin32Error', 'RtlInitCodePageTable', 'RtlInitUnicodeString',
    'RtlNtStatusToDosError', 'RtlSetLastWin32Error', 'RtlUTF8ToUnicodeN',
    'RtlUnicodeToCustomCPN', 'RtlUnicodeToUTF8N', 'RtlUpcaseUnicodeChar',
    '__wine_dbg_get_channel_flags', '__wine_dbg_header', '__wine_dbg_output',
    '__wine_dbg_strdup', '__wine_ps5_audio_available', '__wine_ps5_audio_close',
    '__wine_ps5_audio_open', '__wine_ps5_audio_output', '__wine_ps5_get_hdr_luminance',
    '__wine_ps5_get_specific', '__wine_ps5_get_vulkan_proc', '__wine_ps5_is_title',
    '__wine_ps5_key_create', '__wine_ps5_key_delete', '__wine_ps5_set_specific',
    'ntdll_add_syscall_debug_info',
    'ntdll_get_build_dir', 'ntdll_get_data_dir', 'ntdll_get_dos_file_name',
    'ntdll_get_unix_file_name', 'ntdll_set_exception_jmp_buf', 'ntdll_umbstowcs',
    'ntdll_wcsicmp', 'ntdll_wcsnicmp', 'ntdll_wcstoumbs', 'wine_server_call',
    'wine_server_handle_to_fd', 'wine_server_send_fd', 'wine_server_fd_to_handle',
    '__wine_ps5_desktop_available', '__wine_ps5_desktop_connect',
    '__wine_ps5_desktop_surface',
    'ps5_title_profile_set_blocking',  # wineps5audio.so and GPU winevulkan.so waits
}
B1_WIN32U_EXPORTS = {
    "NtGdiCreateCompatibleDC", "NtGdiDdDDICreateDCFromMemory", "NtGdiDeleteObjectApp",
    "NtGdiGetDCDword", "NtUserCallHwnd", "NtUserCallHwndParam", "NtUserCallNoParam",
    "NtUserCallOneParam", "NtUserGetAncestor", "NtUserGetDCEx",
    "NtUserGetObjectInformation", "NtUserGetProp", "NtUserGetThreadDesktop",
    "NtUserMessageCall", "NtUserReleaseDC", "NtUserSetProp",
    "__wine_get_opengl_driver", "__wine_get_vulkan_driver", "__wine_set_user_driver",
    "window_surface_add_ref", "window_surface_create", "window_surface_release",
}
B1_NATIVE_RESOLVERS = {
    name: "pw_b1_" + name.lstrip("_") for name in (
        "__h_errno", "getaddrinfo", "freeaddrinfo",
        "gethostbyaddr", "gethostbyname", "getnameinfo",
    )
}
# Calls into modules a PsmKit child does not load: wrappers in native-entry.c
# run them in the parent (title) or forward them to it (child). Applied to the
# Unix modules' imports and, by renaming, to netdb.o's calls.
B1_NATIVE_FORWARDED = {
    "sceNetPoolCreate": "pw_net_pool_create",
    "sceNetPoolDestroy": "pw_net_pool_destroy",
    "sceNetResolverCreate": "pw_net_resolver_create",
    "sceNetResolverDestroy": "pw_net_resolver_destroy",
    "sceNetResolverStartNtoa": "pw_net_resolver_ntoa",
    "sceNetResolverStartAton": "pw_net_resolver_aton",
    "sceNetErrnoLoc": "pw_net_errno_loc",
    "sceNetCtlInit": "pw_net_ctl_init",
    "sceNetCtlGetInfo": "pw_net_ctl_get_info",
    "sceSystemServiceParamGetString": "pw_ss_param_get_string",
}
# B1 klog lines are queued for the native writer thread (title-log.c): a
# console klog write costs its calling (game) thread milliseconds per line.
B1_PLATFORM_OVERRIDES = {"sceKernelDebugOutText": "pw_title_klog_text"}
OWNERS = {
    "custom_fd", "custom_loader", "custom_tls", "fail_closed",
    "fself_public_or_platform", "native_pthread", "platform",
    "title_ntdll", "title_win32u",
}
UNREVIEWED_OWNER = "UNREVIEWED_NOT_AUTHORIZED"
PRIVATE = re.compile(r"^(?:kernel_|mdbg_|__kernel_init$|__patch_init$|__rtld_init$|__rtld_so_)")
IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
POLICY_FIELDS = {"module", "binding", "symbol", "target", "owner"}


def candidate_ownership(name: str, *, profile_name="p1w") -> tuple[str, str]:
    """Classify explicit mappings; ordinary candidates remain unauthorized.

    Ownership is per symbol: the native import table binds each name once for
    every module. wineps5.so's payload transport imports (socket, connect,
    shm_open, ...) therefore share the other modules' owners; its title
    desktop branch never calls them."""
    if profile_name == "b1":
        if name in B1_WIN32U_EXPORTS:
            return "title_win32u", name
        if name in B1_NATIVE_RESOLVERS:
            return "platform", B1_NATIVE_RESOLVERS[name]
        if name in B1_NATIVE_FORWARDED:
            return "platform", B1_NATIVE_FORWARDED[name]
        if name in B1_PLATFORM_OVERRIDES:
            return "platform", B1_PLATFORM_OVERRIDES[name]
    if name in TITLE_NTDLL_EXPORTS:
        return "title_ntdll", name
    if name in DL_OVERRIDES:
        return "custom_loader", DL_OVERRIDES[name]
    if name in FAIL_CLOSED:
        return "fail_closed", FAIL_CLOSED[name]
    if name in FAIL_MINUS_ONE:
        return "fail_closed", "pw_native_fail_minus_one"
    if name in FAIL_POSIX_ERROR:
        return "fail_closed", "pw_native_fail_posix_error"
    if name in FAIL_ZERO:
        return "fail_closed", FAIL_ZERO[name]
    if name in PLATFORM_OVERRIDES:
        return "platform", PLATFORM_OVERRIDES[name]
    if name in SPECIAL:
        return SPECIAL[name]
    if name.startswith("__real_pthread_"):
        native = name.removeprefix("__real_")
        # build.sh wraps these three names itself; __real_NAME is the GNU ld
        # spelling that reaches the original symbol rather than its wrapper.
        target = name if native in {"pthread_create", "pthread_join", "pthread_detach"} else native
        return "native_pthread", target
    return UNREVIEWED_OWNER, name


def _validate_symbol(name: str) -> None:
    if not isinstance(name, str) or not name or "\x00" in name or not IDENTIFIER.fullmatch(name):
        raise ValueError(f"unsupported import spelling: {name!r}")
    if PRIVATE.search(name):
        raise ValueError(f"private import forbidden: {name}")


def discovered_rows(module_symbols: dict[str, dict[str, set[str]]], *, profile_name="p1w",
                    gpu=False) -> list[dict]:
    """Return the exact discovered module/binding/symbol surface."""
    expected = module_names(profile_name, gpu)
    if tuple(sorted(module_symbols)) != expected:
        raise ValueError(
            f"dedicated module set drift: expected {list(expected)}, "
            f"found {sorted(module_symbols)}")
    rows = []
    for module in expected:
        bindings = module_symbols[module]
        if set(bindings) != {"strong", "weak"}:
            raise ValueError(f"{module}: expected exact strong/weak symbol sets")
        overlap = bindings["strong"] & bindings["weak"]
        if overlap:
            raise ValueError(f"{module}: symbols have both strong and weak bindings: {sorted(overlap)}")
        for binding in ("strong", "weak"):
            for symbol in sorted(bindings[binding]):
                _validate_symbol(symbol)
                owner, target = candidate_ownership(symbol, profile_name=profile_name)
                rows.append({
                    "module": module,
                    "binding": binding,
                    "symbol": symbol,
                    "target": target,
                    "owner": owner,
                })
    return sorted(rows, key=lambda item: (item["module"], item["symbol"]))


def _reject_duplicate_json_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key in import policy: {key}")
        result[key] = value
    return result


def _validate_policy_rows(rows, *, profile_name="p1w") -> list[dict]:
    if not isinstance(rows, list):
        raise ValueError("import policy imports must be a list")
    validated = []
    seen = set()
    symbol_ownership = {}
    for index, row in enumerate(rows):
        if not isinstance(row, dict) or set(row) != POLICY_FIELDS:
            raise ValueError(f"import policy record {index} has the wrong fields")
        module = row["module"]
        binding = row["binding"]
        symbol = row["symbol"]
        target = row["target"]
        owner = row["owner"]
        if module not in policy_module_names(profile_name) or binding not in {"strong", "weak"}:
            raise ValueError(f"import policy record {index} has an invalid module/binding")
        _validate_symbol(symbol)
        _validate_symbol(target)
        if owner not in OWNERS:
            raise ValueError(f"import policy record {index} has an invalid owner")
        mapped_owner, mapped_target = candidate_ownership(symbol, profile_name=profile_name)
        if mapped_owner == UNREVIEWED_OWNER:
            expected_owner, expected_target = "fself_public_or_platform", symbol
        else:
            expected_owner, expected_target = mapped_owner, mapped_target
        if (owner, target) != (expected_owner, expected_target):
            raise ValueError(
                f"import policy ownership drift for {symbol}: "
                f"policy={(owner, target)}, mapping={(expected_owner, expected_target)}")
        key = (module, symbol)
        if key in seen:
            raise ValueError(f"duplicate import policy record: {module}:{symbol}")
        seen.add(key)
        previous = symbol_ownership.setdefault(symbol, (owner, target))
        if previous != (owner, target):
            raise ValueError(f"inconsistent cross-module ownership for {symbol}")
        validated.append(dict(row))
    canonical = sorted(validated, key=lambda item: (item["module"], item["symbol"]))
    if validated != canonical:
        raise ValueError("import policy records are not in canonical module/symbol order")
    return validated


def load_policy(path: Path, *, profile_name="p1w") -> list[dict]:
    """Load the checked-in policy and reject non-canonical or implicit entries."""
    try:
        document = json.loads(path.read_text(), object_pairs_hook=_reject_duplicate_json_keys)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        raise ValueError(f"cannot load reviewed import policy {path}: {error}") from error
    fields = {"format", "modules", "imports", "native_roles"}
    if profile_name == "b1":
        fields.add("native_gpu_parent")
    if not isinstance(document, dict) or set(document) != fields:
        raise ValueError("import policy has the wrong fields")
    if document["format"] != 2 or document["modules"] != list(policy_module_names(profile_name)):
        raise ValueError("unsupported import policy format or module set")
    if document["native_roles"] != native_role_imports(profile_name):
        raise ValueError("native supervisor role/provider ownership drift")
    if profile_name == "b1" and document["native_gpu_parent"] != B1_GPU_NATIVE_IMPORTS:
        raise ValueError("GPU parent role/provider ownership drift")
    return _validate_policy_rows(document["imports"], profile_name=profile_name)


def make_candidate_manifest(module_symbols: dict[str, dict[str, set[str]]], *, profile_name="p1w",
                            gpu=False) -> dict:
    """Create review data only. Its records are never consumed by the linker."""
    return {
        "format": 1,
        "generator": "runtime_imports.py",
        "authorization": "CANDIDATE_ONLY_NOT_AUTHORIZED",
        "modules": list(module_names(profile_name, gpu)),
        "imports": discovered_rows(module_symbols, profile_name=profile_name, gpu=gpu),
    }


def write_candidate(module_symbols: dict[str, dict[str, set[str]]], path: Path, *, profile_name="p1w",
                    gpu=False) -> dict:
    manifest = make_candidate_manifest(module_symbols, profile_name=profile_name, gpu=gpu)
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def _merge_rows(rows: list[dict]) -> list[dict]:
    merged = {}
    for row in rows:
        symbol = row["symbol"]
        record = merged.setdefault(symbol, {
            "name": symbol,
            "binding": row["binding"],
            "modules": [],
            "module_bindings": {},
            "owner": row["owner"],
            "target": row["target"],
        })
        if (record["owner"], record["target"]) != (row["owner"], row["target"]):
            raise ValueError(f"inconsistent ownership for {symbol}")
        record["module_bindings"][row["module"]] = row["binding"]
        if row["binding"] == "strong":
            record["binding"] = "strong"
    records = []
    for symbol in sorted(merged):
        record = merged[symbol]
        record["module_bindings"] = dict(sorted(record["module_bindings"].items()))
        record["modules"] = list(record["module_bindings"])
        records.append(record)
    return records


def make_records(
        module_symbols: dict[str, dict[str, set[str]]], policy_rows: list[dict], *, profile_name="p1w",
        gpu=False) -> list[dict]:
    """Authorize only an exact match to the reviewed policy, then merge it."""
    policy_rows = _validate_policy_rows(policy_rows, profile_name=profile_name)
    # Rows of modules this build does not ship (GPU modules in a launcher build) are not authority.
    policy_rows = [row for row in policy_rows if row["module"] in module_names(profile_name, gpu)]
    actual = discovered_rows(module_symbols, profile_name=profile_name, gpu=gpu)
    actual_surface = {
        (row["module"], row["binding"], row["symbol"]) for row in actual
    }
    policy_surface = {
        (row["module"], row["binding"], row["symbol"]) for row in policy_rows
    }
    if actual_surface != policy_surface:
        added = sorted(actual_surface - policy_surface)
        removed = sorted(policy_surface - actual_surface)
        raise ValueError(
            "dedicated import policy mismatch before native link: "
            f"added={added}, removed={removed}")
    # load_policy already made target and owner drift a hard error. Use policy
    # rows, never candidate classifications, as the linker's sole authority.
    return _merge_rows(policy_rows)


def module_manifest(module_files: dict[str, Path], *, profile_name="p1w", gpu=False) -> list[dict]:
    if tuple(sorted(module_files)) != module_names(profile_name, gpu):
        raise ValueError(f"module manifest differs from the {profile_name} allow-list")
    paths = module_paths(profile_name, gpu)
    result = []
    for module in module_names(profile_name, gpu):
        path = module_files[module]
        data = path.read_bytes()
        result.append({
            "name": module,
            "path": paths[module],
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
        })
    return result


def render_include(records: list[dict], modules: list[dict], *, profile_name="p1w", gpu=False) -> str:
    """Render the reviewed import table and immutable profile module identities."""
    if [item.get("name") for item in modules] != list(module_names(profile_name, gpu)):
        raise ValueError("generated title module manifest is incomplete or out of order")
    lines = [
        f"/* Generated only after exact match to runtime-import-policy{'-b1' if profile_name == 'b1' else ''}.json. */",
        "/* Do not edit or commit: build-runtime.py recreates this file. */",
        "static const struct pw_title_module_manifest title_modules[] = {",
    ]
    for item in modules:
        if item.get("path") != module_paths(profile_name, gpu)[item["name"]]:
            raise ValueError(f"non-canonical title module path for {item['name']}")
        digest = bytes.fromhex(item["sha256"])
        if len(digest) != 32 or not isinstance(item.get("size"), int) or item["size"] < 0:
            raise ValueError(f"invalid title module identity for {item['name']}")
        encoded = ", ".join(f"0x{byte:02x}" for byte in digest)
        lines.append(
            f'    {{.path = {json.dumps(item["path"])}, .sha256 = {{{encoded}}}, '
            f'.size = UINT64_C({item["size"]})}},')
    lines.append("};")
    aliases = []
    for index, record in enumerate(records):
        if record["owner"] in {"title_ntdll", "title_win32u"}:
            continue
        target = record["target"]
        alias = f"pw_title_import_{index}"
        aliases.append(alias)
        lines.append(f'extern void {alias}(void) __asm__({json.dumps(target)});')
    lines.append("static const struct pw_elf_import native_imports[] = {")
    native_records = [record for record in records if record["owner"] not in {"title_ntdll", "title_win32u"}]
    for record, alias in zip(native_records, aliases):
        lines.append(f'    {{{json.dumps(record["name"])}, (uintptr_t)&{alias}}},')
    lines.append("};")
    return "\n".join(lines) + "\n"


def write_outputs(
        records: list[dict], modules: list[dict], include_path: Path,
        manifest_path: Path, policy_sha256: str, *, profile_name="p1w", gpu=False) -> dict:
    text = render_include(records, modules, profile_name=profile_name, gpu=gpu)
    include_path.write_text(text)
    manifest = {
        "format": 2,
        "generator": "runtime_imports.py",
        "policy_sha256": policy_sha256,
        "include_sha256": hashlib.sha256(text.encode()).hexdigest(),
        "modules": modules,
        "imports": records,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest
