;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Copyright (C) 2023, jpn
;
; Licensed under the Apache License, Version 2.0 (the "License");
; you may not use this file except in compliance with the License.
; You may obtain a copy of the License at
;
; http://www.apache.org/licenses/LICENSE-2.0
;
; Unless required by applicable law or agreed to in writing, software
; distributed under the License is distributed on an "AS IS" BASIS,
; WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
; See the License for the specific language governing permissions and
; limitations under the License.
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;


default rel


%ifidn __?OUTPUT_FORMAT?__, elf64
	%define SYSTEMV64
%endif
%ifidn __?OUTPUT_FORMAT?__, macho64
	%define SYSTEMV64
%endif

%ifndef SYSTEMV64
	%ifidn __?OUTPUT_FORMAT?__, win64
		%define WINDOWS64
	%else
		%error ABI not supported.
	%endif
%endif


; for better column aligment
%define xmmA xmm10
%define xmmB xmm11
%define xmmC xmm12
%define xmmD xmm13
%define xmmE xmm14
%define xmmF xmm15


; argument registers
%ifdef SYSTEMV64
	%define ar1 rdi
	%define ar2 rsi
	%define ar3 rdx
	%define ar4 rcx
%endif

%ifdef WINDOWS64
	%define ar1 rcx
	%define ar2 rdx
	%define ar3 r8
	%define ar4 r9
%endif

section .text
align 16


global jpgr_inverseDCTASM
; Parameters:
; (int16 pointer) sblock , rblock, qtable

global jpgr_upsamplerowASM
; Parameters:
; (pointer) int16 row, int16 target, int mode(0-6)

global jpgr_setrow1ASM
; Parameters:
; (pointer) int16 row source, (pointer) int8 target

global jpgr_setrow3ASM
; Parameters:
; (pointer) int16 row1, row2, row2, (pointer) int8 target, int transform

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Initialize the variables according to the CPU capabilities
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

SSSE3_FLAG equ 0000200h


init:
	; preserve registers
	push		rcx
	push		rdx
	push		rbx
	push		rax

	xor			rax, rax
	cmp			eax, dword[hasSSSE3.initdone]
	jne .done

	mov			eax, 1
	cpuid
	and			ecx, SSSE3_FLAG

	test		ecx, ecx
	jz .done

	; ssse3
	lea			rax, [hasSSSE3]
	mov			dword[rax], 1h

.done:
	lea			rax, [hasSSSE3.initdone]
	mov			dword[rax], 1h

	pop			rax
	pop			rbx
	pop			rdx
	pop			rcx
	jmp rax


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; SSE2 version
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; Constants
C6xSQRT2 equ  4433
S6xSQRT2 equ 10703
A equ   2446
B equ  16819
C equ  25172
D equ  12299
E equ  -7373
F equ -20995
G equ -16069
H equ  -3196
I equ   9633

align 16
rotation1a: dw 4 dup ( S6xSQRT2, C6xSQRT2)
rotation1b: dw 4 dup (-C6xSQRT2, S6xSQRT2)

upscalez0: dw 8 dup (+8192)
upscalez1: dw 4 dup (+8192, -8192)

cI_IsumG: dw 4 dup (I+G, I)
cIsumH_I: dw 4 dup (I, I+H)

cE_DsumE: dw 4 dup (D+E, E)
cAsumE_E: dw 4 dup (E, A+E)

cF_CsumF: dw 4 dup (C+F, F)
cBsumF_F: dw 4 dup (F, B+F)

bias1:
	dd 4 dup (2048)
bias2:
	dd 4 dup (65536)


; ar1=sblock, ar2=rblock, ar3=qtable
jpgr_inverseDCTASM:
%ifdef WINDOWS64
	; preserve xmm6-xmm14
	sub			rsp, 98h

	movaps		[rsp+ 0h], xmm6
	movaps		[rsp+10h], xmm7
	movaps		[rsp+20h], xmm8
	movaps		[rsp+30h], xmm9
	movaps		[rsp+40h], xmmA
	movaps		[rsp+50h], xmmB
	movaps		[rsp+60h], xmmC
	movaps		[rsp+70h], xmmD
	movaps		[rsp+80h], xmmE
%endif

	; load and upscale l0 and l1
	movdqa xmm0, [ar1+00h]  ; l0
	movdqa xmm1, [ar1+40h]  ; l1
	pmullw xmm0, [ar3+00h]
	pmullw xmm1, [ar3+40h]

	; z0 = (l0 + l1) << 13
	; z1 = (l0 - l1) << 13
	movdqa		xmm2, xmm0
	punpcklwd	xmm2, xmm1  ; a1
	punpckhwd	xmm0, xmm1  ; a2
	movdqa		xmm1, xmm2  ; a1
	movdqa		xmm3, xmm0  ; a2
	pmaddwd		xmm2, [upscalez0]  ; xmm2=z0lo
	pmaddwd		xmm0, [upscalez0]  ; xmm0=z0hi
	pmaddwd		xmm1, [upscalez1]  ; xmm1=z1lo
	pmaddwd		xmm3, [upscalez1]  ; xmm3=z1hi

	; load and upscale l2 and l3
	movdqa xmm4, [ar1+20h]  ; l2
	movdqa xmm5, [ar1+60h]  ; l3
	pmullw xmm4, [ar3+20h]
	pmullw xmm5, [ar3+60h]

	; rotation 1:
	; z2 = l2 * -(C6xSQRT2) + l3 * (S6xSQRT2);
	; z3 = l2 *  (S6xSQRT2) + l3 * (C6xSQRT2);
	movdqa		xmm6, xmm4
	punpcklwd	xmm6, xmm5  ; a1
	punpckhwd	xmm4, xmm5  ; a2
	movdqa		xmm5, xmm6  ; a1
	movdqa		xmm7, xmm4  ; a2
	pmaddwd		xmm6, [rotation1b]  ; xmm6=z2lo
	pmaddwd		xmm4, [rotation1b]  ; xmm4=z2hi
	pmaddwd		xmm5, [rotation1a]  ; xmm5=z3lo
	pmaddwd		xmm7, [rotation1a]  ; xmm7=z3hi

