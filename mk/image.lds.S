
#include <asm-generic/embox.lds.h>
#include <apps.lds.h>

#ifdef ARCH_TEXT_ALIGNMENT
# define TEXT_ALIGNMENT    ARCH_TEXT_ALIGNMENT
#else
# define TEXT_ALIGNMENT    DEFAULT_TEXT_ALIGNMENT
#endif

#ifdef ARCH_DATA_ALIGNMENT
# define DATA_ALIGNMENT    ARCH_DATA_ALIGNMENT
#else
# define DATA_ALIGNMENT    DEFAULT_DATA_ALIGNMENT
#endif

PHDRS {
	text     PT_LOAD  FLAGS(5);  /* R_E */
	rodata   PT_LOAD  FLAGS(5);  /* R_E */
	data     PT_LOAD  FLAGS(6);  /* RW_ */
	bss      PT_LOAD  FLAGS(6);  /* RW_ */
	#include <phdrs.lds.h>
}

SECTION_SYMBOLS(text)
SECTION_SYMBOLS(rodata)
SECTION_SYMBOLS(data)
SECTION_SYMBOLS(bss)
#include <section_symbols.lds.h>

SECTIONS {
	.text : ALIGN(TEXT_ALIGNMENT) {

		/* The reset code and the vector table are reached by address, so
		 * they have to survive --gc-sections */
		KEEP(*(.bootcode));
		KEEP(*(.text.kernel_vectors*));
		LDS_MODULES_TEXT
		*(.text)
		*(.text.*)
		etext   = .;

	} SECTION_REGION(text) :text

	.rodata : ALIGN(DATA_ALIGNMENT) {

		LDS_MODULES_RODATA
		*(.rodata)
		*(.rodata.*)
		*(.srodata)
		*(.srodata.*)
		*(.const)

		ALIGNMENT();
		_ctors_start = .;
		/* Priority first, sorted by priority; then the unprioritised ones.
		 * See board/embox-qemu/patches/image.lds-init-priority.py. */
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP(*(.init_array .ctors))
		_ctors_end   = .;

		ALIGNMENT();
		_dtors_start = .;
		/* Walked in reverse by cxx_invoke_destructors(), so the unprioritised
		 * ones go first here and the sorted prioritised ones last. */
		KEEP(*(.fini_array .dtors))
		KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
		_dtors_end   = .;

		ALIGNMENT();
		_eh_frame_begin = .;
		__eh_frame_start = .;
		KEEP(*(.eh_frame))
		KEEP(*(.eh_frame.*))
		KEEP(*(.eh_frame_end))
		__eh_frame_end = .;
		KEEP(*(.gcc_except_table*))

		ALIGNMENT();
		*(.checksum)

	} SECTION_REGION(rodata) :rodata

	.data : ALIGN(DATA_ALIGNMENT) {

		LDS_MODULES_DATA
		_app_data_vma = .;
		LDS_APPS_DATA
		*(.data)
		*(.data.*)
		*(.sdata)
		*(.sdata.*)
		*(.got)
		*(.got.*)

	} SECTION_REGION(data) :data
	LDS_MODULES_DATA_LMA

	.bss : ALIGN(DATA_ALIGNMENT) {

		LDS_MODULES_BSS
		LDS_APPS_BSS
		*(.bss)
		*(.sbss)
		*(.sbss.*)
		*(.bss.[a-zA-Z0-9_-]*) /* this hell is to exclude '.bss..reserve'. */
		/* ... and it excludes clang's .bss..L_MergedGlobals with it, which
		 * then lands past _reserve_end as an orphan and is never zeroed.
		 * '.L' is the local-label prefix, so this matches the compiler's own
		 * sections and nothing else. See
		 * board/embox-qemu/patches/image.lds-bss-merged-globals.py. */
		*(.bss..L*)
		/* *(.tbss) */
		/* *(.tbss.*) */

		ALIGNMENT();
		_bss_end = .;
		/* Reserve (no need to zero on startup) */

		*(.bss..reserve)
		*(.bss..reserve.*)

		LDS_APP_DATA_RESERVE_BSS

		ALIGNMENT();
		_reserve_end = .;
	} SECTION_REGION(bss) :bss

	_bss_len = _bss_end - _bss_vma;
	_bss_len_with_reserve = SIZEOF(.bss);

	#include <sections.lds.h>

	/DISCARD/ : {
		*(.comment)
		*(.note.*)
	}
}
