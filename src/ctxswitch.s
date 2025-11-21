; ctxswitch.s - ca65 assembly context switch for RP6502
; This implements a stack-copying, stackful cooperative context switch.
; Expects two global C variables (absolute addresses):
;   ctx_cur  -> pointer to current task_t
;   ctx_next -> pointer to next task_t
; It copies the entire hardware stack page ($0100-$01FF) into cur->stack and
; copies next->stack back into memory, restores SP and saved registers.

    .segment "ZEROPAGE"
; zero page pointers / temps
ctx_task_ptr:
    .res 2      ; pointer to current/next task struct base
tmp_a:
    .res 4
tmp_x:
    .res 1
tmp_y:
    .res 1
stack_ptr:
    .res 2      ; pointer to stack buffer inside task struct

    .segment "CODE"
    .global _ctx_switch
    .import _ctx_cur
    .import _ctx_next

; Offsets in task_t (must match C definition in scheduler.c)
; fn: 0..1, arg:2..3, in_use:4, one_shot:5, started:6, saved_sp:7,
; a:8, x:9, y:10, p:11, stack:12..(12+SCHED_TASK_STACK_SIZE-1)
FN_OFF = 0
ARG_OFF = 2
INUSE_OFF = 4
ONESHOT_OFF = 5
STARTED_OFF = 6
SAVEDSP_OFF = 7
A_OFF = 8
X_OFF = 9
Y_OFF = 10
P_OFF = 11
WAKE_OFF = 12
STACK_OFF = 14

; ctx_switch: perform swap between *ctx_cur and *ctx_next
_ctx_switch:
    ; load _ctx_cur pointer into ctx_task_ptr (low/high)
    lda _ctx_cur
    sta ctx_task_ptr
    lda _ctx_cur+1
    sta ctx_task_ptr+1

    ; save registers into tmp zp
    stx tmp_x
    sty tmp_y
    sta tmp_a
    php
    pla         ; A <- pushed P
    sta tmp_a+1 ; reuse tmp slot for P (tmp_a+1)
    lda tmp_a   ; restore original A

    ; store regs into current task struct
    ldy #A_OFF
    lda tmp_a
    sta (ctx_task_ptr),y
    ldy #X_OFF
    lda tmp_x
    sta (ctx_task_ptr),y
    ldy #Y_OFF
    lda tmp_y
    sta (ctx_task_ptr),y
    ldy #P_OFF
    lda tmp_a+1
    sta (ctx_task_ptr),y

    ; save SP into task.saved_sp
    tsx
    ldy #SAVEDSP_OFF
    txa
    sta (ctx_task_ptr),y

    ; call C helper to copy stacks: scheduler_copy_stacks()
    .import _scheduler_copy_stacks
    jsr _scheduler_copy_stacks

    ; set ctx_task_ptr to point to _ctx_next so we can read next.saved_sp
    lda _ctx_next
    sta ctx_task_ptr
    lda _ctx_next+1
    sta ctx_task_ptr+1

    ; restore SP from next.saved_sp
    ldy #SAVEDSP_OFF
    lda (ctx_task_ptr),y
    tax
    txs

    ; restore registers A/X/Y/P from next task struct
    ldy #P_OFF
    lda (ctx_task_ptr),y
    pha
    plp         ; restore P

    ldy #A_OFF
    lda (ctx_task_ptr),y
    ; A restored
    ldy #X_OFF
    lda (ctx_task_ptr),y
    tax
    ldy #Y_OFF
    lda (ctx_task_ptr),y
    tay

    ; check started flag in next task
    ldy #STARTED_OFF
    lda (ctx_task_ptr),y
    cmp #0
    bne resume_task

    ; mark started = 1
    lda #1
    sta (ctx_task_ptr),y
    ; Jump to trampoline that will call the C function pointer
    jmp start_trampoline

resume_task:
    ; resumed task: return to saved PC by RTS
    rts

; trampoline: call the C function pointer stored in ctx_next->fn with ctx_next->arg
; when function returns, call scheduler_task_return (C) which will not return
    .import _scheduler_task_return
    .import _scheduler_start_task
start_trampoline:
    ; ctx_task_ptr currently points to next task struct
    ; Simply call into C helper which will call the task function (fn,arg)
    jsr _scheduler_start_task
    ; scheduler_start_task should not return; if it does, just RTS
    rts