%ifdef WINDOWS64
	sub			rsp, 80h
%endif
%ifdef SYSTEMV64
	sub			rsp, 88h
%endif

	; stage 2
	; l0 = z3 + z0;
	; l1 = z1 - z2;
	; l2 = z2 + z1;
	; l3 = z0 - z3;
	movdqa		xmm8, xmm5
	movdqa		xmm9, xmm7
	paddd		xmm8, xmm2  ; l0lo
	paddd		xmm9, xmm0  ; l0hi
	psubd		xmm2, xmm5  ; l3lo
	psubd		xmm0, xmm7  ; l3hi

	; push lane 0 and lane 3 on the stack
	movaps		[rsp+00h], xmm8
	movaps		[rsp+10h], xmm9
	movaps		[rsp+60h], xmm2
	movaps		[rsp+70h], xmm0

	movdqa		xmm8, xmm1
	movdqa		xmm9, xmm3
	psubd		xmm1, xmm6  ; l1lo
	psubd		xmm3, xmm4  ; l1hi
	paddd		xmm6, xmm8  ; l2lo
	paddd		xmm4, xmm9  ; l2hi

	; push lane 1 and lane 2 on the stack
	movaps		[rsp+20h], xmm1
	movaps		[rsp+30h], xmm3
	movaps		[rsp+40h], xmm6
	movaps		[rsp+50h], xmm4

	; odd part
	; z1 = l4 + l7;
	; z2 = l5 + l6;
	; z3 = (int16) (l4 + l6);
	; z4 = (int16) (l5 + l7);
	; z5 = z3 + z4;

	; load and upscale l4 and l5
	movdqa xmm0, [ar1+70h]  ; l4
	movdqa xmm1, [ar1+50h]  ; l5
	pmullw xmm0, [ar3+70h]
	pmullw xmm1, [ar3+50h]

	; load and upscale l6 and l7
	movdqa xmm2, [ar1+30h]  ; l6
	movdqa xmm3, [ar1+10h]  ; l7
	pmullw xmm2, [ar3+30h]
	pmullw xmm3, [ar3+10h]

	movdqa		xmm4, xmm0
	movdqa		xmm5, xmm1
	paddw		xmm4, xmm2  ; z3 = (int16) (l4 + l6);
	paddw		xmm5, xmm3  ; z4 = (int16) (l5 + l7);

	; original:
	; z5 = (z3 + z4) * I
	; z3 = z3 * G
	; z4 = z4 * H
	; z3 += z5
	; z4 += z5
	;
	; this implementation:
	; z3 = z3 * (I    ) + z4 * (I + G)
	; z4 = z3 * (I + H) + z4 * (I)
	movdqa		xmm6, xmm4
	punpcklwd	xmm6, xmm5
	punpckhwd	xmm4, xmm5

	movdqa		xmm7, xmm6
	movdqa		xmm5, xmm4
	pmaddwd		xmm6, [cI_IsumG]  ; z3lo
	pmaddwd		xmm4, [cI_IsumG]  ; z3hi
	pmaddwd		xmm7, [cIsumH_I]  ; z4lo
	pmaddwd		xmm5, [cIsumH_I]  ; z4hi

	; dirty: xmm0 xmm1 xmm2 xmm3 xmm4 xmm5 xmm6 xmm7

	; (l7 * D) + ((l4 + l7) * E)
	; (l4 * A) + ((l4 + l7) * E)
	; (l6 * C) + ((l5 + l6) * F)
	; (l5 * B) + ((l5 + l6) * F)
	;
	; then
	; x * (a + b) + y * b
	movdqa		xmm8, xmm3  ; l7
	punpcklwd	xmm3, xmm0  ; lo
	punpckhwd	xmm8, xmm0  ; hi
	movdqa		xmm0, xmm3  ; lo
	movdqa		xmm9, xmm8  ; hi
	pmaddwd		xmm3, [cE_DsumE]  ; l7lo
	pmaddwd		xmm8, [cE_DsumE]  ; l7hi
	pmaddwd		xmm0, [cAsumE_E]  ; l4lo
	pmaddwd		xmm9, [cAsumE_E]  ; l4hi

	movdqa		xmmA, xmm2  ; l6
	punpcklwd	xmm2, xmm1  ; lo
	punpckhwd	xmmA, xmm1  ; hi
	movdqa		xmm1, xmm2  ; lo
	movdqa		xmmB, xmmA  ; hi
	pmaddwd		xmm2, [cF_CsumF]  ; l6lo
	pmaddwd		xmmA, [cF_CsumF]  ; l6hi
	pmaddwd		xmm1, [cBsumF_F]  ; l5lo
	pmaddwd		xmmB, [cBsumF_F]  ; l5hi

	; dirty: xmm0 xmm1 xmm2 xmm3 xmm4 xmm5 xmm6 xmm7 xmm8 xmm9 xmmA xmmB

	; l7 += z4;
	; l5 += z4;
	;
	; l6 += z3;
	; l4 += z3;
	paddd		xmm3, xmm7  ; l7
	paddd		xmm8, xmm5  ; l7
	paddd		xmm1, xmm7  ; l5
	paddd		xmmB, xmm5  ; l5

	paddd		xmm2, xmm6  ; l6
	paddd		xmmA, xmm4  ; l6
	paddd		xmm0, xmm6  ; l4
	paddd		xmm9, xmm4  ; l4

	; dirty: xmm0 xmm1 xmm2 xmm3 xmm8 xmm9 xmmA xmmB
	movdqa		xmmC, [bias1]

	; last stage
	; l0 = ((l0 + l7) + 2048) >> 12
	; l4 = ((l0 - l7) + 2048) >> 12
	; l7 = ((l1 + l6) + 2048) >> 12
	; l3 = ((l1 - l6) + 2048) >> 12
	; l2 = ((l2 + l5) + 2048) >> 12
	; l5 = ((l2 - l5) + 2048) >> 12
	; l6 = ((l3 + l4) + 2048) >> 12
	; l1 = ((l3 - l4) + 2048) >> 12
	movaps		xmm5, [rsp+ 0h]
	movaps		xmm6, [rsp+10h]
	paddd		xmm5, xmm3
	paddd		xmm6, xmm8
	paddd		xmm5, xmmC
	paddd		xmm6, xmmC
	psrad		xmm5, 12
	psrad		xmm6, 12
	pslld		xmm5, 16
	pslld		xmm6, 16
	psrad		xmm5, 16
	psrad		xmm6, 16
	packssdw	xmm5, xmm6  ; xmm5=l0

	movaps		xmm6, [rsp+ 0h]
	movaps		xmm7, [rsp+10h]
	psubd		xmm6, xmm3
	psubd		xmm7, xmm8
	paddd		xmm6, xmmC
	paddd		xmm7, xmmC
	psrad		xmm6, 12
	psrad		xmm7, 12
	pslld		xmm6, 16
	pslld		xmm7, 16
	psrad		xmm6, 16
	psrad		xmm7, 16
	packssdw	xmm6, xmm7  ; xmm6=l4

	movaps		xmm7, [rsp+20h]
	movaps		xmm8, [rsp+30h]
	paddd		xmm7, xmm2
	paddd		xmm8, xmmA
	paddd		xmm7, xmmC
	paddd		xmm8, xmmC
	psrad		xmm7, 12
	psrad		xmm8, 12
	pslld		xmm7, 16
	pslld		xmm8, 16
	psrad		xmm7, 16
	psrad		xmm8, 16
	packssdw	xmm7, xmm8  ; xmm7=l7

	movaps		xmm8, [rsp+20h]
	movaps		xmm3, [rsp+30h]
	psubd		xmm8, xmm2
	psubd		xmm3, xmmA
	paddd		xmm8, xmmC
	paddd		xmm3, xmmC
	psrad		xmm8, 12
	psrad		xmm3, 12
	pslld		xmm8, 16
	pslld		xmm3, 16
	psrad		xmm8, 16
	psrad		xmm3, 16
	packssdw	xmm8, xmm3  ; xmm8=l3

	movaps		xmm2, [rsp+40h]
	movaps		xmm3, [rsp+50h]
	paddd		xmm2, xmm1
	paddd		xmm3, xmmB
	paddd		xmm2, xmmC
	paddd		xmm3, xmmC
	psrad		xmm2, 12
	psrad		xmm3, 12
	pslld		xmm2, 16
	pslld		xmm3, 16
	psrad		xmm2, 16
	psrad		xmm3, 16
	packssdw	xmm2, xmm3  ; xmm2=l2

	movaps		xmm3, [rsp+40h]
	movaps		xmmA, [rsp+50h]
	psubd		xmm3, xmm1
	psubd		xmmA, xmmB
	paddd		xmm3, xmmC
	paddd		xmmA, xmmC
	psrad		xmm3, 12
	psrad		xmmA, 12
	pslld		xmm3, 16
	pslld		xmmA, 16
	psrad		xmm3, 16
	psrad		xmmA, 16
	packssdw	xmm3, xmmA  ; xmm3=l5

	movaps		xmm1, [rsp+60h]
	movaps		xmmA, [rsp+70h]
	paddd		xmm1, xmm0
	paddd		xmmA, xmm9
	paddd		xmm1, xmmC
	paddd		xmmA, xmmC
	psrad		xmm1, 12
	psrad		xmmA, 12
	pslld		xmm1, 16
	pslld		xmmA, 16
	psrad		xmm1, 16
	psrad		xmmA, 16
	packssdw	xmm1, xmmA  ; xmm1=l6

	movaps		xmm4, [rsp+60h]
	movaps		xmmA, [rsp+70h]
	psubd		xmm4, xmm0
	psubd		xmmA, xmm9
	paddd		xmm4, xmmC
	paddd		xmmA, xmmC
	psrad		xmm4, 12
	psrad		xmmA, 12
	pslld		xmm4, 16
	pslld		xmmA, 16
	psrad		xmm4, 16
	psrad		xmmA, 16
	packssdw	xmm4, xmmA  ; xmm4=l1

	; transpose the 8x8 matrix
	; xmm5=l0
	; xmm4=l1
	; xmm2=l2
	; xmm8=l3
	; xmm6=l4
	; xmm3=l5
	; xmm1=l6
	; xmm7=l7

	movdqa		xmm0, xmm5
	punpcklwd	xmm5, xmm4  ; l0
	punpckhwd	xmm0, xmm4  ; l1
	movdqa		xmm9, xmm7
	punpcklwd	xmm7, xmm3  ; l7
	punpckhwd	xmm9, xmm3  ; l5
	movdqa		xmm4, xmm2
	punpcklwd	xmm2, xmm8  ; l2
	punpckhwd	xmm4, xmm8  ; l3
	movdqa		xmm3, xmm1
	punpcklwd	xmm1, xmm6  ; l6
	punpckhwd	xmm3, xmm6  ; l4

	; xmm5=l0
	; xmm0=l1
	; xmm2=l2
	; xmm4=l3
	; xmm3=l4
	; xmm9=l5
	; xmm1=l6
	; xmm7=l7

	movdqa		xmm6, xmm5
	punpcklwd	xmm5, xmm2  ; l0
	punpckhwd	xmm6, xmm2  ; l2
	movdqa		xmm8, xmm7
	punpcklwd	xmm7, xmm1  ; l7
	punpckhwd	xmm8, xmm1  ; l6
	movdqa		xmm2, xmm0
	punpcklwd	xmm0, xmm4  ; l1
	punpckhwd	xmm2, xmm4  ; l3
	movdqa		xmm1, xmm9
	punpcklwd	xmm9, xmm3  ; l5
	punpckhwd	xmm1, xmm3  ; l4

	; xmm5=l0
	; xmm0=l1
	; xmm6=l2
	; xmm2=l3
	; xmm1=l4
	; xmm9=l5
	; xmm8=l6
	; xmm7=l7

	movdqa		xmm3, xmm5
	punpcklwd	xmm5, xmm7  ; l0
	punpckhwd	xmm3, xmm7  ; l7
	movdqa		xmm4, xmm6
	punpcklwd	xmm6, xmm8  ; l2
	punpckhwd	xmm4, xmm8  ; l6
	movdqa		xmm7, xmm0
	punpcklwd	xmm0, xmm9  ; l1
	punpckhwd	xmm7, xmm9  ; l5
	movdqa		xmm8, xmm2
	punpcklwd	xmm2, xmm1  ; l3
	punpckhwd	xmm8, xmm1  ; l4

	; xmm5=l0
	; xmm0=l1
	; xmm6=l2
	; xmm2=l3
	; xmm8=l4
	; xmm7=l5
	; xmm4=l6
	; xmm3=l7

	; second pass

	; rearrange registers
	movdqa		xmmA, xmm8
	movdqa		xmmB, xmm7
	movdqa		xmmC, xmm4
	movdqa		xmmD, xmm3

	movdqa		xmm1, xmm0  ; l1
	movdqa		xmm0, xmm5  ; l0
	movdqa		xmm4, xmm6  ; l2
	movdqa		xmm5, xmm2  ; l3

	; z0 = (l0 + l1) << 13
	; z1 = (l0 - l1) << 13
	movdqa		xmm2, xmm0
	punpcklwd	xmm2, xmm1  ; a1
	punpckhwd	xmm0, xmm1  ; a2
	movdqa		xmm1, xmm2  ; a1
	movdqa		xmm3, xmm0  ; a2
	pmaddwd		xmm2, [upscalez0]  ; xmm2=z0lo
	pmaddwd		xmm0, [upscalez0]  ; xmm0=z0hi
	pmaddwd		xmm1, [upscalez1]  ; xmm1=z1lo
	pmaddwd		xmm3, [upscalez1]  ; xmm3=z1hi

	; rotation 1:
	; z2 = l2 * -(C6xSQRT2) + l3 * (S6xSQRT2);
	; z3 = l2 *  (S6xSQRT2) + l3 * (C6xSQRT2);
	movdqa		xmm6, xmm4
	punpcklwd	xmm6, xmm5  ; a1
	punpckhwd	xmm4, xmm5  ; a2
	movdqa		xmm5, xmm6  ; a1
	movdqa		xmm7, xmm4  ; a2
	pmaddwd		xmm6, [rotation1b]  ; xmm6=z2lo
	pmaddwd		xmm4, [rotation1b]  ; xmm4=z2hi
	pmaddwd		xmm5, [rotation1a]  ; xmm5=z3lo
	pmaddwd		xmm7, [rotation1a]  ; xmm7=z3hi

	; stage 2
	; l0 = z3 + z0;
	; l1 = z1 - z2;
	; l2 = z2 + z1;
	; l3 = z0 - z3;
	movdqa		xmm8, xmm5
	movdqa		xmm9, xmm7
	paddd		xmm8, xmm2  ; l0lo
	paddd		xmm9, xmm0  ; l0hi
	psubd		xmm2, xmm5  ; l3lo
	psubd		xmm0, xmm7  ; l3hi

	; push lane 0 and lane 3 on the stack
	movaps		[rsp+00h], xmm8
	movaps		[rsp+10h], xmm9
	movaps		[rsp+60h], xmm2
	movaps		[rsp+70h], xmm0

	movdqa		xmm8, xmm1
	movdqa		xmm9, xmm3
	psubd		xmm1, xmm6  ; l1lo
	psubd		xmm3, xmm4  ; l1hi
	paddd		xmm6, xmm8  ; l2lo
	paddd		xmm4, xmm9  ; l2hi

	; push lane 1 and lane 2 on the stack
	movaps		[rsp+20h], xmm1
	movaps		[rsp+30h], xmm3
	movaps		[rsp+40h], xmm6
	movaps		[rsp+50h], xmm4

	; odd part
	; z1 = l4 + l7;
	; z2 = l5 + l6;
	; z3 = (int16) (l4 + l6);
	; z4 = (int16) (l5 + l7);
	; z5 = z3 + z4;

	; l4 and l5
	movdqa xmm0, xmmA  ; l4
	movdqa xmm1, xmmB  ; l5

	; l6 and l7
	movdqa xmm2, xmmC  ; l6
	movdqa xmm3, xmmD  ; l7

	movdqa		xmm4, xmm0
	movdqa		xmm5, xmm1
	paddw		xmm4, xmm2  ; z3 = (int16) (l4 + l6);
	paddw		xmm5, xmm3  ; z4 = (int16) (l5 + l7);

	; original:
	; z5 = (z3 + z4) * I
	; z3 = z3 * G
	; z4 = z4 * H
	; z3 += z5
	; z4 += z5
	;
	; this implementation:
	; z3 = z3 * (I    ) + z4 * (I + G)
	; z4 = z3 * (I + H) + z4 * (I)
	movdqa		xmm6, xmm4
	punpcklwd	xmm6, xmm5
	punpckhwd	xmm4, xmm5

	movdqa		xmm7, xmm6
	movdqa		xmm5, xmm4
	pmaddwd		xmm6, [cI_IsumG]  ; z3lo
	pmaddwd		xmm4, [cI_IsumG]  ; z3hi
	pmaddwd		xmm7, [cIsumH_I]  ; z4lo
	pmaddwd		xmm5, [cIsumH_I]  ; z4hi

	; dirty: xmm0 xmm1 xmm2 xmm3 xmm4 xmm5 xmm6 xmm7

	; (l7 * D) + ((l4 + l7) * E)
	; (l4 * A) + ((l4 + l7) * E)
	; (l6 * C) + ((l5 + l6) * F)
	; (l5 * B) + ((l5 + l6) * F)
	;
	; then
	; x * (a + b) + y * b
	movdqa		xmm8, xmm3  ; l7
	punpcklwd	xmm3, xmm0  ; lo
	punpckhwd	xmm8, xmm0  ; hi
	movdqa		xmm0, xmm3  ; lo
	movdqa		xmm9, xmm8  ; hi
	pmaddwd		xmm3, [cE_DsumE]  ; l7lo
	pmaddwd		xmm8, [cE_DsumE]  ; l7hi
	pmaddwd		xmm0, [cAsumE_E]  ; l4lo
	pmaddwd		xmm9, [cAsumE_E]  ; l4hi

	movdqa		xmmA, xmm2  ; l6
	punpcklwd	xmm2, xmm1  ; lo
	punpckhwd	xmmA, xmm1  ; hi
	movdqa		xmm1, xmm2  ; lo
	movdqa		xmmB, xmmA  ; hi
	pmaddwd		xmm2, [cF_CsumF]  ; l6lo
	pmaddwd		xmmA, [cF_CsumF]  ; l6hi
	pmaddwd		xmm1, [cBsumF_F]  ; l5lo
	pmaddwd		xmmB, [cBsumF_F]  ; l5hi

	; dirty: xmm0 xmm1 xmm2 xmm3 xmm4 xmm5 xmm6 xmm7 xmm8 xmm9 xmmA xmmB

	; l7 += z4;
	; l5 += z4;
	;
	; l6 += z3;
	; l4 += z3;
	paddd		xmm3, xmm7  ; l7
	paddd		xmm8, xmm5  ; l7
	paddd		xmm1, xmm7  ; l5
	paddd		xmmB, xmm5  ; l5

	paddd		xmm2, xmm6  ; l6
	paddd		xmmA, xmm4  ; l6
	paddd		xmm0, xmm6  ; l4
	paddd		xmm9, xmm4  ; l4

	; dirty: xmm0 xmm1 xmm2 xmm3 xmm8 xmm9 xmmA xmmB
	movdqa		xmmC, [bias2]

	; last stage
	; row0 = ((l0 + l7) + 65536) >> 17
	; row7 = ((l0 - l7) + 65536) >> 17
	; row1 = ((l1 + l6) + 65536) >> 17
	; row6 = ((l1 - l6) + 65536) >> 17
	; row2 = ((l2 + l5) + 65536) >> 17
	; row5 = ((l2 - l5) + 65536) >> 17
	; row3 = ((l3 + l4) + 65536) >> 17
	; row4 = ((l3 - l4) + 65536) >> 17
	movaps		xmm5, [rsp+ 0h]
	movaps		xmm6, [rsp+10h]
	paddd		xmm5, xmm3
	paddd		xmm6, xmm8
	paddd		xmm5, xmmC
	paddd		xmm6, xmmC
	psrad		xmm5, 17
	psrad		xmm6, 17
	packssdw	xmm5, xmm6  ; xmm5=l0

	movaps		xmm6, [rsp+ 0h]
	movaps		xmm7, [rsp+10h]
	psubd		xmm6, xmm3
	psubd		xmm7, xmm8
	paddd		xmm6, xmmC
	paddd		xmm7, xmmC
	psrad		xmm6, 17
	psrad		xmm7, 17
	packssdw	xmm6, xmm7  ; xmm6=l4

	movdqa [ar2+00h], xmm5
	movdqa [ar2+70h], xmm6

	movaps		xmm7, [rsp+20h]
	movaps		xmm8, [rsp+30h]
	paddd		xmm7, xmm2
	paddd		xmm8, xmmA
	paddd		xmm7, xmmC
	paddd		xmm8, xmmC
	psrad		xmm7, 17
	psrad		xmm8, 17
	packssdw	xmm7, xmm8  ; xmm7=l7

	movaps		xmm8, [rsp+20h]
	movaps		xmm3, [rsp+30h]
	psubd		xmm8, xmm2
	psubd		xmm3, xmmA
	paddd		xmm8, xmmC
	paddd		xmm3, xmmC
	psrad		xmm8, 17
	psrad		xmm3, 17
	packssdw	xmm8, xmm3  ; xmm8=l3

	movdqa [ar2+10h], xmm7
	movdqa [ar2+60h], xmm8

	movaps		xmm2, [rsp+40h]
	movaps		xmm3, [rsp+50h]
	paddd		xmm2, xmm1
	paddd		xmm3, xmmB
	paddd		xmm2, xmmC
	paddd		xmm3, xmmC
	psrad		xmm2, 17
	psrad		xmm3, 17
	packssdw	xmm2, xmm3  ; xmm2=l2

	movaps		xmm3, [rsp+40h]
	movaps		xmmA, [rsp+50h]
	psubd		xmm3, xmm1
	psubd		xmmA, xmmB
	paddd		xmm3, xmmC
	paddd		xmmA, xmmC
	psrad		xmm3, 17
	psrad		xmmA, 17
	packssdw	xmm3, xmmA  ; xmm3=l5

	movdqa [ar2+20h], xmm2
	movdqa [ar2+50h], xmm3

	movaps		xmm1, [rsp+60h]
	movaps		xmmA, [rsp+70h]
	paddd		xmm1, xmm0
	paddd		xmmA, xmm9
	paddd		xmm1, xmmC
	paddd		xmmA, xmmC
	psrad		xmm1, 17
	psrad		xmmA, 17
	packssdw	xmm1, xmmA  ; xmm1=l6

	movaps		xmm4, [rsp+60h]
	movaps		xmmA, [rsp+70h]
	psubd		xmm4, xmm0
	psubd		xmmA, xmm9
	paddd		xmm4, xmmC
	paddd		xmmA, xmmC
	psrad		xmm4, 17
	psrad		xmmA, 17
	packssdw	xmm4, xmmA  ; xmm4=l1

	movdqa [ar2+30h], xmm1
	movdqa [ar2+40h], xmm4

