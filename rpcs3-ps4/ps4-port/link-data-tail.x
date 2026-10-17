/*
 * Added after the SDK's link.x (native_link.cmake).
 *
 * create-fself shortens the OELF data segment by the distance from the end of its file-backed part to the next
 * 16 KiB boundary. That cuts the end of .bss out of the image the PS4 maps, and a relocation pointing into the cut
 * part makes the loader refuse the whole module (rtld reloc_non_plt, PRX_INVALID_IMAGE, CE-34878-0).
 *
 * The sections below take every writable file-backed input section the SDK script leaves as an orphan (the
 * .data.* sections, and protobuf's pb_defaults, which keeps its own output section for __start_/__stop_), and the
 * last one ends the file-backed data on a 16 KiB boundary, so the distance is zero. package-native.ps1 checks the
 * resulting OELF and refuses to package one whose data segment does not cover the ELF's.
 */
SECTIONS
{
	.data.tail : {
		*(.data.* .data)
	}
	pb_defaults : {
		KEEP(*(pb_defaults))
	}
	.data.end : {
		BYTE(0);
		. = ALIGN(0x4000);
	}
}
INSERT BEFORE .bss;
