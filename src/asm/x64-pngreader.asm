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


section .text
align 16


; In assertions:
; 1) the current row and prev row have 16 extra padding bytes to do SIMD
;    or loop unrolling
; 2) filter is non zero, "only valid values" are 1, 2, 3, 4
; 3) pixel size is 1, 2, 3, 4, 6 or 8
; if any assertion is not meet the program may crash

global pngr_unfilterASM
; Parameters:
; (pointer) current row, (pointer) prev row, row size, filter << 16 | pel size


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;  Initialize the jump table according to the CPU capabilities
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

SSE4_FLAG equ 0180000h  ; sse4.1 | sse4.2


initjump:
	xor			rax, rax
	cmp			rax, qword[initdone]
	jne .done

	; preserve registers
	push		rcx
	push		rdx
	push		rbx

	mov			eax, 1
	cpuid
	and			ecx, SSE4_FLAG

	; sse2
	lea			rax, [sse2_filter1]
	mov			qword[jumptable+1*8], rax
	lea			rax, [sse2_filter2]
	mov			qword[jumptable+2*8], rax
	lea			rax, [sse2_filter3]
	mov			qword[jumptable+3*8], rax
	lea			rax, [sse2_filter4]
	mov			qword[jumptable+4*8], rax

	test		ecx, ecx
	jz  .restore

	; sse4
	lea			rax, [sse4_filter4]
	mov			qword[jumptable+4*8], rax

.restore:
	pop			rbx
	pop			rdx
	pop			rcx

.done:
	lea			rax, [initdone]
	mov			qword[rax], 1h

	jmp pngr_unfilterASM.dojump


; systemv x64: rdi=row, rsi=prev row, rdx=row size, rcx=filter | pel size
; windows x64: rcx=row, rdx=prev row, r8 =row size, r9 =filter | pel size
pngr_unfilterASM:
%ifdef WINDOWS64
	push		rsi
	push		rdi

	mov 		rdi, rcx
	mov 		rsi, rdx
	mov			rcx, r9
	mov			rdx, r8
%endif
	push		rcx

	shr			rcx, 16
.dojump:
	lea			rax, [jumptable]
	jmp qword[rax+rcx*8]


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; Scalar version
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

filter1:
	pop			rcx
	and			rcx, 0xffff

.dofilter:
	; curr + rowsize
	add			rdx, rdi
	mov			rsi, rdi

	; curr + pelsize
	add			rdi, rcx

.loop1:
	cmp			rdi, rdx
	jnb .done

	; 4 times
	%assign counter 0
	%rep 4
		mov			al, byte[rsi+counter]
		add			byte[rdi+counter], al
		%assign counter counter+1
	%endrep

	add			rdi, 4
	add			rsi, 4
	jmp .loop1

.done:
%ifdef WINDOWS64
	pop			rdi
	pop			rsi
%endif
	ret


filter2:
	pop			rcx

	; curr + rowsize
	add			rdx, rdi

.loop1:
	cmp			rdi, rdx
	jnb .done

	; 4 bytes at time
	mov			r10b, byte[rsi+0]
	mov			r11b, byte[rsi+1]
	add			byte[rdi+0], r10b
	add			byte[rdi+1], r11b

	mov			r10b, byte[rsi+2]
	mov			r11b, byte[rsi+3]
	add			byte[rdi+2], r10b
	add			byte[rdi+3], r11b

	add			rdi, 4
	add			rsi, 4
	jmp .loop1

.done:
%ifdef WINDOWS64
	pop			rdi
	pop			rsi
%endif
	ret


filter3:
	pop			rcx
	and			rcx, 0xffff

.dofilter:
	; curr + rowsize
	add			rdx, rdi
	; curr + pelsize
	add			rcx, rdi
	mov			rax, rdi

	; pelsize times
.loop1:
	cmp			rdi, rcx
	jnb .loop2

	movzx		r10d, byte[rsi]
	sar			r10d, 1h
	add			byte[rdi], r10b
	inc			rdi
	inc			rsi
	jmp .loop1