%ifdef WINDOWS64
	add			rsp, 80h
%endif
%ifdef SYSTEMV64
	add			rsp, 88h
%endif

%ifdef WINDOWS64
	; restore registers xmm6-xmm7
	movdqa		xmm6, [rsp+ 0h]
	movdqa		xmm7, [rsp+10h]
	movaps		xmm8, [rsp+20h]
	movaps		xmm9, [rsp+30h]
	movdqa		xmmA, [rsp+40h]
	movdqa		xmmB, [rsp+50h]
	movaps		xmmC, [rsp+60h]
	movaps		xmmD, [rsp+70h]
	movdqa		xmmE, [rsp+80h]
	add			rsp, 98h
%endif
	ret


jpgr_upsamplerowASM:
	lea			rax, [.upsample]
	shl			ar3, 4
	lea 		rax, [rax+ar3]
	movdqa		xmm0, [ar1]
	jmp rax

align 16
.upsample:
	; mode 0
	movdqa		[ar2], xmm0
	ret
	align 16, db 0xcc

	; mode 1
	punpcklwd	xmm0, xmm0
	movdqa		[ar2], xmm0
	ret
	align 16, db 0xcc

	; mode 2
	punpckhwd	xmm0, xmm0
	movdqa		[ar2], xmm0
	ret
	align 16, db 0xcc

	; mode 3
	punpcklwd	xmm0, xmm0
	punpcklwd	xmm0, xmm0
	movdqa		[ar2], xmm0
	ret
	align 16, db 0xcc

	; mode 4
	punpcklwd	xmm0, xmm0
	punpckhwd	xmm0, xmm0
	movdqa		[ar2], xmm0
	ret
	align 16, db 0xcc

	; mode 5
	punpckhwd	xmm0, xmm0
	punpcklwd	xmm0, xmm0
	movdqa		[ar2], xmm0
	ret
	align 16, db 0xcc

	; mode 6
	punpckhwd	xmm0, xmm0
	punpckhwd	xmm0, xmm0
	movdqa		[ar2], xmm0
	ret


