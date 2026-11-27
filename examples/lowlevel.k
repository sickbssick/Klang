$unpack-> <Safe>;
$entry-> <main>;

init::exec main[safe::nul] -> i32 {
    def mask: u64 = (0x0F << 4) | 0b0101;
    safe::print["mask = %d\n", mask];

    def x: mut, i64 = 42;
    def p: *i64 = &x;
    *p = 99;
    safe::print["x via ptr = %d\n", x];

    def buf: *u8 = malloc[16];
    buf[0] = 'K';
    buf[1] = '!';
    buf[2] = 0;
    safe::print["buf = %s\n", buf];
    free[buf];

    ret 0;
}
