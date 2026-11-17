/* SPDX-License-Identifier: MIT */
/* Select the exact Switch Lite provider package without a live PnP restart.
 * Public SetupAPI DI_DONOTCALLCONFIGMG/DI_NEEDREBOOT contract; the caller
 * verifies package signatures/hashes and owns the subsequent planned boot. */
#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef _UNICODE
#define _UNICODE 1
#endif
#include <windows.h>
#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static int failure(const char *stage)
{
    fprintf(stderr,"FAILED %s error=%lu\n",stage,(unsigned long)GetLastError());
    return 1;
}
int wmain(int argc,wchar_t **argv)
{
    setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
    if(argc!=3 || (wcscmp(argv[1],L"--inspect") && wcscmp(argv[1],L"--defer"))) {
        fprintf(stderr,"usage: defer-provider --inspect|--defer <verified-switchfan.inf>\n");return 2;
    }
    const int install=!wcscmp(argv[1],L"--defer");
    WCHAR inf[MAX_PATH];DWORD n=GetFullPathNameW(argv[2],MAX_PATH,inf,NULL);
    if(!n || n>=MAX_PATH)return failure("INF path");
    HDEVINFO set=SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_SYSTEM,NULL);
    if(set==INVALID_HANDLE_VALUE)return failure("CreateDeviceInfoList");
    int result=1;SP_DEVINFO_DATA dev={.cbSize=sizeof(dev)};
    SP_DEVINSTALL_PARAMS_W params={.cbSize=sizeof(params)};
    SP_DRVINFO_DATA_W selected={.cbSize=sizeof(selected)};
    BOOL built=FALSE;
    if(!SetupDiOpenDeviceInfoW(set,L"ACPI\\SWOA0402\\0",NULL,0,&dev)) {failure("OpenDeviceInfo");goto out;}
    if(!IsEqualGUID(&dev.ClassGuid,&GUID_DEVCLASS_SYSTEM)) {fprintf(stderr,"Unexpected device class\n");goto out;}
    if(!SetupDiGetDeviceInstallParamsW(set,&dev,&params)) {failure("GetDeviceInstallParams");goto out;}
    params.Flags|=DI_ENUMSINGLEINF|DI_DONOTCALLCONFIGMG|DI_NEEDREBOOT|DI_QUIETINSTALL;
    memcpy(params.DriverPath,inf,(wcslen(inf)+1)*sizeof(WCHAR));
    if(!SetupDiSetDeviceInstallParamsW(set,&dev,&params)) {failure("SetDeviceInstallParams");goto out;}
    if(!SetupDiBuildDriverInfoList(set,&dev,SPDIT_COMPATDRIVER)) {failure("BuildDriverInfoList");goto out;}
    built=TRUE;DWORD found=0;
    for(DWORD index=0;;++index) {
        SP_DRVINFO_DATA_W driver={.cbSize=sizeof(driver)};
        if(!SetupDiEnumDriverInfoW(set,&dev,SPDIT_COMPATDRIVER,index,&driver)) {
            if(GetLastError()!=ERROR_NO_MORE_ITEMS){failure("EnumDriverInfo");goto out;}
            break;
        }
        DWORD bytes=0;
        if(SetupDiGetDriverInfoDetailW(set,&dev,&driver,NULL,0,&bytes) || GetLastError()!=ERROR_INSUFFICIENT_BUFFER ||
           bytes<sizeof(SP_DRVINFO_DETAIL_DATA_W) || bytes>65536u) {failure("DriverInfoDetail size");goto out;}
        SP_DRVINFO_DETAIL_DATA_W *detail=calloc(1,bytes);
        if(!detail){fprintf(stderr,"Out of memory\n");goto out;}
        detail->cbSize=sizeof(*detail);
        if(!SetupDiGetDriverInfoDetailW(set,&dev,&driver,detail,bytes,NULL)) {free(detail);failure("DriverInfoDetail");goto out;}
        if(!_wcsicmp(detail->InfFileName,inf)) {selected=driver;++found;}
        free(detail);
    }
    if(found!=1) {fprintf(stderr,"Expected one exact compatible INF, found %lu\n",(unsigned long)found);goto out;}
    if(!SetupDiSetSelectedDriverW(set,&dev,&selected)) {failure("SetSelectedDriver");goto out;}
    if(!SetupDiGetDeviceInstallParamsW(set,&dev,&params)) {failure("Read selection flags");goto out;}
    params.Flags|=DI_DONOTCALLCONFIGMG|DI_NEEDREBOOT|DI_QUIETINSTALL;
    if(!SetupDiSetDeviceInstallParamsW(set,&dev,&params)) {failure("Set deferred flags");goto out;}
    printf("TARGET=ACPI\\SWOA0402\\0 version=%llu flags=0x%08lx\n",
           (unsigned long long)selected.DriverVersion,(unsigned long)params.Flags);
    wprintf(L"INF=%ls\n",inf);
    if(!install) {puts("RESULT=INSPECTED no installation requested");result=0;goto out;}
    puts("CALL=DIF_INSTALLDEVICE deferred; live Configuration Manager restart prohibited");
    if(!SetupDiCallClassInstaller(DIF_INSTALLDEVICE,set,&dev)) {failure("DIF_INSTALLDEVICE");goto out;}
    if(!SetupDiGetDeviceInstallParamsW(set,&dev,&params)) {failure("Read installed flags");goto out;}
    if((params.Flags&(DI_DONOTCALLCONFIGMG|DI_NEEDREBOOT))!=(DI_DONOTCALLCONFIGMG|DI_NEEDREBOOT)) {
        fprintf(stderr,"Deferred installation flags were not retained: 0x%08lx\n",(unsigned long)params.Flags);goto out;
    }
    puts("RESULT=STAGED_FOR_REBOOT live driver identity is not verified");result=0;
out:
    if(built)SetupDiDestroyDriverInfoList(set,&dev,SPDIT_COMPATDRIVER);
    SetupDiDestroyDeviceInfoList(set);return result;
}