align 16
c128:
	dw 8 dup (128)

jpgr_setrow1ASM:
	movdqa		xmm0, [ar1]
	paddw		xmm0, [c128]
	packuswb	xmm0, xmm0
	movq		[ar2], xmm0
	ret


; Constants
fixed_1_402 equ 5743
fixed_0_344 equ 1410
fixed_0_714 equ 2925
fixed_1_772 equ 7258

align 16
c1: dw 4 dup (        4096,  fixed_1_402)
c2: dw 4 dup (-fixed_0_344, -fixed_0_714)
c3: dw 4 dup (        4096,  fixed_1_772)
c4: dd 8 dup (526336)

jpgr_setrow3ASM:
	mov			rax, qword[hasSSSE3]
	test		rax, rax
	jz .doinit

	cmp			rax, 10001h
	je ssse3_setrow3

	movdqa		xmm1, [ar1]
	movdqa		xmm2, [ar2]
	movdqa		xmm3, [ar3]

%ifdef WINDOWS64
	mov			r8, qword[rsp+8*5]
%endif
	cmp			r8, 0h
	je .notransform

%ifdef WINDOWS64
	; preserve xmm6, xmm7, xmm8, and xmm9
	sub			rsp, 48h
	movaps		[rsp+ 0h], xmm6
	movaps		[rsp+10h], xmm7
	movaps		[rsp+20h], xmm8
	movaps		[rsp+30h], xmm9
