/* SPDX-License-Identifier: MIT */
/* Never executed: proves import preflight catches a KERNEL32 symbol
 * absent from our runtime while accepting the PE import table itself. */
__declspec(dllimport) void __stdcall MissingNtNativeApi(void);

int import_fixture_entry(void)
{
    MissingNtNativeApi();
    return 0;
}
