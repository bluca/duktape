if (typeof print !== 'function') { print = console.log; }

function test() {
    var i;
    var x;

    x = 'foobar\u{1f4a9}'.repeat(1000);
    console.log(x);

    for (i = 0; i < 1e5; i++) {
        void x.indexOf('\u{1f4a9}');
        void x.indexOf('\u{1f4a9}', 3000);
        void x.indexOf('\u{1f4a9}');
        void x.indexOf('\u{1f4a9}', 3000);
        void x.indexOf('\u{1f4a9}');
        void x.indexOf('\u{1f4a9}', 3000);
        void x.indexOf('\u{1f4a9}');
        void x.indexOf('\u{1f4a9}', 3000);
        void x.indexOf('\u{1f4a9}');
        void x.indexOf('\u{1f4a9}', 3000);
    }
}

try {
    test();
} catch (e) {
    print(e.stack || e);
    throw e;
}
