#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/disk_cache.h"
#include "util/mesa-blake3.h"

static bool decode_hex(const char *text, uint8_t *bytes)
{
   if (strlen(text) != 2 * BLAKE3_OUT_LEN)
      return false;
   for (unsigned i = 0; i < BLAKE3_OUT_LEN; i++) {
      const char *digits = "0123456789abcdef";
      const char *hi = strchr(digits, text[2 * i]);
      const char *lo = strchr(digits, text[2 * i + 1]);
      if (!hi || !lo)
         return false;
      bytes[i] = ((hi - digits) << 4) | (lo - digits);
   }
   return true;
}

int main(int argc, char **argv)
{
   if (argc != 2 && argc != 3)
      return 2;
   FILE *input = fopen(argv[1], "rb");
   if (!input)
      return 3;
   struct disk_cache *cache = disk_cache_create("GM20B replay", "physical cache reader", 0);
   if (!cache) {
      fclose(input);
      return 4;
   }
   struct disk_cache *reference = NULL;
   if (argc == 3) {
      if (!SetEnvironmentVariableA("MESA_SHADER_CACHE_DIR", argv[2]))
         return 7;
      reference = disk_cache_create("GM20B replay", "physical cache reader", 0);
      if (!reference)
         return 8;
   }
   unsigned total = 0, accepted = 0, missed = 0, failures = 0, rewritten = 0;
   char key_hex[65], digest_hex[65];
   unsigned wanted, should_hit;
   while (fscanf(input, "%64s %64s %u %u", key_hex, digest_hex, &wanted, &should_hit) == 4) {
      cache_key key;
      blake3_hash stored;
      if (!decode_hex(key_hex, key) || !decode_hex(digest_hex, stored) || should_hit > 1)
         return 5;
      size_t got = SIZE_MAX;
      void *blob = disk_cache_get(cache, key, &got);
      const bool hit = blob != NULL;
      if (hit) {
         blake3_hash digest;
         _mesa_blake3_compute(blob, got, digest);
         failures += !should_hit || got != wanted || memcmp(digest, stored, sizeof(stored)) != 0;
         accepted++;
         free(blob);
      } else {
         failures += should_hit || got != 0;
         missed++;
         if (reference && !should_hit) {
            size_t size = 0;
            void *other = disk_cache_get(reference, key, &size);
            blake3_hash digest;
            if (other && size == wanted) {
               _mesa_blake3_compute(other, size, digest);
               if (!memcmp(digest, stored, sizeof(stored))) {
                  disk_cache_put(cache, key, other, size, NULL);
                  rewritten++;
               } else {
                  failures++;
               }
            } else {
               failures++;
            }
            free(other);
         }
      }
      printf("%s,%zu,%s\n", key_hex, got, hit ? "hit" : "miss");
      total++;
   }
   const bool complete = feof(input) && !ferror(input);
   fclose(input);
   disk_cache_wait_for_idle(cache);
   disk_cache_destroy(reference);
   disk_cache_destroy(cache);
   printf("total=%u hits=%u misses=%u rewritten=%u disagreements=%u\n", total, accepted, missed, rewritten, failures);
   return complete && total && !failures ? 0 : 6;
}
