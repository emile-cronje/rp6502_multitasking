; Vectors for rp6502 ROM - place reset vector at _init
; This defines the VECTORS segment expected by rp6502.cfg

.segment "VECTORS"
.org $FFFA
.word 0        ; NMI vector (unused)
.import _init
.word _init    ; RESET vector -> _init in main.s
.word 0        ; IRQ/BRK vector (unused)
