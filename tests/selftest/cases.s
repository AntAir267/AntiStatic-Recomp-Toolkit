# AntiStatic self-test: small x86 functions, each run on random inputs both in the Unicorn
# emulator and as the translator's C, compared register for register and byte for byte.
#
# Each case is a label `case_<name>:` ending in `ret`. A `# mask 0x...` line before it says which
# arithmetic flags its FL stores are compared on (CF 1, PF 4, AF 0x10, ZF 0x40, SF 0x80, OF 0x800;
# the default is all six): the others are undefined after that instruction on a real CPU.
# Inputs: eax..edi random (esp is the stack); 0x10000000.. scratch memory:
#   +0x000 64 random dwords   +0x100 32 doubles in [-1000, 1000]   +0x200 32 floats in [-1000, 1000]
#   +0x300 random bytes (a lookup table)   +0xf00.. results the cases write
# Hand-written for this test; no code from any game.

        .intel_syntax noprefix
        .set S, 0x10000000              # scratch
        .set R, 0x10000f00              # results
        .macro FL slot                  # the flags into a result slot
        pushfd
        pop dword ptr [R + \slot]
        mov dword ptr [esp - 4], 0      # (the copy left below the stack pointer)
        .endm
        .text

# ---------------------------------------------------------------- integer arithmetic
case_add:
        add eax, ebx
        FL 0
        adc ecx, edx
        FL 4
        sub esi, edi
        FL 8
        sbb eax, ecx
        FL 12
        ret
case_add8_16:
        add al, bh
        FL 0
        sub cx, dx
        FL 4
        adc ah, dl
        FL 8
        cmp si, di
        FL 12
        ret
case_incdec:
        inc eax
        FL 0
        dec bl
        FL 4
        neg ecx
        FL 8
        not edx
        neg si
        FL 12
        ret
case_cmp_mem:
        cmp dword ptr [S + 4], eax
        FL 0
        sub dword ptr [S + 8], 0x7f
        FL 4
        add byte ptr [S + 0x13], cl
        FL 8
        ret
# mask 0x8c5
case_logic:
        and eax, ebx
        FL 0
        or ecx, edx
        FL 4
        xor esi, edi
        FL 8
        test al, 0x41
        FL 12
        test word ptr [S + 0x10], 0x8001
        FL 16
        ret

# ---------------------------------------------------------------- shifts and rotates
# mask 0xc5
case_shifts:
        shl eax, 3
        FL 0
        shr ebx, 7
        FL 4
        sar ecx, 31
        FL 8
        mov ecx, edx
        and ecx, 0x3f
        shl esi, cl
        FL 12
        sar edi, cl
        FL 16
        shr dx, cl
        ret
# mask 0x8c5
case_shift1:
        shl eax, 1
        FL 0
        shr ebx, 1
        FL 4
        sar cl, 1
        FL 8
        ret
# mask 0x801
case_rotate1:
        rol eax, 1
        FL 0
        ror bx, 1
        FL 4
        rcl ecx, 1
        FL 8
        rcr dl, 1
        FL 12
        rcl si, 1
        FL 16
        ret
# mask 0x1
case_rotate_small:
        mov ecx, edi
        and ecx, 31
        rcl al, cl
        FL 0
        rcr bx, cl
        FL 4
        rcl dh, 5
        rcr si, 11
        FL 8
        ret
# mask 0x1
case_rotate:
        rol eax, 5
        FL 0
        ror ebx, 13
        FL 4
        mov ecx, edx
        rcl esi, cl
        rcr edi, 3
        ret
# mask 0xc5
case_shld:
        shld eax, ebx, 7
        FL 0
        shrd ecx, edx, 19
        FL 4
        mov ecx, esi
        and ecx, 31
        or ecx, 2
        shld edi, edx, cl
        FL 8
        shrd ax, bx, 5
        ret

# ---------------------------------------------------------------- multiply and divide
# mask 0x801
case_mul:
        mul ebx
        FL 0
        mov [R + 4], eax
        mov [R + 8], edx
        mov eax, ecx
        imul esi
        FL 12
        imul edi, ebx
        FL 16
        imul ecx, edx, -77
        FL 20
        mov al, bl
        mul cl
        FL 24
        mov ax, si
        imul dx
        ret
