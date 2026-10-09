# Guest Virtual-Memory Regression

This Windows-only test uses a synthetic header and the actual guest-memory
mapping. It launches no game and requires no game assets or audio device.
Calls go through the real ordinal 184/199 thunk dispatcher rather than directly
invoking a host memory helper.

Coverage:

- 32-bit guest pointer/size fields with poisoned adjacent words.
- Exact stdcall cleanup for allocate/free.
- Page-rounded decommit, zero-fill, reservation ownership and recommit.
- Whole and Xbox-style partial releases, including live fragments on both sides.
- Coalescing after fragmented releases.
- Invalid ranges/types, double release and overflow, without IN/OUT mutation.
- Sixteen 8 MiB allocation/release cycles: 128 MiB of cumulative requests must
  succeed in the roughly 48 MiB guest heap without advancing allocation addresses.

The suite also builds the existing kernel bridge and physical-address tests
against the same runtime to check shared dispatch behavior.

```powershell
cmake -S xboxrecomp\tests\kernel_virtual_memory -B <build-dir> -G "Visual Studio 17 2022" -A x64
cmake --build <build-dir> --config Release --target kernel_virtual_memory_test kernel_virtual_memory_bridge_test kernel_virtual_memory_physical_test --parallel 2
ctest --test-dir <build-dir> -C Release --output-on-failure
```

The heap remains eagerly backed. These tests do not establish full virtual
page-management fidelity or prove that a gameplay map transition is crash-free.
