/* Copyright (c) 2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * \file
 * Benchmark for Crc32C, a Nehalem instruction implementation of CRC32
 * with the Castagnoli polynomial.
 */

// RAMCloud pragma [CPPLINT=0]

#include <Common.h>
#include <BenchUtil.h>
#include <Crc32C.h>

static void
measure(int bytes, bool print = true)
{
    uint8_t *array = reinterpret_cast<uint8_t *>(xmalloc(bytes)); 
    uint32_t crc = 0;

    // randomize input
    for (int i = 0; i < bytes; i++)
        array[i] = generateRandom();

    // do more runs for smaller inputs
    int runs = 1;
    if (bytes < 4096)
        runs = 100;

    // run the test. be sure method call isn't removed by the compiler.
    uint64_t total = 0;
    for (int i = 0; i < runs; i++) {
        uint64_t before = rdtsc();
        crc = RAMCloud::Crc32C(crc, array, bytes);
        total += (rdtsc() - before);
    }
    total /= runs;

    if (print) {
        uint64_t nsec = RAMCloud::cyclesToNanoseconds(total);
        printf("%10d bytes: %10llu ticks    %10llu nsec    %3llu nsec/byte   "
            "%7llu MB/sec    crc32c 0x%08x\n",
            bytes,
            total,
            nsec,
            nsec / bytes,
            (uint64_t)(1.0e9 / ((float)nsec / bytes) / (1024*1024)),
            crc);
    }

    free(array);
}

int
main()
{
    measure(4096, false);   // warm up
    for (int i = 1; i < 128; i++)
        measure(i);
    for (int i = 128; i <= (16 * 1024 * 1024); i *= 2)
        measure(i);

    return 0;
}