.loop2:
	cmp			rdi, rdx
	jnb .done

	movzx		r10d, byte[rax]  ; [curr - pelsize]
	movzx		r11d, byte[rsi]  ; [prev]
	add			r10d, r11d
	sar			r10d, 1h
	add			byte[rdi], r10b

	inc			rax
	inc			rdi
	inc			rsi
	jmp .loop2

.done:
%ifdef WINDOWS64
	pop			rdi
	pop			rsi
%endif
	ret


filter4:
	pop			rcx
	and			rcx, 0xffff

.dofilter:
	; curr + rowsize
	add			rdx, rdi
	; curr + pelsize
	add			rcx, rdi

	push		rbx
	push		r12
	push		r13
	push		r14
	push		r15
	mov			rax, rdi  ; becomes [curr - pelsize] after the first loop
	mov			rbx, rsi  ; becomes [prev - pelsize] after the first loop

.loop1:
	cmp			rdi, rcx
	jnb .loop2

	mov			r10b, byte[rsi]
	add			byte[rdi], r10b
	inc			rsi
	inc			rdi
	jmp .loop1

.loop2:
	cmp			rdi, rdx
	jnb .done

	movzx		r10, byte[rax]  ; a
	movzx		r11, byte[rsi]  ; b
	movzx		r12, byte[rbx]  ; c

	; p
	mov			r8d, r11d
	add			r8d, r10d
	sub			r8d, r12d

	; r13 := pa; r14 := pb; r15 := pc
	mov			r13d, r8d
	sub			r13d, r10d
	mov			 r9d, r13d
	sar			 r9d, 15
	xor			r13d, r9d
	sub			r13d, r9d

	mov			r14d, r8d
	sub			r14d, r11d
	mov			 r9d, r14d
	sar			 r9d, 15
	xor			r14d, r9d
	sub			r14d, r9d

	mov			r15d, r8d
	sub			r15d, r12d
	mov			 r9d, r15d
	sar			 r9d, 15
	xor			r15d, r9d
	sub			r15d, r9d

	; result is on r10 (a)
	mov			r8, r14
	mov			r9, r15
	sub			r8, r13
	sub			r9, r13

	inc			rsi
	inc			rbx
	or			r8d, r9d
	jns .pickdone

	; pick b
	mov			r10d, r11d
	cmp			r14d, r15d
	cmova		r10d, r12d

.pickdone:
	add			byte[rdi], r10b
	inc			rax
	inc			rdi
	jmp .loop2

.done:
	pop r15
	pop r14
	pop r13
	pop r12
	pop rbx
%ifdef WINDOWS64
	pop			rdi
	pop			rsi
%endif
	ret


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; SSE2 version
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

sse2_filter1:
	pop			rcx
	and			rcx, 0xffff

	; use the scalar version is pelsize is <= 2
	cmp			rcx, 2
	jbe filter1.dofilter

	; curr + rowsize
	add			rdx, rdi

	pxor		xmm0, xmm0  ; zero
	pxor		xmm1, xmm1  ; [curr - pelsize]

	; calculate the jump to set the pixel
	mov 		rax, 8
	sub			rax, rcx
	lea			r10, [.setpel]
	lea			r10, [r10+rax*8]

.loop1:
	cmp			rdi, rdx
	jnb .done

	movdqu		xmm2, [rdi]
	punpcklbw	xmm2, xmm0

	paddw		xmm2, xmm1
	movdqa		xmm1, xmm2

	movdqu		[rsp-16], xmm2
	jmp r10

.setpel:
	%assign counter 7
	%rep 8
		mov			ax, word[rsp-16+counter*2]
		mov			byte[rdi+counter], al
		%assign counter counter-1
	%endrep
	nop

	add			rdi, rcx
	jmp .loop1

.done:
%ifdef WINDOWS64
	pop			rdi
	pop			rsi
%endif
	ret


sse2_filter2:
	pop			rcx

	; curr + rowsize
	add			rdx, rdi

.loop1:
	cmp			rdi, rdx
	jnb .done

	movdqu		xmm0, [rdi]
	movdqu		xmm1, [rsi]
	paddb		xmm0, xmm1
	movdqu		[rdi], xmm0

	add			rdi, 10h
	add			rsi, 10h
	jmp .loop1

