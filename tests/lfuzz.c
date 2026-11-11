/* lfuzz.c — coverage-guided fuzz harness for demux.c (libFuzzer).
 * Same invariants as test_demux --fuzz (window/PES bounds, AU length),
 * but the mutator is libFuzzer's edge-guided engine instead of blind
 * xorshift mutation. Each input is self-contained: state resets first
 * so any crash reproduces from the artifact file alone. Build & run:
 *   clang -g -O1 -fsanitize=fuzzer,address,undefined \
 *     -fno-sanitize-recover=all -o fuzz_demux lfuzz.c demux.c
 *   ./test_demux --seeds fuzz_seeds && ./fuzz_demux -max_total_time=90 \
 *     -use_value_profile=1 fuzz_corpus fuzz_seeds
 */
#include <string.h>
#include <stdint.h>
#include "demux.h"

#define PORT 15550

static struct demux d;

static void lf_au(void *ctx, const unsigned char *au, long len)
{
    (void)ctx; (void)au;
    if (len <= 0 || len > DEMUX_ES_CAP)
        __builtin_trap();            /* AU length invariant */
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    memset(&d, 0, sizeof d);
    d.pmt_pid = d.vpid = d.apid = -1;

    demux_frame(&d, data, (int)size, PORT, lf_au, NULL);
    demux_eos(&d, lf_au, NULL);      /* close + flush like golden replay */

    if (d.esr > d.esw || d.esw > DEMUX_ES_CAP || d.peslen > DEMUX_PES_CAP)
        __builtin_trap();            /* window / PES bounds invariant */
    return 0;
}
