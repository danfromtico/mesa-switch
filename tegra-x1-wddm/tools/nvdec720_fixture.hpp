/* SPDX-License-Identifier: MIT */
#ifndef NVDEC720_FIXTURE_HPP
#define NVDEC720_FIXTURE_HPP
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

template<typename T> struct Com {
   T *p = nullptr;
   Com() = default;
   Com(const Com &) = delete;
   Com &operator=(const Com &) = delete;
   ~Com() { reset(); }
   T *operator->() const { return p; }
   T **put() { reset(); return &p; }
   void reset() { if (p) { p->Release(); p=nullptr; } }
};
struct Failure { HRESULT result; const char *call; };
struct Mapping {
   ID3D11DeviceContext *context;
   ID3D11Resource *resource;
   bool active = true;
   ~Mapping() { finish(); }
   void finish() { if (active) { context->Unmap(resource,0); active=false; } }
};
static void require(HRESULT hr, const char *call)
{
   std::printf("RETURN %s=0x%08lx\n", call, (unsigned long)hr);
   if (FAILED(hr)) throw Failure{hr,call};
}
static void check(bool ok, const char *what)
{ if (!ok) throw Failure{E_FAIL,what}; }

static std::vector<uint8_t> references(const wchar_t *path)
{
   FILE *f = _wfopen(path,L"rb"); check(f != nullptr,"open reference");
   std::vector<uint8_t> out; char hash[65]; unsigned index;
   while (std::fscanf(f,"%u %64s",&index,hash)==2) {
      if (index != out.size()/32 || std::strlen(hash)!=64 || index>=64) { std::fclose(f); check(false,"reference index"); }
      for (unsigned i=0;i<32;++i) {
         unsigned value;
         if (std::sscanf(hash+i*2,"%2x",&value)!=1) { std::fclose(f); check(false,"reference hash"); }
         out.push_back((uint8_t)value);
      }
   }
   std::fclose(f); check(out.size()==6*32,"six-frame fixture reference"); return out;
}

#endif
