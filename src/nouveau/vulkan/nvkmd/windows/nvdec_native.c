/* SPDX-License-Identifier: MIT */
#define WIN32_NO_STATUS
#include "nvdec_native.h"
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <d3dkmthk.h>
#include <stdlib.h>
#include <string.h>

LONG tg_nvdec_native_open(struct tg_nvdec_native *c, const struct tg_gpu_adapter_info *i)
{
   if (!c || c->adapter || c->device || c->session || !i ||
       i->version!=TG_GPU_ADAPTER_ABI || i->size!=sizeof(*i) || i->reserved ||
       (i->flags&(TG_GPU_ADAPTER_LUID_VALID|TG_GPU_ADAPTER_NATIVE_SUBMIT))!=
          (TG_GPU_ADAPTER_LUID_VALID|TG_GPU_ADAPTER_NATIVE_SUBMIT)) return STATUS_INVALID_PARAMETER;
   D3DKMT_OPENADAPTERFROMLUID a={.AdapterLuid={.LowPart=i->luid_low,.HighPart=i->luid_high}};
   LONG status=D3DKMTOpenAdapterFromLuid(&a);
   if (status!=STATUS_SUCCESS) return status;
   c->adapter=a.hAdapter;
   D3DKMT_CREATEDEVICE d={.hAdapter=c->adapter};
   status=D3DKMTCreateDevice(&d);
   if (status==STATUS_SUCCESS) c->device=d.hDevice;
   else {
      LONG cleanup=tg_nvdec_native_close(c);
      if (cleanup!=STATUS_SUCCESS) return cleanup;
   }
   return status;
}
LONG tg_nvdec_native_call(struct tg_nvdec_native *c,uint32_t command,
   const void *input,uint32_t input_size,void *output,uint32_t output_size)
{
   uint32_t payload=input_size>output_size?input_size:output_size;
   if (!c || !c->adapter || !c->device || (input_size && !input) || (output_size && !output) ||
       payload>TG_NVDEC_ESCAPE_MAX_PAYLOAD || command<TG_NVDEC_ESCAPE_LOAD || command>TG_NVDEC_ESCAPE_CLOSE)
      return STATUS_INVALID_PARAMETER;
   if (command==TG_NVDEC_ESCAPE_LOAD ? c->session!=0 : c->session==0) return STATUS_INVALID_HANDLE;
   struct tg_nvdec_escape *h=calloc(1,sizeof(*h)+payload);
   if (!h) return STATUS_NO_MEMORY;
   *h=(struct tg_nvdec_escape){TG_NVDEC_ESCAPE_MAGIC,TG_NVDEC_ESCAPE_VERSION,
      (uint32_t)sizeof(*h)+payload,command,input_size,output_size,0,0,c->session};
   if (input_size) memcpy(h+1,input,input_size);
   D3DKMT_ESCAPE e={.hAdapter=c->adapter,.hDevice=c->device,.Type=D3DKMT_ESCAPE_DRIVERPRIVATE,
      .Flags={.HardwareAccess=1},.pPrivateDriverData=h,.PrivateDriverDataSize=h->size};
   LONG status=D3DKMTEscape(&e);
   if (status==STATUS_SUCCESS) {
      if (h->magic!=TG_NVDEC_ESCAPE_MAGIC || h->version!=TG_NVDEC_ESCAPE_VERSION ||
          h->size!=sizeof(*h)+payload || h->command!=command || h->reserved ||
          h->input_bytes!=input_size || h->output_bytes!=output_size || h->result>0 ||
          (command==TG_NVDEC_ESCAPE_LOAD && h->result==0 && !h->session) ||
          (command==TG_NVDEC_ESCAPE_CLOSE && h->result==0 && h->session) ||
          (command!=TG_NVDEC_ESCAPE_LOAD && !(command==TG_NVDEC_ESCAPE_CLOSE && h->result==0) && h->session!=c->session))
         status=STATUS_DEVICE_PROTOCOL_ERROR;
      else {
         c->session=h->session; status=h->result;
         if (output_size) memcpy(output,h+1,output_size);
      }
   }
   free(h);
   return status;
}
LONG tg_nvdec_native_close(struct tg_nvdec_native *c)
{
   if (!c) return STATUS_INVALID_PARAMETER;
   LONG status=STATUS_SUCCESS;
   if (c->session && c->device)
      status=tg_nvdec_native_call(c,TG_NVDEC_ESCAPE_CLOSE,NULL,0,NULL,0);
   if (c->device) {
      const D3DKMT_DESTROYDEVICE d={.hDevice=c->device};
      LONG next=D3DKMTDestroyDevice(&d);
      if (next==STATUS_SUCCESS) c->device=0,c->session=0;
      else status=next;
   }
   if (c->adapter && !c->device) {
      const D3DKMT_CLOSEADAPTER a={.hAdapter=c->adapter};
      LONG next=D3DKMTCloseAdapter(&a);
      if (next==STATUS_SUCCESS) c->adapter=0;
      else status=next;
   }
   return status;
}
