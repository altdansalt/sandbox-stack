// Control harness: run a guest .wasm under wazero's interpreter with the same
// env.read/env.write/env.exit imports as the sandbox host. Not part of the TCB.
package main

import (
	"context"
	"fmt"
	"os"

	"github.com/tetratelabs/wazero"
	"github.com/tetratelabs/wazero/api"
	"github.com/tetratelabs/wazero/sys"
)

func main() {
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: control guest.wasm")
		os.Exit(2)
	}
	wasm, err := os.ReadFile(os.Args[1])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	ctx := context.Background()
	r := wazero.NewRuntimeWithConfig(ctx, wazero.NewRuntimeConfigInterpreter())
	defer r.Close(ctx)

	fdRead := func(_ context.Context, m api.Module, fd, ptr, n uint32) uint32 {
		if fd != 0 {
			return 0xFFFFFFFF
		}
		buf, ok := m.Memory().Read(ptr, n)
		if !ok {
			panic("read: out of bounds")
		}
		k, err := os.Stdin.Read(buf)
		if err != nil && k == 0 {
			return 0
		}
		return uint32(k)
	}
	fdWrite := func(_ context.Context, m api.Module, fd, ptr, n uint32) uint32 {
		if fd != 1 && fd != 2 {
			return 0xFFFFFFFF
		}
		buf, ok := m.Memory().Read(ptr, n)
		if !ok {
			panic("write: out of bounds")
		}
		f := os.Stdout
		if fd == 2 {
			f = os.Stderr
		}
		k, _ := f.Write(buf)
		return uint32(k)
	}
	exit := func(_ context.Context, code uint32) {
		os.Stdout.Sync()
		os.Exit(int(code))
	}
	_, err = r.NewHostModuleBuilder("env").
		NewFunctionBuilder().WithFunc(fdRead).Export("read").
		NewFunctionBuilder().WithFunc(fdWrite).Export("write").
		NewFunctionBuilder().WithFunc(exit).Export("exit").
		Instantiate(ctx)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(2)
	}
	_, err = r.InstantiateWithConfig(ctx, wasm, wazero.NewModuleConfig().WithStartFunctions("_start"))
	if err != nil {
		if e, ok := err.(*sys.ExitError); ok {
			os.Exit(int(e.ExitCode()))
		}
		fmt.Fprintln(os.Stderr, "control:", err)
		os.Exit(3)
	}
}
