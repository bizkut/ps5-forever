/* Exercise the real WinVerifyTrust -> CryptQueryObject -> signer metadata path.
 * No trust overrides and no execution of the supplied PE.  Arguments:
 *   test-signature-file.exe REPORT FILE EXPECTED_ORGANIZATION [accept|reject|inspect]
 * inspect records observations without imposing a trust result (diagnosis only).
 * accept/reject assert the combined trust + organization result, not just parsing.
 */
#include <windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mssip.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <wchar.h>

static FILE *report;
static void LOG(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
    fflush(stdout);
    if (!report) return;
    va_start(args, format);
    vfprintf(report, format, args);
    va_end(args);
    fflush(report);
}

static void registry(void)
{
    static const struct
    {
        const char *label;
        const WCHAR *path, *dll, *function;
    } keys[] = {
        {"trust_init", L"Software\\Microsoft\\Cryptography\\Providers\\Trust\\Initialization\\{00AAC56B-CD44-11D0-8CC2-00C04FC295EE}", L"$DLL", L"$Function"},
        {"trust_object", L"Software\\Microsoft\\Cryptography\\Providers\\Trust\\Message\\{00AAC56B-CD44-11D0-8CC2-00C04FC295EE}", L"$DLL", L"$Function"},
        {"trust_signature", L"Software\\Microsoft\\Cryptography\\Providers\\Trust\\Signature\\{00AAC56B-CD44-11D0-8CC2-00C04FC295EE}", L"$DLL", L"$Function"},
        {"trust_certificate", L"Software\\Microsoft\\Cryptography\\Providers\\Trust\\Certificate\\{00AAC56B-CD44-11D0-8CC2-00C04FC295EE}", L"$DLL", L"$Function"},
        {"trust_policy", L"Software\\Microsoft\\Cryptography\\Providers\\Trust\\FinalPolicy\\{00AAC56B-CD44-11D0-8CC2-00C04FC295EE}", L"$DLL", L"$Function"},
        {"sip_get", L"Software\\Microsoft\\Cryptography\\OID\\EncodingType 0\\CryptSIPDllGetSignedDataMsg\\{C689AAB8-8E78-11D0-8C47-00C04FC295EE}", L"Dll", L"FuncName"},
        {"sip_put", L"Software\\Microsoft\\Cryptography\\OID\\EncodingType 0\\CryptSIPDllPutSignedDataMsg\\{C689AAB8-8E78-11D0-8C47-00C04FC295EE}", L"Dll", L"FuncName"},
        {"sip_create", L"Software\\Microsoft\\Cryptography\\OID\\EncodingType 0\\CryptSIPDllCreateIndirectData\\{C689AAB8-8E78-11D0-8C47-00C04FC295EE}", L"Dll", L"FuncName"},
        {"sip_verify", L"Software\\Microsoft\\Cryptography\\OID\\EncodingType 0\\CryptSIPDllVerifyIndirectData\\{C689AAB8-8E78-11D0-8C47-00C04FC295EE}", L"Dll", L"FuncName"},
        {"sip_remove", L"Software\\Microsoft\\Cryptography\\OID\\EncodingType 0\\CryptSIPDllRemoveSignedDataMsg\\{C689AAB8-8E78-11D0-8C47-00C04FC295EE}", L"Dll", L"FuncName"},
    };
    static const REGSAM views[] = {KEY_WOW64_32KEY, KEY_WOW64_64KEY};
    unsigned int view, i;

    for (view = 0; view < sizeof(views) / sizeof(views[0]); ++view)
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        {
            HKEY key;
            LONG error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keys[i].path, 0, KEY_READ | views[view], &key);
            LOG("registry view=%u key=%s open=%08lx", view ? 64 : 32, keys[i].label, (DWORD)error);
            if (!error)
            {
                DWORD size = 0, type = 0;
                error = RegQueryValueExW(key, keys[i].dll, NULL, &type, NULL, &size);
                LOG(" dll=%08lx dll_type=%lu dll_bytes=%lu", (DWORD)error, type, size);
                size = type = 0;
                error = RegQueryValueExW(key, keys[i].function, NULL, &type, NULL, &size);
                LOG(" function=%08lx function_type=%lu function_bytes=%lu", (DWORD)error, type, size);
                RegCloseKey(key);
            }
            LOG("\n");
        }
}

static void sip(const WCHAR *path)
{
    GUID subject;
    SIP_DISPATCH_INFO dispatch = {0};
    BOOL ret;
    DWORD error;

    SetLastError(0);
    ret = CryptSIPRetrieveSubjectGuid(path, NULL, &subject);
    error = GetLastError();
    LOG("sip_subject result=%u error=%08lx\n", ret, error);
    if (!ret) return;
    dispatch.cbSize = sizeof(dispatch);
    SetLastError(0);
    ret = CryptSIPLoad(&subject, 0, &dispatch);
    error = GetLastError();
    LOG("sip_load result=%u error=%08lx get=%u put=%u create=%u verify=%u remove=%u\n",
        ret, error, !!dispatch.pfGet, !!dispatch.pfPut, !!dispatch.pfCreate,
        !!dispatch.pfVerify, !!dispatch.pfRemove);
}

