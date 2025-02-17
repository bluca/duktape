/*
 *  Test for default string encoding behavior: valid UTF-8 strings are
 *  encoded as CBOR text strings, other strings as CBOR byte strings.
 */

/*===
66666f6f626172
"foobar"
69666f6fe188b4626172
"foo\u1234bar"
49666f6feda080626172
|666f6feda080626172|
49666f6fedb080626172
|666f6fedb080626172|
4d666f6fedb08064383030626172
|666f6fedb08064383030626172|
6a666f6ff09f92a9626172
"foo\U0001f4a9bar"
done
===*/

function test(val) {
    var t = CBOR.encode(val);
    print(Duktape.enc('hex', t));
    print(Duktape.enc('jx', CBOR.decode(t)));
}

// Plain ASCII.
test('foobar');

// BMP.
test('foo\u1234bar');

// Unpaired surrogate.
test('foo\ud800bar');
test('foo\udc00bar');
test('foo\udc00d800bar');

// Paired surrogate.  Currently CBOR.encode() does not combine the
// surrogate by itself, but WTF-8 string intern step does that so
// the outcome is a combined codepoint.
test('foo\ud83d\udca9bar');

// XXX: Add coverage when C API exists:
// - BF A0: initial byte is a continuation byte

print('done');