# mask 0x0
case_div:
        or ecx, 1
        xor edx, edx
        div ecx
        mov [R + 4], eax
        mov [R + 8], edx
        mov eax, ebx
        cdq
        or esi, 1
        idiv esi
        mov [R + 12], eax
        mov [R + 16], edx
        and edi, 0xffff
        or bl, 1
        mov eax, edi
        and eax, 0xff
        div bl                          # 8-bit div: AL = AX / BL, AH = AX % BL
        mov [R + 20], eax
        mov eax, edi
        and eax, 0x7f
        or dh, 0x81                     # negative divisor
        idiv dh
        mov [R + 24], eax
        mov eax, ebx                    # 16-bit: DX:AX / r16, the quotient kept in range
        and edx, 0x7f
        or cx, 0x100
        div cx
        mov [R + 28], eax
        mov [R + 32], edx
        mov eax, esi
        cwd
        or di, 0x8001                   # odd and negative
        idiv di
        ret

# ---------------------------------------------------------------- bits
# mask 0x1
case_bt:
        bt eax, ebx
        FL 0
        bts ecx, 5
        FL 4
        btr edx, esi
        FL 8
        btc edi, 31
        FL 12
        mov ebx, eax
        and ebx, 0x7f
        sub ebx, 64                     # -64..63: a bit string on both sides of S+0x40
        bt dword ptr [S + 0x40], ebx
        FL 16
        bts dword ptr [S + 0x40], ebx
        btr dword ptr [S + 0x48], 3
        FL 20
        ret
# mask 0x40
case_bsf:
        or eax, 1
        bsf ebx, eax
        FL 0
        bsr ecx, edx
        FL 4
        bswap esi
        ret
case_xadd:
        xadd eax, ebx
        FL 0
        xadd dword ptr [S + 0x20], ecx
        FL 4
        mov eax, [S + 0x24]
        cmpxchg dword ptr [S + 0x24], edx
        FL 8
        cmpxchg esi, edi
        FL 12
        ret

# ---------------------------------------------------------------- moves, conditions
case_cond:
        cmp eax, ebx
        setb byte ptr [R + 0]
        setg byte ptr [R + 1]
        setnp byte ptr [R + 2]
        setbe byte ptr [R + 3]
        cmovl ecx, edx
        cmovae esi, edi
        cmp al, 0x80
        cmovs edi, eax
        sbb edx, edx
        ret
case_moves:
        movsx eax, bl
        movzx ecx, dh
        movsx edx, si
        xchg ebx, edi
        xchg [S + 0x30], esi
        lea edi, [eax + ecx * 4 - 0x1234]
        mov ebx, S + 0x300
        mov al, dl
        xlatb
        cwde
        mov [R], eax
        mov eax, ecx
        cbw
        cwd
        ret
case_stack:
        pushad
        mov eax, 1
        mov ebp, 2
        popad
        push ebp
        enter 0x20, 0
        mov [ebp - 4], eax
        mov ecx, [ebp - 4]
        leave
        pop ebp
        pushfd
        or dword ptr [esp], 0x400       # DF
        popfd
        pushfd
        pop dword ptr [R]
        cld
        ret
case_lahf:
        add eax, ebx
        lahf
        mov [R], ah
        mov ah, cl
        sahf
        FL 4
        cmc
        FL 8
        stc
        FL 12
        clc
        FL 16
        ret

# ---------------------------------------------------------------- string instructions
case_strings:
        lea esi, [S + 0x00]
        lea edi, [S + 0x80]
        mov ecx, 9
        rep movsd
        mov ecx, 7
        rep movsb
        lea edi, [S + 0xc0]
        mov ecx, 5
        rep stosd
        lea edi, [S + 0x300]
        mov ecx, 64
        mov al, [S + 0x320]
        repne scasb
        FL 0
        mov [R + 4], ecx
        lea esi, [S + 0x300]
        lea edi, [S + 0x300]
        mov ecx, 16
        repe cmpsb
        FL 8
        mov [R + 12], ecx
        lodsw
        ret
case_strings_back:
        std
        lea esi, [S + 0x3f]
        lea edi, [S + 0xff]
        mov ecx, 12
        rep movsb
        cld
        ret

# ---------------------------------------------------------------- BCD
# mask 0xd5
case_daa:                               # valid packed BCD in (Unicorn's DAA differs from
        and bl, 0x77                    # the CPU on invalid input: QEMU tests AL > 0x9F)
        and cl, 0x77
        mov al, bl
        add al, cl
        daa
        FL 0
        mov [R + 4], eax
        and dl, 0x99
        and bh, 0x77
        mov al, dl
        sub al, bh
        das
        FL 8
        ret
# mask 0x11
case_aaa:
        mov ax, bx
        add al, cl
        aaa
        FL 0
        mov [R + 4], eax
        mov ax, dx
        sub al, ch
        aas
        ret
# mask 0xc4
case_aam:
        mov al, bl
        aam
        FL 0
        mov [R + 4], eax
        mov ax, cx
        and ax, 0x0f0f
        aad
        ret

