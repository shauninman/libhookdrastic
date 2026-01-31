#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <addr> [value]\n", argv[0]);
		return 1;
	}

	off_t addr = strtoul(argv[1], NULL, 0);
	int set = argc > 2;
	uint32_t val = set ? strtoul(argv[2], NULL, 0) : 0;

	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	size_t pagesz = getpagesize();
	off_t base = addr & ~(pagesz - 1);

	uint8_t *map = mmap(NULL, pagesz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	volatile uint32_t *reg = (uint32_t *)(map + (addr - base));

	if (set) {
		*reg = val;
		printf("Wrote 0x%08X to 0x%08lX\n", val, addr);
	} else {
		printf("0x%08lX = 0x%08X\n", addr, *reg);
	}

	munmap((void*)map, pagesz);
	close(fd);
	return 0;
}
