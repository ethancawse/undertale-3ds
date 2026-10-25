/* The layout of an AOT module (source/ctr_aot_module.h): linked to run at the start of the heap's address range,
   where __system_allocateHeaps reserves __aotm_region_size bytes (--defsym), with the main program's symbols from
   --just-symbols. The header comes first; code and read-only data fill whole pages, which the loader makes read-only
   and executable; the writable data follows. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(aotModuleHeader)

SECTIONS
{
    . = 0x08000000;
    .text : {
        KEEP(*(.aotm.header))
        *(.text .text.* .gnu.linkonce.t.*)
        *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
        *(.rodata .rodata.* .gnu.linkonce.r.*)
        . = ALIGN(0x1000);
    }
    __aotm_code_end = .;
    .data : {
        *(.data .data.* .gnu.linkonce.d.*)
        . = ALIGN(4);
    }
    __aotm_image_end = .;
    .bss (NOLOAD) : {
        __aotm_bss_start = .;
        *(.bss .bss.* .gnu.linkonce.b.* COMMON)
        . = ALIGN(4);
        __aotm_bss_end = .;
    }
    ASSERT(__aotm_bss_end - 0x08000000 <= __aotm_region_size,
           "AOT module larger than the region reserved for it: lower the budget or raise CTRPORT_AOT_REGION_MB")

    /DISCARD/ : {
        *(.ARM.exidx* .ARM.extab* .init_array* .fini_array* .ctors* .dtors* .comment .note.* .ARM.attributes)
    }
}
