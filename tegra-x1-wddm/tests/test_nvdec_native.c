/* SPDX-License-Identifier: MIT */
/* Private transport controls with public KMT layouts and CPU substitutes.
 * No real KMT allocation, native device or decode completion is proved. */
#define WIN32_NO_STATUS
#include "nvdec_native.h"
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <d3dkmthk.h>
#include <assert.h>
#include <stdio.h>
static LONG create_status, destroy_status, close_status, delivery_status, operation_status;
static unsigned destroys, closes, escapes;
static int corrupt;
NTSTATUS APIENTRY D3DKMTOpenAdapterFromLuid(const D3DKMT_OPENADAPTERFROMLUID *q)
{
   assert(q->AdapterLuid.LowPart==0x1234 && q->AdapterLuid.HighPart==0);
   ((D3DKMT_OPENADAPTERFROMLUID *)q)->hAdapter=10;return 0;
}
NTSTATUS APIENTRY D3DKMTCreateDevice(D3DKMT_CREATEDEVICE *q)
{ assert(q->hAdapter==10);if(!create_status)q->hDevice=20;return create_status; }
NTSTATUS APIENTRY D3DKMTDestroyDevice(const D3DKMT_DESTROYDEVICE *q)
{ assert(q->hDevice==20);++destroys;return destroy_status; }
NTSTATUS APIENTRY D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *q)
{ assert(q->hAdapter==10);++closes;return close_status; }
NTSTATUS APIENTRY D3DKMTEscape(const D3DKMT_ESCAPE *q)
{
   assert(q->hAdapter==10 && q->hDevice==20 && !q->hContext && q->Flags.Value==1 && q->Type==D3DKMT_ESCAPE_DRIVERPRIVATE);
   struct tg_nvdec_escape *h=q->pPrivateDriverData;
   assert(h->size==q->PrivateDriverDataSize && !h->result && !h->reserved);++escapes;
   if(delivery_status)return delivery_status;
   h->result=operation_status;
   if(h->command==TG_NVDEC_ESCAPE_LOAD)h->session=31; /* retained even after failed boot */
   if(h->command==TG_NVDEC_ESCAPE_CLOSE && !operation_status)h->session=0;
   if(h->output_bytes)memset(h+1,0,h->output_bytes);
   if(corrupt==1)++h->session;
   if(corrupt==2)h->result=STATUS_PENDING;
   if(corrupt==3)++h->input_bytes;
   if(corrupt==4)h->version++;
   return 0;
}
int main(void)
{
   struct tg_nvdec_native c={0};
   struct tg_gpu_adapter_info i={.version=TG_GPU_ADAPTER_ABI,.size=sizeof(i),
      .flags=TG_GPU_ADAPTER_LUID_VALID|TG_GPU_ADAPTER_NATIVE_SUBMIT,.luid_low=0x1234};
   create_status=STATUS_NO_MEMORY;close_status=STATUS_DEVICE_BUSY;
   assert(tg_nvdec_native_open(&c,&i)==STATUS_DEVICE_BUSY && c.adapter==10 && !c.device);
   close_status=0;assert(tg_nvdec_native_close(&c)==0 && !c.adapter);
   create_status=0;assert(tg_nvdec_native_open(&c,&i)==0 && c.device==20);
   operation_status=STATUS_IO_TIMEOUT;
   assert(tg_nvdec_native_call(&c,TG_NVDEC_ESCAPE_LOAD,NULL,0,NULL,0)==STATUS_IO_TIMEOUT && c.session==31);
   unsigned before=escapes;assert(tg_nvdec_native_call(&c,TG_NVDEC_ESCAPE_LOAD,NULL,0,NULL,0)==STATUS_INVALID_HANDLE && escapes==before);
   operation_status=0;
   for(corrupt=1;corrupt<=4;++corrupt) {
      ULONG output=99;
      assert(tg_nvdec_native_call(&c,TG_NVDEC_ESCAPE_STATUS,NULL,0,&output,sizeof(output))==STATUS_DEVICE_PROTOCOL_ERROR);
      assert(output==99 && c.session==31);
   }
   corrupt=0;delivery_status=STATUS_PENDING;
   assert(tg_nvdec_native_call(&c,TG_NVDEC_ESCAPE_STATUS,NULL,0,NULL,0)==STATUS_PENDING && c.session==31);
   delivery_status=0;operation_status=STATUS_DEVICE_BUSY;destroy_status=STATUS_DEVICE_BUSY;
   before=closes;assert(tg_nvdec_native_close(&c)==STATUS_DEVICE_BUSY && c.device==20 && c.adapter==10 && c.session==31 && closes==before);
   operation_status=destroy_status=0;
   assert(tg_nvdec_native_close(&c)==0 && !c.device && !c.adapter && !c.session && destroys==2);
   puts("PASS: real-layout KMT packet validation and retained cleanup failures (CPU only)");return 0;
}
