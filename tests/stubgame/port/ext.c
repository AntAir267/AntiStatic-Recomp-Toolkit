/* A stand-in only the port has (stubext.dll!StubExtValue), defined in a file that includes the
   translation's funcs.h, as ports' own DLL stand-ins do. Its definition must win over the
   generated fallback (missing_<name>) on every linker: ELF and Windows (COFF), in any link order. */
#include "runtime.h"
#include "funcs.h"
API(STUBEXT, StubExtValue, 0) { (void)c; return 7; }
