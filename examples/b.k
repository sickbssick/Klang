$unpack-> <Safe>;

$entry-> <main>;




init::exec main[safe::nul] -> i32 {
        ;; Simple Calculator!!
        
        safe::print["=== K Calculator ===\n"];

        safe::print["Enter you're operation!\n"];
        def op: mut, char = '\0';
        safe::print["User: "];
        safe::getin[" %c", op];

        safe::print["you have choosen: %c\n", op];

        safe::print["Choose your first number: "];
        def numb1: mut, f64 = 0.0;
        safe::getin["%dd", numb1];

        safe::print["Choose your second number: "];
        def numb2: mut, f64 = 0.0;
        safe::getin["%dd", numb2];

        def res: f64, mut, pub = 0.0;
        def res1: i64, mut, pub = 0;

        ifd (op == '+')>>do {
            res = numb1 + numb2;
            safe::print["the result is: %dd\n", res];
        } else ifd (op == '-')>>do {
            res = numb1 - numb2;
            safe::print["the result is: %dd\n", res];
        } else ifd(op == '*')>>do {
            res = numb1 * numb2;
            safe::print["the result is: %dd\n", res];
        } else ifd(op == '/')>>do {
            ifd (numb1 == 0 || numb2 == 0)>>do {
                safe::print["Invalid! Cannot divide by 0\n"];
            } else>>do {
                res = numb1 / numb2;
                safe::print["the result is: %dd\n", res];
            }
        } else ifd(op == 'p')>>do {
            res = numb1 ** numb2;
            safe::print["the result is: %dd", res];
        } else>>do {
            safe::print["Invalid Operation: %c\n", op];
        }

            def answer: mut, pub, char = '\0';
            safe::print["Do you wish to continue? y/n: "];
            safe::getin[" %c", answer];

            while (answer == 'y')>>do {
                safe::print["=== K Calculator ===\n"];

        safe::print["Enter you're operation!\n"];
        def op: mut, char = '\0';
        safe::print["User: "];
        safe::getin[" %c", op];

        safe::print["you have choosen: %c\n", op];

        safe::print["Choose your first number: "];
        def numb1: mut, f64 = 0.0;
        safe::getin["%dd", numb1];

        safe::print["Choose your second number: "];
        def numb2: mut, f64 = 0.0;
        safe::getin["%dd", numb2];

        def res: f64, mut, pub = 0.0;
        def res1: i64, mut, pub = 0;

        ifd (op == '+')>>do {
            res = numb1 + numb2;
            safe::print["the result is: %dd\n", res];
        } else ifd (op == '-')>>do {
            res = numb1 - numb2;
            safe::print["the result is: %dd\n", res];
        } else ifd(op == '*')>>do {
            res = numb1 * numb2;
            safe::print["the result is: %dd\n", res];
        } else ifd(op == '/')>>do {
            ifd (numb1 == 0 || numb2 == 0)>>do {
                safe::print["Invalid! Cannot divide by 0\n"];
            } else>>do {
                res = numb1 / numb2;
                safe::print["the result is: %dd\n", res];
            }
        } else ifd(op == 'p')>>do {
            res = numb1 ** numb2;
            safe::print["the result is: %dd\n", res];
        } else>>do {
            safe::print["Invalid Operation: %c\n", op];
        }
            }



    ret 0;

}