ENTRY(_start)
PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}
SECTIONS
{
  /* Match the CI ring-3 fixture: keep user pages clear of supervisor identity mappings. */
  . = 0x0000002000000000;
  .text : ALIGN(0x1000) { *(.text.start) *(.text .text.*) } :text
  .rodata : ALIGN(0x1000) { *(.rodata .rodata.*) } :text
  .data : ALIGN(0x1000) { *(.data .data.*) *(.init_array .init_array.*) } :data
  .bss : ALIGN(0x1000) { *(COMMON) *(.bss .bss.*) } :data
  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
