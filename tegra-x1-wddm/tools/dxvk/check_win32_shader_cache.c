// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "blake3.h"

static void hex32(const uint8_t *data, char out[65]) {
    static const char chars[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[2*i] = chars[data[i] >> 4];
        out[2*i+1] = chars[data[i] & 15];
    }
    out[64] = 0;
}

static bool has_key_filename(const wchar_t *name) {
    return wcslen(name) == 68 && wcscmp(name + 64, L".bin") == 0 &&
           wcsspn(name, L"0123456789abcdef") == 64;
}

static bool inspect(const wchar_t *directory, const wchar_t *name, unsigned *valid_count) {
    wchar_t path[1024];
    if (swprintf(path, 1024, L"%ls\\%ls", directory, name) < 0) return false;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    uint8_t header[80] = {0}, buf[8192], digest[32];
    DWORD got = 0;
    LARGE_INTEGER file_size;
    bool ok = GetFileSizeEx(f, &file_size) && ReadFile(f, header, sizeof(header), &got, NULL);
    const bool complete_header = ok && got == sizeof(header);
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    uint64_t payload = 0, trailing_zero = 0;
    while (ok) {
        ok = ReadFile(f, buf, sizeof(buf), &got, NULL);
        if (!ok || !got) break;
        blake3_hasher_update(&hasher, buf, got);
        payload += got;
        for (DWORD i=0; i<got; i++) {
            if (buf[i]) trailing_zero=0;
            else trailing_zero++;
        }
    }
    CloseHandle(f);
    if (!ok) return false;
    blake3_hasher_finalize(&hasher, digest, sizeof(digest));
    uint64_t magic = 0, stated_size = 0;
    memcpy(&magic, header, 8);
    memcpy(&stated_size, header + 8, 8);
    char name_key[65], header_key[65], actual[65], expected[65];
    for (unsigned i=0; i<64; i++) name_key[i]=(char)name[i];
    name_key[64]=0;
    hex32(header + 16, header_key);
    hex32(header + 48, expected);
    hex32(digest, actual);
    const bool layout = complete_header && magic == UINT64_C(0x314548434143534d) &&
                        payload == stated_size && (uint64_t)file_size.QuadPart == 80 + payload &&
                        strcmp(header_key, name_key) == 0;
    const bool valid = layout && memcmp(header + 48, digest, sizeof(digest)) == 0;
    *valid_count += valid;
    printf("ENTRY %s %llu %d %d %s %s %llu\n", name_key, (unsigned long long)payload,
           layout, valid, expected, actual, (unsigned long long)trailing_zero);
    return true;
}

int wmain(int argc, wchar_t **argv) {
    blake3_hasher hasher;
    uint8_t digest[32]; char hex[65];
    blake3_hasher_init(&hasher);
    blake3_hasher_finalize(&hasher, digest, sizeof(digest));
    hex32(digest, hex);
    if (strcmp(hex, "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262")) return 3;
    if (argc != 2) return 2;
    wchar_t pattern[1024];
    if (swprintf(pattern, 1024, L"%ls\\*.bin", argv[1]) < 0) return 2;
    WIN32_FIND_DATAW data;
    HANDLE dir = FindFirstFileW(pattern, &data);
    if (dir == INVALID_HANDLE_VALUE) return 4;
    unsigned scanned=0, valid=0; bool ok=true;
    do {
        if (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!has_key_filename(data.cFileName)) continue;
        if (!inspect(argv[1], data.cFileName, &valid)) { ok=false; break; }
        scanned++;
    } while (FindNextFileW(dir, &data));
    DWORD status=GetLastError();
    FindClose(dir);
    printf("TOTAL %u %u\n", scanned, valid);
    return ok && status == ERROR_NO_MORE_FILES && scanned ? 0 : 5;
}
