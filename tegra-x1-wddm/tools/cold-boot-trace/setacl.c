/* SPDX-License-Identifier: MIT
 * WinPE may not resolve the target DWM virtual group through LookupAccountSid.
 * Construct its ACL using numeric SIDs; never grant access to Everyone.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <stdio.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *sddl;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (argc != 3) return 2;
    if (!wcscmp(argv[2], L"traverse"))
        sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;FRFX;;;S-1-5-90-0)";
    else if (!wcscmp(argv[2], L"dwm"))
        sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;S-1-5-90-0)";
    else if (!wcscmp(argv[2], L"system"))
        sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    else return 2;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &sd, NULL)) {
        printf("SDDL_FAILED %lu\n", GetLastError()); return 3;
    }
    BOOL ok = SetFileSecurityW(argv[1], DACL_SECURITY_INFORMATION |
                              PROTECTED_DACL_SECURITY_INFORMATION, sd);
    DWORD error = ok ? 0 : GetLastError();
    LocalFree(sd);
    if (!ok) { printf("SET_ACL_FAILED %lu\n", error); return 4; }
    DWORD bytes = 0;
    GetFileSecurityW(argv[1], DACL_SECURITY_INFORMATION, NULL, 0, &bytes);
    if (!bytes) return 5;
    sd = LocalAlloc(LMEM_FIXED, bytes);
    if (!sd) return 6;
    wchar_t *actual = NULL;
    ok = GetFileSecurityW(argv[1], DACL_SECURITY_INFORMATION, sd, bytes, &bytes) &&
         ConvertSecurityDescriptorToStringSecurityDescriptorW(
             sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &actual, NULL);
    if (ok) wprintf(L"ACL %ls %ls\n", argv[1], actual);
    if (actual) LocalFree(actual);
    LocalFree(sd);
    return ok ? 0 : 7;
}