%endif

	; r = y + cr * 1.402
	; implemented as
	; y * scale + cr * 1.402
	movdqa		xmm4, xmm1
	movdqa		xmm5, xmm1
	punpckhwd	xmm4, xmm3
	punpcklwd	xmm5, xmm3
	movdqa		xmm0, [c1]
	pmaddwd		xmm4, xmm0
	pmaddwd		xmm5, xmm0

	; b = y + cb * 1.177
	; implemented as
	; y * scale + cb * 1.777
	movdqa		xmm6, xmm1
	movdqa		xmm7, xmm1
	punpckhwd	xmm6, xmm2
	punpcklwd	xmm7, xmm2
	movdqa		xmm0, [c3]
	pmaddwd		xmm6, xmm0
	pmaddwd		xmm7, xmm0

	; g = y + cb * -0.344 + cr * -0.714
	; implemented as
	; temp = cb * -0.344 + cr * -0.714
	; temp = temp + y
	movdqa		xmm8, xmm2
	movdqa		xmm9, xmm2
	punpckhwd	xmm8, xmm3
	punpcklwd	xmm9, xmm3
	movdqa		xmm0, [c2]
	pmaddwd		xmm8, xmm0
	pmaddwd		xmm9, xmm0

	movdqa		xmm0, [c4]

	; g
	paddd		xmm8, xmm0
	paddd		xmm9, xmm0
	psrad		xmm8, 12
	psrad		xmm9, 12
	packssdw	xmm9, xmm8
	paddw		xmm9, xmm1
	movdqa		xmm2, xmm9

	; r
	paddd		xmm4, xmm0
	paddd		xmm5, xmm0
	psrad		xmm4, 12
	psrad		xmm5, 12
	packssdw	xmm5, xmm4
	movdqa		xmm1, xmm5

	; b
	paddd		xmm6, xmm0
	paddd		xmm7, xmm0
	psrad		xmm6, 12
	psrad		xmm7, 12
	packssdw	xmm7, xmm6
	movdqa		xmm3, xmm7

