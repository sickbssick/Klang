OUTPUT_FORMAT("elf64-x86-64")
ENTRY(efi_main)

SECTIONS
{
    . = 0x0;
    .pe_header : {
        *(.pe_header)
    }
    . = ALIGN(0x1000);
    __headers_size = .;

    __text_rva = .;
    __entry_rva = efi_main;
    .text : {
        *(.text*)
    }
    .rodata : ALIGN(16) {
        *(.rodata*)
    }
    .data : ALIGN(16) {
        *(.data*)
    }
    .bss : ALIGN(16) {
        *(COMMON)
        *(.bss*)
    }
    __text_vsize = . - __text_rva;
    . = ALIGN(0x1000);
    __text_raw_size = . - __text_rva;

    __reloc_rva = .;
    .reloc : {
        *(.reloc)
        __reloc_end = .;
        . = ALIGN(0x1000);
    }
    __reloc_size = __reloc_end - __reloc_rva;
    __reloc_raw_size = . - __reloc_rva;

    __image_size = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
