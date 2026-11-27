$entry-> <kmain>;

init::exec serial_putc[ch: u8] -> void {
    raw::outb[0x3F8, ch];
    ret;
}

init::exec kmain[safe::nul] -> void {
    def vga: *u16 = 0xB8000;
    def color: u16 = 0x0F << 8;
    vga[0] = 'K' | color;
    vga[1] = 'O' | color;
    vga[2] = 'S' | color;

    serial_putc['K'];
    serial_putc['\n'];

    raw::asm["cli; hlt"];
    ret;
}
