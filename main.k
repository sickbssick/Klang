$unpack-> <Safe>;

$entry-> <main>;

init::exec main[safe::nul] -> i32 {
    for (def i: mut, i64 = 0; i < 1000000; i++)>>do {
        safe::print["%d", i];
    }

   ret 0;
}