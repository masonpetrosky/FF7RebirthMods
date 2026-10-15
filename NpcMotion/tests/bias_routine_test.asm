; Enters the texture bias routine the way the game does: rbx = the view, xmm1/xmm0/xmm6/xmm8 = the four
; View.TemporalSamplerBias values. The routine jumps back to BiasRoutineReturn.
; SPDX-License-Identifier: MIT

.code

; uint64_t RunBiasRoutine(const void* routine, void* view, const float* values, float* registers_after)
; values: x, y, z, w. registers_after: xmm6 and xmm8 after the routine. Returns the flags that changed (0 = none).
RunBiasRoutine PROC
    push rbx
    push rsi
    push rdi
    sub rsp, 40h
    movups xmmword ptr [rsp], xmm6
    movups xmmword ptr [rsp + 10h], xmm8
    mov rbx, rdx
    mov rsi, r9
    movss xmm1, dword ptr [r8]
    movss xmm0, dword ptr [r8 + 4]
    movss xmm6, dword ptr [r8 + 8]
    movss xmm8, dword ptr [r8 + 12]
    xor eax, eax
    cmp eax, 1                          ; known flags (carry and sign set)
    pushfq
    pop rdi
    jmp rcx
BiasRoutineReturn::
    pushfq
    pop rax
    xor rax, rdi
    movss dword ptr [rsi], xmm6
    movss dword ptr [rsi + 4], xmm8
    movups xmm6, xmmword ptr [rsp]
    movups xmm8, xmmword ptr [rsp + 10h]
    add rsp, 40h
    pop rdi
    pop rsi
    pop rbx
    ret
RunBiasRoutine ENDP

; const void* BiasRoutineReturnAddress()
BiasRoutineReturnAddress PROC
    lea rax, BiasRoutineReturn
    ret
BiasRoutineReturnAddress ENDP

END