static void metadata(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE file;
    BOOL ret;
    DWORD error;

    SetLastError(0);
    ret = GetFileAttributesExW(path, GetFileExInfoStandard, &attrs);
    error = GetLastError();
    LOG("attributes result=%u error=%08lx\n", ret, error);
    SetLastError(0);
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    error = GetLastError();
    LOG("open result=%u error=%08lx\n", file != INVALID_HANDLE_VALUE, error);
    if (file == INVALID_HANDLE_VALUE) return;
    SetLastError(0);
    ret = GetFileInformationByHandle(file, &info);
    error = GetLastError();
    LOG("information result=%u error=%08lx\n", ret, error);
    if (ret)
        LOG("file size=%08lx%08lx attributes=%08lx links=%lu index=%08lx%08lx creation=%08lx%08lx\n",
            info.nFileSizeHigh, info.nFileSizeLow, info.dwFileAttributes, info.nNumberOfLinks,
            info.nFileIndexHigh, info.nFileIndexLow, info.ftCreationTime.dwHighDateTime,
            info.ftCreationTime.dwLowDateTime);
    CloseHandle(file);
}

static LONG trust(const WCHAR *path, BOOL wrong_action)
{
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_FILE_INFO file = {0};
    WINTRUST_DATA data = {0};
    CRYPT_PROVIDER_DATA *provider;
    LONG status;
    DWORD error, i;

    if (wrong_action) action.Data1 ^= 1;
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path;
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    SetLastError(0);
    status = WinVerifyTrust(NULL, &action, &data);
    error = GetLastError();
    LOG("trust wrong_action=%u status=%08lx error=%08lx\n", wrong_action, (DWORD)status, error);
    provider = WTHelperProvDataFromStateData(data.hWVTStateData);
    if (provider)
    {
        for (i = 0; i < provider->cdwTrustStepErrors; ++i)
            if (provider->padwTrustStepErrors[i])
                LOG("trust_step index=%lu error=%08lx\n", i, provider->padwTrustStepErrors[i]);
        LOG("trust_signers count=%lu\n", provider->csSigners);
        for (i = 0; i < provider->csSigners; ++i)
            LOG("trust_signer index=%lu certificates=%lu error=%08lx time=%08lx%08lx\n", i,
                provider->pasSigners[i].csCertChain, provider->pasSigners[i].dwError,
                provider->pasSigners[i].sftVerifyAsOf.dwHighDateTime,
                provider->pasSigners[i].sftVerifyAsOf.dwLowDateTime);
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &action, &data);
    return status;
}

static BOOL signer(const WCHAR *path, const WCHAR *expected)
{
    HCERTSTORE store = NULL;
    HCRYPTMSG message = NULL;
    CMSG_SIGNER_INFO *info = NULL;
    CERT_INFO subject = {0};
    PCCERT_CONTEXT cert = NULL;
    WCHAR *name = NULL;
    DWORD size = 0, count, error;
    BOOL ret, match = FALSE;

    /* Match the bootstrapper's optional NULL output arguments exactly. */
    SetLastError(0);
    ret = CryptQueryObject(CERT_QUERY_OBJECT_FILE, path,
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, NULL, NULL, NULL,
                          &store, &message, NULL);
    error = GetLastError();
    LOG("query result=%u error=%08lx store=%u message=%u\n", ret, error, !!store, !!message);
    if (!ret) goto done;
    SetLastError(0);
    ret = CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, NULL, &size);
    error = GetLastError();
    LOG("signer_size result=%u error=%08lx size=%lu\n", ret, error, size);
    if (!ret || !(info = calloc(1, size))) goto done;
    SetLastError(0);
    ret = CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, info, &size);
    error = GetLastError();
    LOG("signer_info result=%u error=%08lx\n", ret, error);
    if (!ret) goto done;
    subject.Issuer = info->Issuer;
    subject.SerialNumber = info->SerialNumber;
    SetLastError(0);
    cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                     0, CERT_FIND_SUBJECT_CERT, &subject, NULL);
    error = GetLastError();
    LOG("signer_certificate result=%u error=%08lx\n", !!cert, error);
    if (!cert) goto done;
    SetLastError(0);
    count = CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0, (void *)szOID_ORGANIZATION_NAME, NULL, 0);
    error = GetLastError();
    LOG("organization_size count=%lu error=%08lx\n", count, error);
    if (!count || !(name = calloc(count, sizeof(*name)))) goto done;
    SetLastError(0);
    count = CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0, (void *)szOID_ORGANIZATION_NAME, name, count);
    error = GetLastError();
    match = count && !wcscmp(name, expected);
    LOG("organization count=%lu error=%08lx expected_match=%u\n", count, error, match);

done:
    free(name);
    free(info);
    if (cert) CertFreeCertificateContext(cert);
    if (message) CryptMsgClose(message);
    if (store) CertCloseStore(store, 0);
    return match;
}

int wmain(int argc, WCHAR **argv)
{
    LONG status, invalid_action;
    BOOL organization, accepted;
    int result = 0;
    SYSTEMTIME now;

    if (argc != 5)
    {
        LOG("signature-file-probe: done exit=2 invalid_arguments\n");
        return 2;
    }
    report = _wfopen(argv[1], L"wb");
    if (!report)
    {
        LOG("signature-file-probe: done exit=3 report_open_failed\n");
        return 3;
    }
    GetSystemTime(&now);
    LOG("signature-file-probe version=1 pointer_bits=%u utc=%04u-%02u-%02uT%02u:%02u:%02u\n",
        (unsigned)(sizeof(void *) * 8), now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond);
    registry();
    metadata(argv[2]);
    status = trust(argv[2], FALSE);
    organization = signer(argv[2], argv[3]);
    sip(argv[2]);
    invalid_action = trust(argv[2], TRUE);
    accepted = !status && organization;
    if (!wcscmp(argv[4], L"accept")) result = !accepted;
    else if (!wcscmp(argv[4], L"reject")) result = accepted;
    else if (wcscmp(argv[4], L"inspect")) result = 2;
    LOG("result accepted=%u wrong_action_rejected=%u assertion_exit=%d\n", accepted, !!invalid_action, result);
    LOG("signature-file-probe: done exit=%d\n", result);
    fclose(report);
    return result;
}
