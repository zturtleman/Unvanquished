; ===========================================================================
; Copyright (C) 2011 Thilo Schulz <thilo@tjps.eu>
; 
; This file is part of Daemon source code.
; 
; Daemon source code is free software; you can redistribute it
; and/or modify it under the terms of the GNU General Public License as
; published by the Free Software Foundation; either version 2 of the License,
; or (at your option) any later version.
; 
; Daemon source code is distributed in the hope that it will be
; useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
; 
; You should have received a copy of the GNU General Public License
; along with Daemon source code; if not, write to the Free Software
; Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
; ===========================================================================

; the MASM version of the snapvector functions using SSE or FPU
; assumes that the cdecl calling convention is being used on x86, and the fastcall one on x64
;
; function prototype:
; void qsnapvector(vec3_t vec)

IFNDEF idx64
.model flat, c
ENDIF

.data

  ALIGN 16
  ssemask DWORD 0FFFFFFFFh, 0FFFFFFFFh, 0FFFFFFFFh, 00000000h
  ssecw DWORD 00001F80h

IFNDEF idx64
  fpucw WORD 037Fh
ENDIF

.code

IFDEF idx64
; qsnapvector using SSE

  qsnapvectorsse PROC
    sub rsp, 8
	movaps xmm1, ssemask		; initialize the mask register for maskmovdqu
	movups xmm0, [rcx]			; our vector is stored here; read 4 values at once
	movaps xmm2, xmm0			; keep a copy of the original data
	andps xmm0, xmm1			; set the fourth value to zero in xmm0
	andnps xmm1, xmm2			; copy the fourth value to xmm1 and set the rest to zero
	cvtps2dq xmm0, xmm0			; convert 4 floats to ints
	cvtdq2ps xmm0, xmm0			; convert 4 ints to floats
	orps xmm0, xmm1				; combine all 4 values again
	movups [rcx], xmm0			; write 3 rounded and 1 unchanged values back to the memory
	ret
  qsnapvectorsse ENDP

ELSE

  qsnapvectorsse PROC
	mov eax, dword ptr 4[esp]	; store the address of the vector in eax
	movaps xmm1, ssemask		; initialize the mask register for maskmovdqu
	movups xmm0, [eax]			; our vector is stored here; read 4 values at once
	movaps xmm2, xmm0			; keep a copy of the original data
	andps xmm0, xmm1			; set the fourth value to zero in xmm0
	andnps xmm1, xmm2			; copy the fourth value to xmm1 and set the rest to zero
	cvtps2dq xmm0, xmm0			; convert 4 floats to ints
	cvtdq2ps xmm0, xmm0			; convert 4 ints to floats
	orps xmm0, xmm1				; combine all 4 values again
	movups [eax], xmm0			; write 3 rounded and 1 unchanged values back to the memory
	ret
  qsnapvectorsse ENDP

  qroundx87 macro src
	fld dword ptr src
	fistp dword ptr src
	fild dword ptr src
	fstp dword ptr src
  endm

  qsnapvectorx87 PROC
	mov eax, dword ptr 4[esp]
	qroundx87 [eax]
	qroundx87 4[eax]
	qroundx87 8[eax]
	ret
  qsnapvectorx87 ENDP

ENDIF

end