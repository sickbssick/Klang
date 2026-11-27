$unpack-> <Safe>;
$entry-> <main>;

set i32, int;

init::exec main[safe::nul] -> int {
    safe::print["Hello World\n"];

    def exp1: i64 = 5075;
    def exp2: f64 = 7600.55;
    def exp3: safe::str = "Alex";
    def exp4: byte = 'A';
    def exp5: bool = true;
    def exp6: u64 = 7500000;

    safe::print["%d\n", exp1];
    safe::print["%dd\n", exp2];
    safe::print["%s\n", exp3];
    safe::print["%c\n", exp4];
    safe::print["%b\n", exp5];
    safe::print["%u\n", exp6];

    def x: mut, i8 = 10;
    def y: i8 = 20;
    ifd (x < y)>>do {
        safe::print["smaller\n"];
    } else>>do {
        safe::print["bigger or equal\n"];
    }

    for (def i: mut, i64 = 0; i < 3; i++)>>do {
        safe::print["i = %d\n", i];
    }

    safe::print["2**5 = %d\n", 2 ** 5];
    ret 0;
}