%ifdef WINDOWS64
	; restore registers
	movaps		xmm6, [rsp+ 0h]
	movaps		xmm7, [rsp+10h]
	movaps		xmm8, [rsp+20h]
	movaps		xmm9, [rsp+30h]
	add			rsp, 48h
%endif

	packuswb	xmm1, xmm1
	packuswb	xmm2, xmm2
	packuswb	xmm3, xmm3

.setpels:
	sub			rsp, 8
	movq		[rsp], xmm1

	%assign counter 0
	%rep 4
		mov			al, byte[rsp+(counter*2+0)]
		mov			dl, byte[rsp+(counter*2+1)]
		mov			byte[ar4+(counter*2+0)*3+0], al
		mov			byte[ar4+(counter*2+1)*3+0], dl
		%assign counter counter+1
	%endrep

	movq		[rsp], xmm2
	%assign counter 0
	%rep 4
		mov			al, byte[rsp+(counter*2+0)]
		mov			dl, byte[rsp+(counter*2+1)]
		mov			byte[ar4+(counter*2+0)*3+1], al
		mov			byte[ar4+(counter*2+1)*3+1], dl
		%assign counter counter+1
	%endrep

	movq		[rsp], xmm3
	%assign counter 0
	%rep 4
		mov			al, byte[rsp+(counter*2+0)]
		mov			dl, byte[rsp+(counter*2+1)]
		mov			byte[ar4+(counter*2+0)*3+2], al
		mov			byte[ar4+(counter*2+1)*3+2], dl
		%assign counter counter+1
	%endrep

	add			rsp, 8
	ret

