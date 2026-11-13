/*
 * Added to lld's default layout of libemerald.so (INSERT): the game's own
 * variables together, between __ctr_game_* symbols, so the soft reset can put
 * them back as the GBA clears its RAM (3ds_game_full.c) - the same grouping
 * as 3ds_port/emerald3ds.ld.in.
 *
 * The game translation units' read-only data (build/root/src, build/root/data)
 * leaves .rodata and .data.rel.ro - where a PIC link keeps tables of pointers,
 * the asm data's included (tools/rw_data.sh) - and is gathered in .gamedata,
 * writable (the bundle loader fills it, 3ds_script_loader.c) and before .bss,
 * so that both links below lay everything out alike. As on the 3DS
 * (emerald3ds.ld.in), android.mk makes two scripts of this template, which
 * differ only in @GAMEDATA_TYPE@: the image link keeps the section's contents
 * for scripts/gen_gamedata_bundle.py to take out, and the shipped library
 * reserves it NOLOAD: game content, it comes from the data pack at start-up.
 */
SECTIONS
{
	.ctr_game_data :
	{
		__ctr_game_data = .;
		/* Writable data only: .data.rel.ro.* (constant tables of pointers)
		 * is read-only game data and goes to .gamedata below. */
		EXCLUDE_FILE(*build/root/src/platform/*) *build/root/src/*(CTR_WRITABLE_DATA)
		*build/root/data/*(CTR_WRITABLE_DATA)
		. = ALIGN(4);
		__ctr_game_data_end = .;
	}
	/* The only script_data linked is the Mystery Event command table's, which
	 * .gamedata takes (below); the script bytecode is bundled. Another one
	 * would be an orphan after .gamedata, following a NOBITS section with
	 * contents: the NOLOAD link would gain a segment the image does not have,
	 * and gen_gamedata_bundle.py would refuse the two links. */
	.gamedata ALIGN(32) @GAMEDATA_TYPE@ :
	{
		__ctr_gamedata = .;
		*build/root/src/*(.rodata .rodata.* .data.rel.ro .data.rel.ro.*)
		*build/root/data/*(.rodata .rodata.* .data.rel.ro .data.rel.ro.*)
		/* The Mystery Event command table: assembled 32-bit addresses, which
		 * only the bundle loader fills in on arm64 (android.mk). */
		*build/root/data/*(script_data)
		. = ALIGN(4);
		__ctr_gamedata_end = .;
	}
}
INSERT AFTER .data;

/* arm64's unwind tables (.eh_frame), placed: lld otherwise orders them
 * against .rodata differently with --emit-relocs, and the two links - which
 * must lay everything out alike - came out 16 bytes apart. 32-bit ARM unwinds
 * from .ARM.exidx instead and has little .eh_frame. */
SECTIONS
{
	.eh_frame_hdr : { *(.eh_frame_hdr) }
	.eh_frame : { KEEP(*(.eh_frame)) }
}
INSERT AFTER .rodata;

SECTIONS
{
	.ctr_game_bss (NOLOAD) :
	{
		__ctr_game_bss = .;
		/* Juan's defeat text runs past gDisplayedStringBattle[300]: room behind it. */
		*build/root/src/battle_main.o(.bss.gDisplayedStringBattle)
		. += 1024;
		EXCLUDE_FILE(*build/root/src/platform/*) *build/root/src/*(.bss .bss.* COMMON)
		*build/root/data/*(.bss .bss.* COMMON)
		. = ALIGN(4);
		__ctr_game_bss_end = .;
	}
}
INSERT AFTER .bss;
