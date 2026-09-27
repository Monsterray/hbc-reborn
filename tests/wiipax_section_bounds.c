#include <stdio.h>

#define main wiipax_tool_main
#include "../wiipax/client/main.c"
#undef main

int main(void) {
	elf_t elf = {0};
	Elf32_Ehdr ehdr = {0};
	Elf32_Shdr shdrs[2] = {0};
	u8 data[64] = {0};

	elf.data = data;
	elf.len = sizeof(data);
	elf.ehdr = &ehdr;
	elf.shdrs = shdrs;
	ehdr.e_shnum = be16(2);
	ehdr.e_shstrndx = be16(1);
	shdrs[0].sh_name = be32(4);
	shdrs[0].sh_offset = be32(42);
	shdrs[1].sh_offset = be32(16);
	shdrs[1].sh_size = be32(4);
	memcpy(&data[20], ".payload", 9);

	/* The name points just past the table, where a tempting match is placed. */
	return find_payload_offset(&elf) == 42 ? 0 : 2;
}
