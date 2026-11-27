KCC = ../../kcc

all: BOOTX64.EFI

BOOTX64.EFI: uefi_entry.s kernel.k linker.ld
	$(KCC) -kernel -ffocus -T linker.ld uefi_entry.s kernel.k -bo kernel.elf
	objcopy -O binary kernel.elf BOOTX64.EFI
	mkdir -p esp/EFI/BOOT
	cp BOOTX64.EFI esp/EFI/BOOT/BOOTX64.EFI

run: all
	qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd -drive format=raw,file=fat:rw:esp -serial stdio

clean:
	rm -rf kernel.elf BOOTX64.EFI uefi.img esp
