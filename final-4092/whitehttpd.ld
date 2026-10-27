ENTRY(__start)
PHDRS { text PT_LOAD FILEHDR PHDRS FLAGS(5); bss PT_LOAD FLAGS(6); stack PT_GNU_STACK FLAGS(6); }
SECTIONS {
  . = 0x00400000 + SIZEOF_HEADERS;
  .text : { *(.text) *(.sdata) } :text
  . = ALIGN(0x1000);
  _gp = . - 0x1000;
  .sbss : { *(.sbss) *(.bss) } :bss
  /DISCARD/ : { *(*) }
}
