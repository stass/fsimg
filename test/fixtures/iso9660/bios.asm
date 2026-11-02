; SPDX-License-Identifier: BSD-2-Clause
; Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
;
; A no-emulation El Torito image. Initialize COM1, print a serial marker,
; then exit the test VM through QEMU's explicitly configured debug port.
bits 16
org 0x7c00

    cli
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov sp, 0x7c00
    cld
    mov dx, 0x3fb
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8
    mov al, 1
    out dx, al
    inc dx
    xor al, al
    out dx, al
    mov dx, 0x3fb
    mov al, 3
    out dx, al
    mov si, marker
.next:
    lodsb
    test al, al
    jz .done
    mov bl, al
.ready:
    mov dx, 0x3fd
    in al, dx
    test al, 0x20
    jz .ready
    mov al, bl
    mov dx, 0x3f8
    out dx, al
    jmp .next
.done:
    mov dx, 0xf4
    mov al, 0x10
    out dx, al
    hlt
    jmp .done

marker: db 'FSIMG_BIOS_OK', 10, 0
times 510 - ($ - $$) db 0
dw 0xaa55
times 2048 - ($ - $$) db 0