.done:
%ifdef WINDOWS64
	pop			rdi
	pop			rsi
%endif
	ret


align 16
andmask:
	db 16 dup(1)

sse2_filter3:
	pop			rcx
	and			rcx, 0xffff

	; use the scalar version is pelsize is <= 2
	cmp			rcx, 2
	jbe filter3.dofilter

	; curr + rowsize
	add			rdx, rdi

	; calculate the jump to set the pixel
	mov 		rax, 8
	sub			rax, rcx
	lea			r10, [.setpel]
	lea			r10, [r10+rax*8]

	pxor		xmm0, xmm0  ; zero
	pxor		xmm1, xmm1
	movdqa		xmm6, [andmask]

.loop1:
	cmp			rdi, rdx
	jnb .done

	; xmm1 := [curr]; xmm2 := a; xmm3 := b;
	movdqa		xmm2, xmm1
	movdqu		xmm1, [rdi]
	movdqu		xmm3, [rsi]

	movdqa		xmm4, xmm2
	movdqa		xmm5, xmm2
	pavgb		xmm4, xmm3
	pxor		xmm5, xmm3
	pand		xmm5, xmm6
	psubb		xmm4, xmm5
	paddb		xmm1, xmm4
	movdqa		xmm2, xmm1
	punpcklbw	xmm2, xmm0

	movdqu		[rsp-16], xmm2
	jmp r10

.setpel:
	%assign counter 7
	%rep 8
		mov			ax, word[rsp-16+counter*2]
		mov			byte[rdi+counter], al
		%assign counter counter-1
	%endrep
	nop

	add			rdi, rcx
	add			rsi, rcx
	jmp .loop1

.done:
%ifdef WINDOWS64
	pop			rdi
	pop			rsi
%endif
	ret


sse2_filter4:
	pop			rcx
	and			rcx, 0xffff

	; use the scalar version is pelsize is == 1
	cmp			rcx, 1
	je filter4.dofilter

	; curr + rowsize
	add			rdx, rdi

	; calculate the jump to set the pixel
	mov 		rax, 8
	sub			rax, rcx
	lea			r10, [.setpel]
	lea			r10, [r10+rax*8]

	pxor		xmm1, xmm1 ; a
	pxor		xmm2, xmm2 ; b
	pxor		xmm3, xmm3 ; c
	pxor		xmm4, xmm4 ; x
	pxor		xmm5, xmm5 ; zero

.loop1:
	cmp			rdi, rdx
	jnb .done

	movdqa		xmm3, xmm2   ; c = b
	movdqu		xmm2, [rsi]  ; b = prev
	movdqa		xmm1, xmm4   ; a = x
	movdqu		xmm4, [rdi]  ; x = curr
	punpcklbw	xmm2, xmm5
	punpcklbw	xmm4, xmm5

	; pa = b - c
	movdqa		xmm6, xmm2
	psubw		xmm6, xmm3
	; pb = a - c
	movdqa		xmm7, xmm1
	psubw		xmm7, xmm3
	; pc = pb + pb
	movdqa		xmm8, xmm7
	paddw		xmm8, xmm6

	; abs(pa)
	movdqa		xmm9, xmm6
	psraw		xmm9, 15
	pxor		xmm6, xmm9
	psubw		xmm6, xmm9

	; abs(pb)
	movdqa		xmm9, xmm7
	psraw		xmm9, 15
	pxor		xmm7, xmm9
	psubw		xmm7, xmm9

	; abs(pc)
	movdqa		xmm9, xmm8
	psraw		xmm9, 15
	pxor		xmm8, xmm9
	psubw		xmm8, xmm9

	; xmm9 = min(pc, min(pa, pb))
	movdqa		xmm9, xmm6
	pminsw		xmm9, xmm7 ; min(pa, pb)
	pminsw		xmm9, xmm8 ; min(min(pa, pb), pc) := sm

	pcmpeqw		xmm6, xmm9  ; min(pa, min(pc, min(pa, pb))); mask1
	pcmpeqw		xmm7, xmm9  ; min(pb, min(pc, min(pa, pb))); mask2

	movdqa		xmm9, xmm7  ; andnot(min(pb..), c) := 1

	pandn		xmm9, xmm3  ; andnot(mask1, c)
	pand		xmm7, xmm2  ; and(mask1, b)
	por			xmm9, xmm7  ; or (1, 2) := v

	movdqa		xmm0, xmm6
	pandn		xmm0, xmm9 ; andnot(min(pa..), v)   := 3
	pand		xmm6, xmm1 ; and(min(pa..), a)      := 4
	por			xmm0, xmm6 ; or(3, 4)

	movdqa		xmm9, xmm0
	paddb		xmm4, xmm9

	movdqu		[rsp-16], xmm4
	jmp r10

