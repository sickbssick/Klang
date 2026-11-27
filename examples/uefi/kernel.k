$entry-> <kmain>;

init::exec serial_putc[ch: u8] -> void {
    raw::outb[0x3F8, ch];
    ret;
}

init::exec uefi_print[sys_table: u64, msg: *u8] -> void {
    for (def i: mut, u64 = 0; msg[i] != 0; i++)>>do {
        uefi_putc[sys_table, msg[i]];
    }
    ret;
}

init::exec kmain[img_handle: u64, sys_table: u64] -> u64 {
    uefi_print[sys_table, "Hello from K UEFI Kernel!\r\n"];
    uefi_print[sys_table, "Running in 64-bit Long Mode via OVMF.\r\n"];

    raw::asm["cli; hlt"];
    ret 0;
}
