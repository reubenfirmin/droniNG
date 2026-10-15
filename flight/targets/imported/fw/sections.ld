/*
    ChibiOS/RT - Copyright (C) 2006,2007,2008,2009,2010,
                 2011,2012,2013 Giovanni Di Sirio.

    This file is part of ChibiOS/RT.

    ChibiOS/RT is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    ChibiOS/RT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.

    A special exception to the GPL can be applied should you wish to distribute
    a combined work that includes ChibiOS/RT, without being obliged to provide
    the source code for any proprietary components. See the file exception.txt
    for full details of how and when the exception can be applied.
*/
/* Derived F4 direct-boot layout: vectors occupy sector 0; settings and
 * autotune are never load regions. */
ENTRY(ResetHandler)
__ram_start__ = ORIGIN(ram);
__ram_size__ = LENGTH(ram);
__ram_end__ = ORIGIN(ram) + LENGTH(ram);
SECTIONS {
    startup : ALIGN(16) {
        PROVIDE(pios_isr_vector_table_base = .);
        KEEP(*(vectors))
    } > vectors
    constructors : ALIGN(4) {
        PROVIDE(__init_array_start = .);
        KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array))
        PROVIDE(__init_array_end = .);
    } > flash
    destructors : ALIGN(4) {
        PROVIDE(__fini_array_start = .);
        KEEP(*(.fini_array)) KEEP(*(SORT(.fini_array.*)))
        PROVIDE(__fini_array_end = .);
    } > flash
    .text : ALIGN(16) {
        _text = .;
        *(.text.startup.*) *(.text) *(.text.*) *(.rodata) *(.rodata.*)
        *(.glue_7t) *(.glue_7) *(.gcc*)
    } > flash
    .initcallmodule.init : ALIGN(4) {
        __module_initcall_start = .;
        KEEP(*(SORT_BY_NAME(.initcall.*)))
        __module_initcall_end = .;
    } > flash
    .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > flash
    .ARM.exidx : {
        PROVIDE(__exidx_start = .);
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        PROVIDE(__exidx_end = .);
    } > flash
    .textalign : { . = ALIGN(8); _etext = .; } > flash
    .boardinfo : ALIGN(4) {
        KEEP(*(.boardinfo))
        KEEP(*(.boardprofile))
    } > bd_info
    .stacks (NOLOAD) : ALIGN(8) {
        __main_stack_base__ = .;
        . += __main_stack_size__;
        __main_stack_end__ = .;
        __process_stack_base__ = .;
        __main_thread_stack_base__ = .;
        . += __process_stack_size__;
        __process_stack_end__ = .;
        __main_thread_stack_end__ = .;
    } > ram
    .data : ALIGN(4) {
        PROVIDE(_data = .);
        *(.data) *(.data.*) *(.ramtext)
        . = ALIGN(4);
        PROVIDE(_edata = .);
    } > ram AT > flash
    _textdata = LOADADDR(.data);
    .bss (NOLOAD) : ALIGN(4) {
        PROVIDE(_bss_start = .);
        *(SORT_BY_ALIGNMENT(.bss*)) *(COMMON)
        . = ALIGN(4);
        PROVIDE(_bss_end = .);
    } > ram
    .heap (NOLOAD) : ALIGN(8) {
        _sheap = .;
        *(.heap)
        . = ORIGIN(ram) + LENGTH(ram) - ABSOLUTE(_sheap);
        _eheap = .;
    } > ram
}
PROVIDE(end = .);
_end = .;
__heap_base__ = _sheap;
__heap_end__ = __ram_end__;
_cmm_start = _end;
_cmm_end = _end;
ASSERT(SIZEOF(startup) <= LENGTH(vectors), "Imported vectors exceed sector 0")
ASSERT(SIZEOF(.boardinfo) <= LENGTH(bd_info), "Imported identity exceeds sector 3")
ASSERT(__heap_end__ - __heap_base__ >= 48k, "Imported runtime heap budget below 48 KiB")