# ---------------------------------------------------------------- control flow
case_fall:
        cmp eax, ebx                    # the flags go on into the next function (a split body)
case_fall_next:
        jb 1f
        mov eax, 1
        ret
1:      mov eax, 2
        ret
case_switch:
        and eax, 7
        jmp [switch_table + eax * 4]
2:      mov ecx, 10
        ret
3:      mov ecx, 11
        ret
4:      mov ecx, 12
        ret
5:      mov ecx, 13
        ret
        .p2align 2
switch_table:
        .long 2b, 3b, 4b, 5b, 2b, 3b, 4b, 5b
        .p2align 4
case_loop:
        mov ecx, eax
        and ecx, 15
        inc ecx
        xor edx, edx
1:      add edx, ecx
        loop 1b
        ret
case_call:
        push ebx
        call helper_sum
        add esp, 4
        ret
helper_sum:
        mov eax, [esp + 4]
        add eax, ecx
        ret

# ---------------------------------------------------------------- x87
case_x87_arith:
        fld qword ptr [S + 0x100]
        fadd qword ptr [S + 0x108]
        fld qword ptr [S + 0x110]
        fmul st, st(1)
        fsub qword ptr [S + 0x118]
        fdiv dword ptr [S + 0x200]
        fxch st(1)
        fsubr st, st(1)
        fstp qword ptr [R + 0]
        fstp qword ptr [R + 8]
        fld dword ptr [S + 0x204]
        fabs
        fsqrt
        fchs
        fstp dword ptr [R + 16]
        ret
case_x87_compare:
        fld qword ptr [S + 0x100]
        fcomp qword ptr [S + 0x108]
        fnstsw ax
        test ah, 0x41
        setz byte ptr [R + 0]
        fld qword ptr [S + 0x110]
        fld qword ptr [S + 0x118]
        fucompp
        fnstsw ax
        sahf
        seta byte ptr [R + 1]
        fld qword ptr [S + 0x120]
        fldz
        fcomip st, st(1)
        setb byte ptr [R + 2]
        fstp st(0)
        and eax, 0x4500                 # condition bits only (the exception bits differ by design)
        ret
case_x87_int:
        fild dword ptr [S + 0x00]
        fild word ptr [S + 0x04]
        faddp st(1), st
        fistp dword ptr [R + 0]
        fld qword ptr [S + 0x100]
        fistp qword ptr [R + 4]
        fld qword ptr [S + 0x108]
        frndint
        fstp qword ptr [R + 12]
        fild dword ptr [S + 0x08]
        fidiv dword ptr [S + 0x0c]
        fstp qword ptr [R + 20]
        ret
case_x87_rounding:
        sub esp, 4
        fnstcw [esp]
        mov ax, [esp]
        or ax, 0x0c00                   # truncate
        mov [esp + 2], ax
        fldcw [esp + 2]
        fld qword ptr [S + 0x100]
        fistp dword ptr [R + 0]
        and ax, 0xf3ff
        or ax, 0x0400                   # round down
        mov [esp + 2], ax
        fldcw [esp + 2]
        fld qword ptr [S + 0x108]
        fistp dword ptr [R + 4]
        fldcw [esp]
        add esp, 4
        ret
case_x87_consts:
        fldpi
        fld1
        faddp st(1), st
        fldl2e
        fmulp st(1), st
        fstp qword ptr [R]
        fld qword ptr [S + 0x100]
        fxtract
        fstp qword ptr [R + 8]
        fstp qword ptr [R + 16]
        ret

# packed BCD (fbld / fbstp: Borland's float-to-text), ffreep, and fptan / fsincos on an argument
# past 2^63 (nothing pushed, C2 set: the status word after them is compared)
case_x87_bcd:
        fld qword ptr [S + 0x100]
        fbstp tbyte ptr [R]
        fbld tbyte ptr [R]
        fstp qword ptr [R + 16]
        fld qword ptr [S + 0x108]
        fld1
        ffreep st(0)
        fstp qword ptr [R + 24]
        ret
