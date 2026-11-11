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
	/* Placed, not left an orphan: one after .gamedata would follow a NOBITS
	 * section with contents, and the NOLOAD link would gain a segment the
	 * image does not have - every address moved. .gamedata ends the data. */
	script_data : { *(script_data) }
	.gamedata ALIGN(32) @GAMEDATA_TYPE@ :
	{
		__ctr_gamedata = .;
		*build/root/src/*(.rodata .rodata.* .data.rel.ro .data.rel.ro.*)
		*build/root/data/*(.rodata .rodata.* .data.rel.ro .data.rel.ro.*)
		. = ALIGN(4);
		__ctr_gamedata_end = .;
	}
}
INSERT AFTER .data;

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
