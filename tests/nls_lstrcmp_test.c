/* SPDX-License-Identifier: MIT */
/* Fedora 45 regression: Win32 x64 ABI and exact kernel32 IAT name.
 * The current implementation is a case-sensitive ordinal UTF-16
 * comparison; locale-tailored native Windows collation is not claimed. */
#include "compat.h"
#include "nls.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef int32_t (NT_MS_ABI *compare_w)(const uint16_t *, const uint16_t *);
static int failures;
static void expect(int valid, const char *name)
{
    if (!valid) {
        ++failures;
        fprintf(stderr, "FAIL lstrcmpW: %s\n", name);
    }
}
static int sign(int value) { return (value > 0) - (value < 0); }

int main(void)
{
    compare_w kernel = (compare_w)nt_runtime_resolve("KERNEL32.dll", "lstrcmpW");
    compare_w base = (compare_w)nt_runtime_resolve("KERNELBASE.dll", "lstrcmpW");
    expect(kernel != NULL && base != NULL, "exact Win32/KERNELBASE export");
    if (kernel == NULL || base == NULL)
        return EXIT_FAILURE;
    expect(kernel(u"", u"") == 0, "two empty strings");
    expect(kernel(u"", u"a") < 0 && kernel(u"a", u"") > 0, "empty vs nonempty");
    expect(kernel(u"same", u"same") == 0, "equal ASCII");
    expect(kernel(u"ab", u"abc") < 0 && kernel(u"abc", u"ab") > 0, "prefix sign");
    expect(kernel(u"abc", u"abd") < 0 && kernel(u"abd", u"abc") > 0, "different suffix");
    expect(kernel(u"A", u"a") != 0, "case-sensitive comparison");
    expect(kernel(u"Grüße", u"Grüße") == 0, "German Unicode");
    expect(kernel(u"árbol", u"árbol") == 0, "Spanish accented Unicode");
    expect(kernel(u"niño", u"niño") == 0, "Spanish tilde Unicode");
    expect(kernel(u"😀", u"😀") == 0, "surrogate pair Unicode");
    expect(sign(kernel(u"niño", u"nino")) ==
           -sign(kernel(u"nino", u"niño")), "antisymmetric Unicode sign");
    expect(sign(kernel(u"abc", u"abd")) ==
           sign(base(u"abc", u"abd")), "KERNELBASE export same implementation");
    nt_SetLastError(0x14445u);
    expect(kernel(u"Grüße", u"Grüße") == 0 &&
           nt_GetLastError() == 0x14445u, "does not clobber LastError on valid input");
    if (!failures)
        puts("PASS lstrcmpW: UTF-16, exact kernel32/kernelbase exports, sign, ABI, LastError");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
