/* SPDX-License-Identifier: MIT */
/* The platform models use ASCII driver names stored as counted UTF-16. */
static BOOLEAN NTAPI ModelEqualUnicodeString(const UNICODE_STRING *a,const UNICODE_STRING *b,
    BOOLEAN ignore_case)
{
    if(a->Length!=b->Length)return FALSE;
    for(USHORT i=0;i<a->Length/sizeof(WCHAR);++i) {
        WCHAR left=a->Buffer[i],right=b->Buffer[i];
        if(ignore_case) {
            if(left>='A' && left<='Z')left+='a'-'A';
            if(right>='A' && right<='Z')right+='a'-'A';
        }
        if(left!=right)return FALSE;
    }
    return TRUE;
}
#ifdef TG_PLATFORM_PTHREAD_MODEL
#define RtlEqualUnicodeString ModelEqualUnicodeString
#else
BOOLEAN (NTAPI * const __imp_RtlEqualUnicodeString)(const UNICODE_STRING *,const UNICODE_STRING *,
    BOOLEAN)=ModelEqualUnicodeString;
#endif
