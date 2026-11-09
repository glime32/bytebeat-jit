<p align="center"><img src="docs/banner-bytebeat-jit.png" alt="bytebeat-jit" width="100%"></p>

<p align="center"><img src="https://img.shields.io/badge/C-x86--64%20JIT-3949ab?style=flat-square" alt="C"> <img src="https://img.shields.io/badge/license-MIT-610089?style=flat-square" alt="license"></p>

Music from **a single line of arithmetic**, compiled **to x86-64 machine code on the fly**.

<p align="center"><a href="README.fr.md">🇫🇷 Version française</a></p>

*Bytebeat* (viznut, 2011): evaluate a formula of `t`, the sample number, 8000 times per second, keep the low byte, and send it to the sound card. `t*(42&t>>10)` already makes a melody that repeats and evolves.

This program reads the formula, builds its syntax tree, and runs it with **three engines** written from scratch:

| Engine | Idea | Speed (`t*5&(t>>7)\|t*3&(t*4>>10)`) |
|---|---|---|
| tree | recursive walk of the tree for every sample | 19 M samples/s |
| stack | compiled to *bytecode* for a small stack machine | 21 M samples/s |
| **jit** | compiled to x86-64 instructions, written byte by byte into memory, then run | **490 M samples/s** |

The JIT is 15 to 30 times faster than the other two, about 60,000 times real time.

![Spectrogram of t*(42&t>>10)](images/spectro1.png)
![Spectrogram of t*5&(t>>7)|t*3&(t*4>>10)](images/spectro2.png)

*Spectrograms of two formulas (made with [spectrogramme-secret](https://github.com/glime32/spectrogramme-secret)). You can see the first one's melody, and the repeating structure.*

## Usage

`--jouer` = play, `-d` = duration in seconds.

```sh
make
./bytebeat "t*(42&t>>10)" --jouer                 # listen (aplay, pw-play or paplay)
./bytebeat "t*(42&t>>10)" -o son.wav -d 60        # one minute as WAV
./bytebeat "t*(42&t>>10)" | aplay -f U8           # raw bytes on standard output
./bytebeat "t*(42&t>>10)" --code                  # bytecode, machine code and disassembly
./bytebeat "t*(42&t>>10)" --bench                 # the three engines head to head
./bytebeat --fuzz 3000                            # 3000 random formulas: do the engines agree?
```

More formulas in [`formules.txt`](formules.txt). The language: `t`, numbers (`42`, `0xFF`), and C operators with their precedence: `+ - * / % & | ^ << >> ~ ! < <= > >= == != && || ?:`.

## What the JIT does

```
$ ./bytebeat "t*(42&t>>10)" --code
code x86-64 (22 octets) :
   0:  89 f8              mov    eax,edi        ; eax = t
   2:  b9 0a 00 00 00     mov    ecx,0xa
   7:  d3 f8              sar    eax,cl         ; t >> 10
   9:  b9 2a 00 00 00     mov    ecx,0x2a
   e:  21 c8              and    eax,ecx        ; & 42
  10:  89 f9              mov    ecx,edi
  12:  0f af c1           imul   eax,ecx        ; * t
  15:  c3                 ret
```

1. **Parsing.** A lexer splits the text, then a *precedence climbing* parser builds the tree following C's precedence (`t*(42&t>>10)`: `>>` before `&`, parentheses before `*`).
2. **Constant folding.** Every subtree without `t` is computed once and for all (`3*4+t` → `12+t`), and a `?:` with a constant condition loses its dead branch.
3. **Code generation.** `t` arrives in register `edi` (System V calling convention), the result goes back in `eax`. For `a OP b`: compute `b`, set it aside (`push`), compute `a`, get `b` back into `ecx` (`pop`) and emit `op eax, ecx`. When `b` is a constant or `t`, the stack isn't needed; for a commutative operator, the operands are swapped if that avoids it.
4. **Jumps.** `?:`, `&&` and `||` become conditional jumps (`jz`, `jmp`). Their target isn't known yet when they are written: 4 bytes are left empty and filled in once the target has been generated.
5. **Running.** The code is written into a page obtained with `mmap`, switched to read + execute with `mprotect` (never writable and executable at the same time), then called like a C function `int32_t f(int32_t t)`.

**Traps.** `idiv` crashes the CPU (`SIGFPE`) on a division by 0 *and* on `INT_MIN / -1`. The generated code checks both cases before dividing. Shifts are taken modulo 32 (as the CPU does), `>>` keeps the sign, and every computation wraps around on 32 bits: the three engines have exactly the same semantics.

**Testing with random formulas (`--fuzz`).** A generator writes random formulas (with tricky values: 0, −1, 31, 32, 2147483647...). Each one goes through the tree, the stack machine, the JIT and the folded tree, for 400 values of `t`: all four results must be identical. 3000 formulas, 1.2 million comparisons, not a single difference.

The JIT only writes x86-64; elsewhere (ARM...), the program uses the stack machine.
