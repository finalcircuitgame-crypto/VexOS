ENTRY(kernel_main)

PHDRS {
    kernel PT_LOAD FLAGS(5); /* Read + execute */
    data   PT_LOAD FLAGS(6); /* Read + write */
    fonts  PT_LOAD FLAGS(4); /* Read-only embedded font data */
}

SECTIONS {
    /* Keep executable kernel code at its established low physical address. */
    . = 0x100000;

    .text : {
        *(.text)
        *(.text.*)
    } :kernel

    .rodata : {
        *(.rodata)
        *(.rodata.*)
        *(.eh_frame)
        *(.eh_frame.*)
    } :kernel

    . = ALIGN(0x1000);
    .data : {
        *(.data)
        *(.data.*)
    } :data

    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    } :data

    /*
     * Large TTF files live in a separate read-only PT_LOAD segment. This
     * prevents their bytes from expanding the low-address executable segment
     * and lets UEFI reserve the font blob independently at 64 MiB.
     */
    . = 0x04000000;
    .font_assets : ALIGN(0x1000) {
        *(.font_assets)
    } :fonts
}