# (fnstenv keeps this function's x87 stack in the CPU record, where the push-nothing case is
# followed; its environment is cleared after, the emulator's instruction pointers being its own)
case_x87_trig_far:
        fnstenv [R + 64]
        mov dword ptr [R + 64], 0
        mov dword ptr [R + 68], 0
        mov dword ptr [R + 72], 0
        mov dword ptr [R + 76], 0
        mov dword ptr [R + 80], 0
        mov dword ptr [R + 84], 0
        mov dword ptr [R + 88], 0
        mov dword ptr [R + 32], 0
        mov dword ptr [R + 36], 0x44000000   # 2^65 as a double (0x4400000000000000)
        fld qword ptr [R + 32]
        fptan
        fnstsw ax
        and eax, 0x4700
        mov [R], eax
        fsincos
        fnstsw ax
        and eax, 0x4700
        mov [R + 4], eax
        fstp qword ptr [R + 8]
        fld qword ptr [S + 0x100]
        fptan
        fstp qword ptr [R + 16]         # (the 1.0 it pushes; the tangent itself is left out: the
        fstp st(0)                      #  emulator's software x87 can be a last bit apart)
        ret
# pushf / popf with the operand-size prefix (16 bits), loop with the address-size prefix (cx)
case_flags16_loop16:
        cmp eax, ebx
        pushf
        pop dx
        and edx, 0x8d5
        mov [R], edx
        mov dx, [S]
        and dx, 0x08d5                  # (the arithmetic flags only: no trap or direction flag)
        push dx
        popf
        FL 4
        mov ecx, 0x00030005
        xor esi, esi
1:      inc esi
        addr16 loop 1b
        mov [R + 8], ecx
        mov [R + 12], esi
        ret

# ---------------------------------------------------------------- SSE and MMX
# mask 0xc5
case_sse:
        movss xmm0, [S + 0x200]
        addss xmm0, [S + 0x204]
        movups xmm1, [S + 0x208]
        mulps xmm1, xmm1
        minps xmm1, [S + 0x218]
        movups [R + 0], xmm1
        sqrtss xmm2, [S + 0x228]
        andps xmm2, xmm0
        cvttss2si eax, xmm0
        cvtsi2ss xmm3, ebx
        movss [R + 16], xmm3
        comiss xmm0, xmm3
        FL 20
        xorps xmm4, xmm4
        movss [R + 24], xmm0
        ret
# SSE2 doubles (the 32 random doubles at S+0x100): scalar and packed arithmetic, a compare into
# EFLAGS, conversions both ways, the register and memory forms of movsd
# mask 0xc5
case_sse2_double:
        movsd xmm0, [S + 0x100]
        addsd xmm0, [S + 0x108]
        movupd xmm1, [S + 0x110]
        mulpd xmm1, [S + 0x120]
        divsd xmm1, xmm0
        movsd xmm2, xmm1
        sqrtsd xmm3, [S + 0x130]
        minpd xmm3, xmm2
        maxsd xmm3, [S + 0x138]
        movupd [R + 0], xmm1
        movsd [R + 16], xmm3
        ucomisd xmm0, xmm1
        FL 24
        cvtsd2ss xmm4, xmm0
        cvtss2sd xmm5, xmm4
        movsd [R + 28], xmm5
        cvttsd2si eax, xmm0
        cvtsi2sd xmm6, ecx
        cmpltsd xmm6, xmm0
        movsd [R + 36], xmm6
        cvtdq2pd xmm7, [S + 0]
        cvtpd2ps xmm7, xmm7
        movlps [R + 44], xmm7
        xorpd xmm2, xmm2
        unpcklpd xmm2, xmm0
        shufpd xmm2, xmm1, 1
        movupd [R + 52], xmm2
        movsd [R + 68], xmm0
        ret
# the SSE2 integer forms on xmm: the byte shifts and half shuffles
case_sse2_int:
        movdqu xmm0, [S + 0]
        movdqu xmm1, xmm0
        pslldq xmm0, 3
        psrldq xmm1, 5
        pshufhw xmm2, [S + 16], 0x1b
        pshuflw xmm3, [S + 32], 0x93
        paddq xmm0, xmm1
        movdqu [R + 0], xmm0
        movdqu [R + 16], xmm2
        movdqu [R + 32], xmm3
        ret
case_mmx:
        movd mm0, eax
        movd mm1, ebx
        punpcklbw mm0, mm1
        movd mm2, ecx
        punpcklbw mm2, mm2
        pmullw mm0, mm2
        paddusw mm0, mm0
        psraw mm0, 3
        packsswb mm0, mm0
        movd eax, mm0
        emms
        ret

