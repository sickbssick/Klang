# K Language

**K** is a fast, low-level systems programming language with a clean syntax, a self-hosting compiler, and built-in support for bare-metal/UEFI kernel development.

> Compiler: `kcc` — compiles faster than `gcc -O0` on the same machine.

---

## Comments

``` k
$unpack-> <Safe>;
$entry-> <main>;

init::exec main[safe::nul] -> void {
    ;; This is a commment
    ;; This is where your code goes..

    ret 0;
}
```

## Install

Download the latest `kcc` binary from the [Releases](../../releases) page and put it in your PATH:

```bash
# Linux x86-64
chmod +x kcc
mv kcc ~/.local/bin/kcc
kcc --version   # kcc 0.5.4 (K self-hosted, x86_64-linux)
```

---

## Hello World

```k
$unpack-> <Safe>;
$entry-> <main>;

init::exec main[safe::nul] -> i32 {
    safe::print["Hello, World!\n"];
    ret 0;
}
```

```bash
kcc hello.k -o hello
./hello
```

---

## Language Overview

### Variables
```k
def x: i64 = 42;
def y: mut, f64 = 3.14;   ;; mutable
def name: *u8 = "Soph";
def arr: [4]i64 = {1, 2, 3, 4};
```

### Functions
```k
init::exec add[a: i64, b: i64] -> i64 {
    ret a + b;
}
```

### Control Flow
```k
ifd (x > 0)>>do {
    safe::print["positive\n"];
} else>>do {
    safe::print["non-positive\n"];
}

for (def i: mut, i64 = 0; i < 10; i++)>>do {
    safe::print["%d\n", i];
}

while (x > 0)>>do { x--; }
```

### Classes & Enums
```k
class Vec2 {
    x: f64;
    y: f64;
};

list::enum Direction {
    North = 0,
    South = 1,
    East  = 2,
    West  = 3
}
```

### Operators
| Operator | Meaning |
|:--|:--|
| `**` | Power (`2 ** 10 = 1024`) |
| `<<` `>>` | Bit shift |
| `&` `\|` `^` `~` | Bitwise |
| `&&` `\|\|` `!` | Logical |

### Format specifiers (`safe::print` / `safe::getin`)
| Specifier | Type |
|:--|:--|
| `%d` | `i64` integer |
| `%dd` | `f64` float |
| `%s` | `*u8` string |
| `%c` | `char` |

---

## Compiler Flags

```
kcc [flags] <files>

  -o <out>       Output file name
  -c             Compile to .o (no link)
  -S             Emit assembly (.s)
  -ffocus        Fastest build (piped asm + direct ld)
  -Wall/-derr    Extra warnings (unused vars, shadowing)
  -Werror        Warnings as errors
  -I<dir>        Header search path
  -l<lib>        Link library (e.g. -lSDL2)
  -ff            Freestanding (no libc)
  -kernel        Bare-metal kernel mode
  -T <ld.ld>     Custom linker script
  -time          Show per-phase timing
  -r             Build and run
  -v             Version
```

---

## Kernel / UEFI

K has first-class support for kernel development:

```k
$entry-> <kmain>;

def vga: pub, *u16 = 0xB8000;

init::exec kmain[safe::nul] -> void {
    vga[0] = 'K' | (0x0F << 8);
    ret;
}
```

```bash
kcc -kernel -ffocus -T linker.ld kernel.k -o kernel.elf
```

See [`examples/uefi/`](examples/uefi/) for a full UEFI bootable kernel.

---


## Examples

| File | Description |
|:--|:--|
| [`examples/hello.k`](examples/hello.k) | Hello World + types + loops |
| [`examples/lowlevel.k`](examples/lowlevel.k) | Port I/O, ISR, raw memory |
| [`examples/uefi/`](examples/uefi/) | Bootable UEFI kernel (QEMU/OVMF) |

---

## Safety

- **Garbage Collector**: When using `$unpack-> <Safe>`, all heap allocations (`malloc`, `calloc`, `realloc`) are GC-tracked and freed automatically on exit.
- **Immutability by default**: Variables are immutable unless declared `mut`.
- **Static type checking**: Type mismatches, undefined variables, out-of-bounds array indices, and format string errors are caught at compile time.

---

## License

Proprietary. Binary use permitted. Source not distributed.