.setpel:
	%assign counter 7
	%rep 8
		mov			ax, word[rsp-16+counter*2]
		mov			byte[rdi+counter], al
		%assign counter counter-1
	%endrep
	nop

	add			rdi, rcx
	add			rsi, rcx
	jmp .loop1

.done:
%ifdef WINDOWS64
	pop			rdi
	pop			rsi
%endif
	ret


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; SSE4 version
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

sse4_filter4:
	pop			rcx
	and			rcx, 0xffff

	; use the scalar version is pelsize is == 1
	cmp			rcx, 1
	je filter4.dofilter

	; curr + rowsize
	add			rdx, rdi

	; calculate the jump to set the pixel
	mov 		rax, 8
	sub			rax, rcx
	lea			r10, [.setpel]
	lea			r10, [r10+rax*8]

	pxor		xmm1, xmm1 ; a
	pxor		xmm2, xmm2 ; b
	pxor		xmm3, xmm3 ; c
	pxor		xmm4, xmm4 ; x
	pxor		xmm5, xmm5 ; zero

.loop1:
	cmp			rdi, rdx
	jnb .done

	movdqa		xmm3, xmm2   ; c = b
	movdqu		xmm2, [rsi]  ; b = prev
	movdqa		xmm1, xmm4   ; a = x
	movdqu		xmm4, [rdi]  ; x = curr
	punpcklbw	xmm2, xmm5
	punpcklbw	xmm4, xmm5

	movdqa		xmm6, xmm2
	psubw		xmm6, xmm3  ; pa = b - c
	movdqa		xmm7, xmm1
	psubw		xmm7, xmm3  ; pb = a - c
	movdqa		xmm8, xmm7
	paddw		xmm8, xmm6  ; pc = pb + pb

	pabsw		xmm6, xmm6
	pabsw		xmm7, xmm7
	pabsw		xmm8, xmm8

	movdqa		xmm9, xmm6
	pminsw		xmm9, xmm7  ; min(pa, pb)
	pminsw		xmm9, xmm8  ; min(min(pa, pb), pc) := sm

	pcmpeqw		xmm6, xmm9  ; min(pa, min(pc, min(pa, pb))); mask1
	pcmpeqw		xmm7, xmm9  ; min(pb, min(pc, min(pa, pb))); mask2

	;
	movdqa		xmm0, xmm7
	movdqa		xmm9, xmm3
	pblendvb	xmm9, xmm2  ; blend(c, b) := 5

	movdqa		xmm0, xmm6
	pblendvb	xmm9, xmm1  ; blend(5, a)

	paddb		xmm4, xmm9

	movdqu		[rsp-16], xmm4
	jmp r10

.setpel:
	%assign counter 7
	%rep 8
		mov			ax, word[rsp-16+counter*2]
		mov			byte[rdi+counter], al
		%assign counter counter-1
	%endrep
	nop

	add			rdi, rcx
	add			rsi, rcx
	jmp .loop1

.done:
%ifdef WINDOWS64
	pop			rdi
	pop			rsi
%endif
	ret


dummy:
	pop			rcx
%ifdef WINDOWS64
	pop			rdi
	pop			rsi
%endif
	ret


section .data
align 16

jumptable:
	dq		dummy
	dq		initjump
	dq		initjump
	dq		initjump
	dq		initjump

initdone:
	dq		0h