# fcmov after each kind of flag-setting instruction (xboxrecomp f4e42be: its FCMOV lost the carry);
# the doubles at +0x100 and +0x108 are the two choices, the integers set the flags
case_fcmov:
        fld qword ptr [S + 0x100]
        fld qword ptr [S + 0x108]       # st0 = b, st1 = a
        cmp eax, ebx
        fcmovb st(0), st(1)
        fst qword ptr [R + 0]
        cmp ecx, edx
        fcmovnbe st(0), st(1)
        fst qword ptr [R + 8]
        add eax, ebx                    # a carry from an add
        fld qword ptr [S + 0x110]
        fcmovnb st(0), st(2)
        fstp qword ptr [R + 16]
        sub esi, edi                    # from a sub, then ZF too
        fld qword ptr [S + 0x118]
        fcmovbe st(0), st(2)
        fstp qword ptr [R + 24]
        cmp esi, esi
        fld qword ptr [S + 0x120]
        fcmove st(0), st(1)
        fstp qword ptr [R + 32]
        test ecx, ecx
        fld qword ptr [S + 0x128]
        fcmovne st(0), st(2)
        fstp qword ptr [R + 40]
        test dl, 3                      # parity of the low byte
        fld qword ptr [S + 0x130]
        fcmovu st(0), st(1)
        fstp qword ptr [R + 48]
        test bl, 5
        fld qword ptr [S + 0x138]
        fcmovnu st(0), st(2)
        fstp qword ptr [R + 56]
        fstp qword ptr [R + 64]
        fstp qword ptr [R + 72]
        ret

# the SSE bitwise ops on whole registers and movq on xmm (xboxrecomp 27ce783: they vanished there)
case_sse_logic:
        movdqu xmm0, [S + 0]
        movdqu xmm1, [S + 16]
        movdqu xmm2, xmm0
        pand xmm2, xmm1
        movdqu xmm3, xmm0
        pandn xmm3, xmm1
        movdqu xmm4, xmm0
        por xmm4, [S + 32]
        movdqu xmm5, xmm1
        pxor xmm5, xmm0
        movups xmm6, [S + 48]
        andnps xmm6, xmm0
        orps xmm6, xmm3
        xorps xmm6, xmm2
        andps xmm6, xmm4
        movq xmm7, xmm5                 # the low quadword, the high one cleared
        movdqu [R + 0], xmm2
        movdqu [R + 16], xmm3
        movdqu [R + 32], xmm4
        movdqu [R + 48], xmm5
        movdqu [R + 64], xmm6
        movdqu [R + 80], xmm7
        movq xmm0, qword ptr [S + 64]
        movq qword ptr [R + 96], xmm1
        movdqu [R + 104], xmm0
        ret

# fincstp / fdecstp move the x87 stack top without touching the registers (xboxrecomp 27ce783)
case_x87_stackptr:
        fld qword ptr [S + 0x100]
        fld qword ptr [S + 0x108]       # st0 = b, st1 = a
        fdecstp
        fincstp                         # back: st0 = b
        fstp qword ptr [R + 0]
        fld qword ptr [S + 0x110]       # st0 = c, st1 = a
        fincstp                         # st0 = a (c now st7, still full)
        fstp qword ptr [R + 8]          # a; the stack is empty
        fdecstp
        fdecstp                         # st0 = c again
        fstp qword ptr [R + 16]
        fincstp                         # balanced: as many pushes as pops overall
        ret

# ---------------------------------------------------------------- x87 under every precision and rounding mode
# (SwARc's x87 oracle idea: the same arithmetic under each control word, 80-bit results compared bit
# for bit). The generated C computes on the host x87 with the program's control word, so the
# compiler must keep every rounding where the program's instructions put it.
        .macro X87_CW_CASE name, cw
case_\name:
        sub esp, 4
        fnstcw [esp]
        mov word ptr [esp + 2], \cw
        fldcw [esp + 2]
        fld qword ptr [S + 0x100]
        fdiv qword ptr [S + 0x108]
        fld qword ptr [S + 0x110]
        fmul st, st(1)
        fadd qword ptr [S + 0x118]
        fstp tbyte ptr [R + 0]
        fld st(0)
        fsqrt
        fstp tbyte ptr [R + 16]
        fld dword ptr [S + 0x200]
        fdiv dword ptr [S + 0x204]
        fst dword ptr [R + 32]
        fstp qword ptr [R + 40]
        fistp dword ptr [R + 48]
        fild dword ptr [S + 0x00]
        fidiv dword ptr [S + 0x04]
        frndint
        fstp tbyte ptr [R + 56]
        fldcw [esp]
        add esp, 4
        ret
        .endm
        X87_CW_CASE x87_pc24_near, 0x007f
        X87_CW_CASE x87_pc53_near, 0x027f
        X87_CW_CASE x87_pc64_near, 0x037f
        X87_CW_CASE x87_pc64_down, 0x077f
        X87_CW_CASE x87_pc64_up, 0x0b7f
        X87_CW_CASE x87_pc64_zero, 0x0f7f
        X87_CW_CASE x87_pc24_zero, 0x0c7f
        X87_CW_CASE x87_pc53_up, 0x0a7f