.notransform:
	movdqa		xmm0, [c128]
	paddw		xmm1, xmm0
	paddw		xmm2, xmm0
	paddw		xmm3, xmm0
	packuswb	xmm1, xmm1
	packuswb	xmm2, xmm2
	packuswb	xmm3, xmm3

	jmp .setpels

.doinit:
	lea			rax, [jpgr_setrow3ASM]
	jmp init


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; SSSE3 version
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

; Suffle tables
align 16
suffle1:
	.1: db  0, -1, -1,  1, -1, -1,  2, -1, -1,  3, -1, -1,  4, -1, -1,  5
	.2: db -1, -1,  6, -1, -1,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1

suffle2:
	.1: db -1,  0, -1, -1,  1, -1, -1,  2, -1, -1,  3, -1, -1,  4, -1, -1
	.2: db  5, -1, -1,  6, -1, -1,  7, -1, -1, -1, -1, -1, -1, -1, -1, -1

suffle3:
	.1: db -1, -1,  0, -1, -1,  1, -1, -1,  2, -1, -1,  3, -1, -1,  4, -1
	.2: db -1,  5, -1, -1,  6, -1, -1,  7, -1, -1, -1, -1, -1, -1, -1, -1


ssse3_setrow3:
	movdqa		xmm1, [ar1]
	movdqa		xmm2, [ar2]
	movdqa		xmm3, [ar3]

%ifdef WINDOWS64
	mov			r8, qword[rsp+8*5]
%endif

	cmp			r8, 0h
	je .notransform

%ifdef WINDOWS64
	; preserve xmm7, xmm8, and xmm9
	sub			rsp, 38h
	movaps		[rsp+ 0h], xmm7
	movaps		[rsp+10h], xmm8
	movaps		[rsp+20h], xmm9
%endif

	; r = y + cr * 1.402
	; implemented as
	; y * scale + cr * 1.402
	movdqa		xmm4, xmm1
	movdqa		xmm5, xmm1
	punpckhwd	xmm4, xmm3
	punpcklwd	xmm5, xmm3
	movdqa		xmm0, [c1]
	pmaddwd		xmm4, xmm0
	pmaddwd		xmm5, xmm0

	; b = y + cb * 1.177
	; implemented as
	; y * scale + cb * 1.777
	movdqa		xmm6, xmm1
	movdqa		xmm7, xmm1
	punpckhwd	xmm6, xmm2
	punpcklwd	xmm7, xmm2
	movdqa		xmm0, [c3]
	pmaddwd		xmm6, xmm0
	pmaddwd		xmm7, xmm0

	; g = y + cb * -0.344 + cr * -0.714
	; implemented as
	; temp = cb * -0.344 + cr * -0.714
	; temp = temp + y
	movdqa		xmm8, xmm2
	movdqa		xmm9, xmm2
	punpckhwd	xmm8, xmm3
	punpcklwd	xmm9, xmm3
	movdqa		xmm0, [c2]
	pmaddwd		xmm8, xmm0
	pmaddwd		xmm9, xmm0

	movdqa		xmm0, [c4]

	; g
	paddd		xmm8, xmm0
	paddd		xmm9, xmm0
	psrad		xmm8, 12
	psrad		xmm9, 12
	packssdw	xmm9, xmm8
	paddw		xmm9, xmm1
	movdqa		xmm2, xmm9

	; r
	paddd		xmm4, xmm0
	paddd		xmm5, xmm0
	psrad		xmm4, 12
	psrad		xmm5, 12
	packssdw	xmm5, xmm4
	movdqa		xmm1, xmm5

	; b
	paddd		xmm6, xmm0
	paddd		xmm7, xmm0
	psrad		xmm6, 12
	psrad		xmm7, 12
	packssdw	xmm7, xmm6
	movdqa		xmm3, xmm7

%ifdef WINDOWS64
	; restore registers
	movaps		xmm7, [rsp+ 0h]
	movaps		xmm8, [rsp+10h]
	movaps		xmm9, [rsp+20h]
	add			rsp, 38h
%endif

	packuswb	xmm1, xmm1
	packuswb	xmm2, xmm2
	packuswb	xmm3, xmm3

.setpels:
%ifdef WINDOWS64
	; preserve xmm6
	sub			rsp, 18h
	movaps		[rsp], xmm6
%endif

	; ssse3 ssufle_epi8
	movdqa		xmm0, [suffle1.1]
	movdqa		xmm4, xmm1
	pshufb		xmm4, xmm0

	movdqa		xmm0, [suffle2.1]
	movdqa		xmm5, xmm2
	pshufb		xmm5, xmm0

	movdqa		xmm0, [suffle3.1]
	movdqa		xmm6, xmm3
	pshufb		xmm6, xmm0

	por			xmm6, xmm5
	por			xmm6, xmm4
	movdqu		[ar4], xmm6

	; remaining pixels
	movdqa		xmm0, [suffle1.2]
	movdqa		xmm4, xmm1
	pshufb		xmm4, xmm0

	movdqa		xmm0, [suffle2.2]
	movdqa		xmm5, xmm2
	pshufb		xmm5, xmm0

	movdqa		xmm0, [suffle3.2]
	movdqa		xmm6, xmm3
	pshufb		xmm6, xmm0

	por			xmm6, xmm5
	por			xmm6, xmm4
	movq		[ar4+10h], xmm6

%ifdef WINDOWS64
	; restore xmm6
	movaps		xmm6, [rsp]
	add			rsp, 18h
%endif
	ret

.notransform:
	movdqa		xmm0, [c128]
	paddw		xmm1, xmm0
	paddw		xmm2, xmm0
	paddw		xmm3, xmm0
	packuswb	xmm1, xmm1
	packuswb	xmm2, xmm2
	packuswb	xmm3, xmm3

	jmp .setpels


section .data
align 16


hasSSSE3:
	dw 0h
	.initdone:
		dw 0h
