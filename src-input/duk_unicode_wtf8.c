/*
 *  WTF-8 helpers
 */

#include "duk_internal.h"

/* Check whether a byte sequence is valid WTF-8. */
DUK_INTERNAL duk_bool_t duk_unicode_is_valid_wtf8(const duk_uint8_t *data, duk_size_t blen) {
	const duk_uint8_t *p;
	const duk_uint8_t *p_end;

	DUK_ASSERT(data != NULL || blen == 0);

	p = data;
	p_end = data + blen;
	while (p != p_end) {
		duk_uint8_t t;

		t = *p;
		if (DUK_LIKELY(t <= 0x7fU)) {
			p++;
			continue;
		}

		if (t <= 0xc1U) {
			/* 0x80-0xbf: continuation byte, 0xc0 and 0xc1 invalid
			 * initial bytes for 2-byte sequences (too low codepoint).
			 */
			return 0;
		} else if (t <= 0xdfU) {
			if (p_end - p >= 2 && p[1] >= 0x80U && p[1] <= 0xbfU) {
				p += 2;
			} else {
				return 0;
			}
		} else if (t <= 0xefU) {
			duk_uint8_t lower = (t == 0xe0U ? 0xa0U : 0x80U);
			if (p_end - p >= 3 && p[1] >= lower && p[1] <= 0xbfU && p[2] >= 0x80U && p[2] <= 0xbfU) {
				p += 3;
			} else {
				return 0;
			}
		} else if (t <= 0xf4U) {
			duk_uint8_t lower = (t == 0xf0U ? 0x90U : 0x80U);
			duk_uint8_t upper = (t == 0xf4U ? 0x8fU : 0xbfU);

			if (p_end - p >= 4 && p[1] >= lower && p[1] <= upper && p[2] >= 0x80U && p[2] <= 0xbfU && p[3] >= 0x80U &&
			    p[3] <= 0xbfU) {
				p += 4;
			} else {
				return 0;
			}
		} else {
			/* 0xf5-0xf7 are invalid 4-byte sequences, 0xf8-0xff are invalid
			 * initial bytes.
			 */
			return 0;
		}
	}

	return 1;
}

/* Straightforward reference implementation for the WTF-8 sanitization algorithm.
 * Caller must ensure 'out' has enough space for maximum expansion, 3x input.
 */
DUK_LOCAL duk_uint32_t duk__unicode_wtf8_sanitize_string_reference(const duk_uint8_t *str,
                                                                   duk_uint32_t str_blen,
                                                                   duk_uint8_t *out_data) {
	const duk_uint8_t *p;
	const duk_uint8_t *p_end;
	duk_uint8_t *q;
	duk_uint32_t out_clen_sub = 0; /* Output charlen = out_blen - out_clen_sub. */
	duk_uint32_t out_blen;
	duk_uint32_t out_clen;

	DUK_ASSERT(str_blen == 0 || str != NULL);
	DUK_ASSERT(out_data != NULL);

	p = str;
	p_end = str + str_blen;
	q = out_data;

	while (p != p_end) {
		duk_uint8_t t;
		duk_uint8_t lower, upper;
		duk_small_int_t num_cont;
		duk_uint_t cp;

		DUK_ASSERT(p < p_end);

		/* >>> u'\ud7ff'.encode('utf-8')
		 * '\xed\x9f\xbf'
		 * >>> u'\ud800'.encode('utf-8')
		 * '\xed\xa0\x80'
		 * >>> u'\udc00'.encode('utf-8')
		 * '\xed\xb0\x80'
		 * >>> u'\udfff'.encode('utf-8')
		 * '\xed\xbf\xbf'
		 * >>> u'\ue000'.encode('utf-8')
		 * '\xee\x80\x80'
		 */
		t = *p++;
		if (DUK_LIKELY(t < 0x80U)) {
			/* For ASCII blen == clen, no need to update
			 * out_clen_sub.
			 */
			*q++ = t;
			continue;
		}

		if (t >= 0xc2U) {
			if (t <= 0xdfU) {
				cp = t & 0x1fU;
				num_cont = 1;
				lower = 0x80U;
				upper = 0xbfU;
			} else if (t <= 0xefU) {
				cp = t & 0x0fU;
				num_cont = 2;
				lower = (t == 0xe0U ? 0xa0U : 0x80U);
				upper = 0xbfU;
			} else if (t <= 0xf4U) {
				cp = t & 0x07U;
				num_cont = 3;
				lower = (t == 0xf0U ? 0x90U : 0x80U);
				upper = (t == 0xf4U ? 0x8fU : 0xbfU);
			} else {
				/* Invalid initial byte. */
				DUK_ASSERT(t >= 0xf5U);
				DUK_ASSERT_DISABLE(t <= 0xffU);
				goto replacement;
			}
		} else {
			/* Continuation byte or invalid initial byte (0xc0 or 0xc1). */
			DUK_ASSERT((t >= 0x80U && t <= 0xbfU) || t == 0xc0U || t == 0xc1U);
			goto replacement;
		}

		DUK_ASSERT(num_cont > 0);
		do {
			if (DUK_UNLIKELY(p == p_end)) {
				/* Truncated. */
				goto replacement;
			}
			t = *p++;
			if (DUK_LIKELY(t >= lower && t <= upper)) {
				cp = (cp << 6) + (t & 0x3fU);
				lower = 0x80U;
				upper = 0xbfU;
			} else {
				/* Invalid continuation byte, use replacement and
				 * reinterpret invalid byte.
				 */
				p--;
				goto replacement;
			}
		} while (--num_cont != 0);

		if (cp >= 0xd800UL && cp <= 0xdfffUL) {
			/* High or low surrogate. */
			if (cp < 0xdc00UL) {
				/* High surrogate, check if paired. */
				duk_bool_t is_paired = (p_end - p >= 3 && p[0] == 0xedU && p[1] >= 0xb0U && p[1] <= 0xbfU &&
				                        p[2] >= 0x80U && p[2] <= 0xbfU);
				if (is_paired) {
					duk_uint32_t hi, lo, cp_combined;

					hi = cp & 0x3ffU;
					lo = (((duk_uint32_t) p[1] & 0x0fU) << 6U) + ((duk_uint32_t) p[2] & 0x3fU);
					cp_combined = 0x10000UL + (hi << 10U) + lo;
					cp = cp_combined;

					p += 3;
				} else {
					/* Keep 'cp' as is. */
				}
			} else {
				/* Unpaired low surrogate, keep as is. */
			}
		}

		/* Emit original or combined surrogate pair codepoint 'cp'. */
		DUK_ASSERT(cp >= 0x80UL); /* ASCII handled already. */
		DUK_ASSERT(cp <= 0x10ffffUL); /* Decode upper/lower ranges ensure this. */
		if (cp <= 0x7ffUL) {
			*q++ = 0xc0U + (cp >> 6U);
			*q++ = 0x80U + (cp & 0x3fU);
			out_clen_sub += 1;
		} else if (cp <= 0xffffUL) {
			*q++ = 0xe0U + (cp >> 12U);
			*q++ = 0x80U + ((cp >> 6U) & 0x3fU);
			*q++ = 0x80U + (cp & 0x3fU);
			out_clen_sub += 2;
		} else {
			*q++ = 0xf0U + (cp >> 18U);
			*q++ = 0x80U + ((cp >> 12U) & 0x3fU);
			*q++ = 0x80U + ((cp >> 6U) & 0x3fU);
			*q++ = 0x80U + (cp & 0x3fU);
			out_clen_sub += 3;
		}
		continue;

	replacement:
		*q++ = 0xefU;
		*q++ = 0xbfU;
		*q++ = 0xbdU;
		out_clen_sub += 2; /* 3 bytes, 1 char */
	}

	out_blen = (duk_uint32_t) (q - out_data);
	DUK_ASSERT(out_clen_sub <= out_blen);
	out_clen = out_blen - out_clen_sub;
	DUK_UNREF(out_clen);

	return out_blen;
}

/* Sanitize Symbol reference, for now copied 1:1. */
DUK_LOCAL duk_uint32_t duk__unicode_wtf8_sanitize_symbol_reference(const duk_uint8_t *str,
                                                                   duk_uint32_t str_blen,
                                                                   duk_uint8_t *out) {
	DUK_ASSERT(str_blen == 0 || str != NULL);
	DUK_ASSERT(out != NULL);

	duk_memcpy((void *) out, (const void *) str, (size_t) str_blen);
	return str_blen;
}

DUK_INTERNAL duk_uint32_t duk_unicode_wtf8_sanitize_symbol(const duk_uint8_t *str, duk_uint32_t str_blen, duk_uint8_t *out) {
	return duk__unicode_wtf8_sanitize_symbol_reference(str, str_blen, out);
}

DUK_INTERNAL duk_uint32_t duk_unicode_wtf8_sanitize_string(const duk_uint8_t *str, duk_uint32_t str_blen, duk_uint8_t *out) {
	return duk__unicode_wtf8_sanitize_string_reference(str, str_blen, out);
}

DUK_INTERNAL duk_uint32_t duk_unicode_wtf8_sanitize_detect(const duk_uint8_t *str, duk_uint32_t str_blen, duk_uint8_t *out) {
	duk_bool_t symbol = 0;

	DUK_ASSERT(str_blen == 0 || str != NULL);
	DUK_ASSERT(out != NULL);

	if (DUK_LIKELY(str_blen) > 0) {
		duk_uint8_t ib = str[0];
		if (DUK_LIKELY(ib <= 0x7fU)) {
			/* ASCII expected. */
		} else {
			if (DUK_UNLIKELY(ib == 0x80U || ib == 0x81U || ib == 0x82U || ib == 0xffU)) {
				symbol = 1;
			}
		}
	}

	if (DUK_UNLIKELY(symbol)) {
		return duk_unicode_wtf8_sanitize_symbol(str, str_blen, out);
	} else {
		return duk_unicode_wtf8_sanitize_string(str, str_blen, out);
	}
}

#if 0
DUK_LOCAL duk_uint32_t duk__unicode_wtf8_sanitize_asciicheck_reference(const duk_uint8_t *str, duk_uint32_t blen) {
	duk_uint32_t i;

	DUK_ASSERT(blen == 0 || str != NULL);

	/* For now, just fast path ASCII. */
	for (i = 0; i < blen; i++) {
		if (DUK_UNLIKELY(str[i] >= 0x80U)) {
			return i;
		}
	}

	return blen;
}
#endif

DUK_LOCAL DUK_NOINLINE duk_uint32_t duk__unicode_wtf8_sanitize_asciicheck_optimized(const duk_uint8_t *str, duk_uint32_t blen) {
	const duk_uint8_t *p;
	const duk_uint8_t *p_end;
	const duk_uint32_t *p32;
	const duk_uint32_t *p32_end;

	DUK_ASSERT(blen == 0 || str != NULL);

	/* Simplify handling by skipping short strings, avoids the need for e.g.
	 * end-of-input check in the alignment setup loop.
	 */
	p = str;
	p_end = p + blen;
	if (blen < 8U) {
		goto skip_fastpath;
	}

	/* Step to 4-byte alignment. */
	while (((duk_size_t) (const void *) p) & 0x03UL) {
		DUK_ASSERT(p < p_end);
		if (DUK_UNLIKELY(*p >= 0x80U)) {
			goto skip_fastpath; /* Handle with shared code below. */
		}
		p++;
	}

	p32_end = (const duk_uint32_t *) (const void *) (p + ((duk_size_t) (p_end - p) & (duk_size_t) (~0x03U)));
	p32 = (const duk_uint32_t *) (const void *) p;
	DUK_ASSERT(p32_end > p32); /* At least one full block, guaranteed by minimum size check. */
	do {
		duk_uint32_t x;

		DUK_ASSERT(((const duk_uint8_t *) p32 + 4U) <= (const duk_uint8_t *) p_end);
		x = *p32;
		if (DUK_LIKELY((x & 0x80808080UL) == 0UL)) {
			; /* All 4 bytes are ASCII. */
		} else {
			/* One or more bytes are not ASCII, handle in slow path. */
			break;
		}
		p32++;
	} while (p32 != p32_end);

	p = (const duk_uint8_t *) p32;
	/* Fall through to handle the rest. */

skip_fastpath:
	while (p != p_end) {
		DUK_ASSERT(p < p_end);
		if (DUK_UNLIKELY(*p >= 0x80U)) {
			return (duk_uint32_t) (p - str);
		}
		p++;
	}

	DUK_ASSERT(p == str + blen);
	return blen;
}

/* Check how many valid WTF-8 bytes we can keep from the beginning of the
 * input data.  The check can be conservative, i.e. reject some valid
 * sequences if that makes common cases faster.  Return value indicates
 * how many bytes can be kept.
 *
 * However, for Symbol values MUST return 'blen', i.e. keep entire string
 * as is (call site expects this).
 */
DUK_INTERNAL duk_uint32_t duk_unicode_wtf8_sanitize_keepcheck(const duk_uint8_t *str, duk_uint32_t blen) {
	duk_uint32_t blen_keep;

	blen_keep = duk__unicode_wtf8_sanitize_asciicheck_optimized(str, blen);
#if 0
	blen_keep = duk__unicode_wtf8_sanitize_asciicheck_reference(str, blen);
#endif

	if (DUK_UNLIKELY(blen_keep == 0U)) {
		/* Symbols begin with an invalid WTF-8 byte so we can detect
		 * them reliably here.
		 */
		if (blen > 0U) {
			duk_uint8_t ib = str[0];
			if (DUK_UNLIKELY(ib == 0x80U || ib == 0x81U || ib == 0x82U || ib == 0xffU)) {
				return blen;
			}
		}
	}

	return blen_keep;
}

/* Compute ECMAScript character length for a valid WTF-8 string (caller ensures).
 * Character length is number of WTF-8 codepoints except non-BMP codepoints count
 * as two characters as they'd normally be represented by a surrogate pair in ES.
 */
DUK_INTERNAL duk_size_t duk_unicode_wtf8_charlength(const duk_uint8_t *data, duk_size_t blen) {
	const duk_uint8_t *p;
	const duk_uint8_t *p_end;
	duk_size_t adj = 0;
	duk_size_t clen;

	DUK_ASSERT(duk_unicode_is_valid_wtf8(data, blen));

	p = data;
	p_end = data + blen;
	adj = 0;
	while (p != p_end) {
		duk_uint8_t x;

		DUK_ASSERT(p <= p_end);
		x = *p;
		if (DUK_LIKELY(x <= 0x7fU)) {
			/* ASCII: 1 byte, 1 char, so adj += 0. */
			p++;
		} else {
			DUK_ASSERT(!(x >= 0x80U && x <= 0xbfU)); /* Valid WTF-8 assumption. */
			if (x <= 0xdfU) {
				/* 2-byte sequence, one char. */
				p += 2;
				adj += (2 - 1);
			} else if (x <= 0xefU) {
				/* 3-byte sequence, one char. */
				p += 3;
				adj += (3 - 1);
			} else {
				/* 4-byte sequence, two chars, because non-BMP is
				 * represented as a surrogate pair in ES view.
				 */
				p += 4;
				adj += (4 - 2);
			}
		}
		DUK_ASSERT(p <= p_end);
	}

	DUK_ASSERT(adj <= blen);
	clen = blen - adj;
	DUK_ASSERT(clen <= blen);
	return clen;
}

/* Substring operation for a valid WTF-8 string.  Input must be valid WTF-8 for
 * memory safety to be guaranteed.  The character offset [start,end[ are from
 * ECMAScript viewpoint, i.e. non-BMP codepoints considered to be represented by
 * a surrogate pair.
 *
 * In most cases the substring can be copied as is from the input.  However, it
 * may be that the start and/or end offset are in the middle of a non-BMP codepoint
 * in which case we must manufacture a surrogate character.
 */

DUK_INTERNAL duk_hstring *duk_push_wtf8_substring_hstring(duk_hthread *thr,
                                                          duk_hstring *h_input,
                                                          duk_size_t start_offset,
                                                          duk_size_t end_offset) {
	const duk_uint8_t *data;
	duk_uint32_t blen;
	duk_uint_t prefix_surrogate = 0;
	duk_uint_t suffix_surrogate = 0;
	duk_size_t copy_start;
	duk_size_t copy_end;
	duk_uint32_t start_byteoff;
	duk_uint32_t start_charoff;
	duk_uint32_t end_byteoff;
	duk_uint32_t end_charoff;

	/* ASCII fast path. */
	if (duk_hstring_get_charlen(h_input) == duk_hstring_get_bytelen(h_input)) {
		duk_push_lstring(thr,
		                 (const char *) (duk_hstring_get_data(h_input) + start_offset),
		                 (duk_size_t) (end_offset - start_offset));
		return duk_known_hstring_m1(thr);
	}

	/* Caller must validate input to be WTF-8 and offsets to be valid and
	 * non-crossed.
	 */
	data = duk_hstring_get_data(h_input);
	blen = duk_hstring_get_bytelen(h_input);
	DUK_ASSERT(duk_unicode_is_valid_wtf8(data, blen));
	DUK_UNREF(blen);
	DUK_ASSERT(data != NULL || blen == 0);
	DUK_ASSERT(start_offset <= end_offset);

	/* Special handling for zero-size input to avoid corner case below:
	 * for an empty substring the start and end offset might both be
	 * splitting the same non-BMP codepoint.
	 */
	if (start_offset == end_offset) {
		duk_push_hstring_empty(thr);
		goto done;
	}

	/* Scan to start. */
	duk_strcache_scan_char2byte_wtf8(thr, h_input, start_offset, &start_byteoff, &start_charoff);

	if (DUK_UNLIKELY(start_charoff != start_offset)) {
		/* Start position split a logical surrogate pair encoded
		 * as a single WTF-8 codepoint, extract surrogate as prefix.
		 */
		duk_ucodepoint_t start_cp;

		start_cp = duk_unicode_wtf8_decode_known(duk_hstring_get_data(h_input) + start_byteoff);
		prefix_surrogate = 0xdc00UL + ((start_cp - 0x10000UL) & 0x3ffUL);
		copy_start = start_byteoff + 4; /* Skip encoded non-BMP codepoint. */
	} else {
		copy_start = start_byteoff;
	}

	DUK_DD(DUK_DDPRINT("copy_start=%ld, prefix_surrogate=%ld", (long) copy_start, (long) prefix_surrogate));

	duk_strcache_scan_char2byte_wtf8(thr, h_input, end_offset, &end_byteoff, &end_charoff);

	if (DUK_UNLIKELY(end_charoff != end_offset)) {
		/* End position splits a logical surrogate pair encoded
		 * as a single WTF-8 codepoint, extract surrogate as suffix.
		 */
		duk_ucodepoint_t end_cp;

		end_cp = duk_unicode_wtf8_decode_known(duk_hstring_get_data(h_input) + end_byteoff);
		suffix_surrogate = 0xd800UL + ((end_cp - 0x10000UL) >> 10U);
		copy_end = end_byteoff;
	} else {
		copy_end = end_byteoff;
	}
	DUK_ASSERT(copy_end >= copy_start);

	DUK_DD(DUK_DDPRINT("copy_end=%ld, suffix_surrogate=%ld", (long) copy_end, (long) suffix_surrogate));

	/* Push result string.  If no surrogates need to be injected, we can
	 * push directly from the input without a temporary.  If surrogates
	 * need to be injected, we need a temporary.
	 */

	if (DUK_LIKELY(prefix_surrogate == 0 && suffix_surrogate == 0)) {
		duk_size_t copy_size;

		copy_size = copy_end - copy_start;
		DUK_DD(DUK_DDPRINT("fast path, no temporary: copy_size=%ld", (long) copy_size));

		duk_push_lstring(thr, (const char *) (data + copy_start), copy_size);
	} else {
		duk_size_t copy_size = copy_end - copy_start;
		duk_size_t alloc_size = (prefix_surrogate ? 3 : 0) + copy_size + (suffix_surrogate ? 3 : 0);
		duk_uint8_t *buf;
		duk_uint8_t *q;

		DUK_DD(DUK_DDPRINT("slow path: prefix_surrogate=%ld, suffix_surrogate=%ld, copy_size=%ld, alloc_size=%ld",
		                   (long) prefix_surrogate,
		                   (long) suffix_surrogate,
		                   (long) copy_size,
		                   (long) alloc_size));

		DUK_ASSERT(alloc_size > 0); /* At least one manufactured surrogate. */
		buf = (duk_uint8_t *) duk_push_fixed_buffer_nozero(thr, alloc_size);
		DUK_ASSERT(buf != NULL);

		q = buf;
		if (prefix_surrogate) {
			DUK_ASSERT(prefix_surrogate >= 0xdc00UL && prefix_surrogate <= 0xdfffUL); /* Low surrogate if any. */
			DUK_ASSERT(0xe0U + (prefix_surrogate >> 12U) == 0xedU);
			*q++ = 0xedU;
			*q++ = 0x80U + ((prefix_surrogate >> 6U) & 0x3fU);
			*q++ = 0x80U + (prefix_surrogate & 0x3fU);
		}
		duk_memcpy((void *) q, (const void *) (data + copy_start), copy_size);
		q += copy_size;
		if (suffix_surrogate) {
			DUK_ASSERT(suffix_surrogate >= 0xd800UL && suffix_surrogate <= 0xdbffUL); /* High surrogate if any. */
			DUK_ASSERT(0xe0U + (suffix_surrogate >> 12U) == 0xedU);
			*q++ = 0xedU;
			*q++ = 0x80U + ((suffix_surrogate >> 6U) & 0x3fU);
			*q++ = 0x80U + (suffix_surrogate & 0x3fU);
		}
		DUK_ASSERT(buf + alloc_size == q);

		duk_push_lstring(thr, (const char *) buf, alloc_size);
		duk_remove_m2(thr);
	}

done:
	return duk_known_hstring_m1(thr);
}

/* Find a string from within an input string. Must account for non-BMP codepoints,
 * e.g. search string may start with a low surrogate which must be correctly matched
 * with combined surrogates in the input.
 *
 * Empty string always matches.
 */

/* Naive implementation for reference. */
DUK_LOCAL duk_int_t duk__unicode_wtf8_search_forwards_reference(duk_hthread *thr,
                                                                duk_hstring *h_input,
                                                                duk_hstring *h_match,
                                                                duk_uint32_t start_charoff) {
	duk_uint32_t match_charlen;
	duk_uint32_t input_charlen;
	duk_uint32_t charoff;

	input_charlen = duk_hstring_get_charlen(h_input);
	match_charlen = duk_hstring_get_charlen(h_match);
	DUK_DD(DUK_DDPRINT("input_charlen=%ld, match_charlen=%d, start_charoff=%ld",
	                   (long) input_charlen,
	                   (long) match_charlen,
	                   (long) start_charoff));

	for (charoff = start_charoff; charoff <= input_charlen; charoff++) {
		/* Must scan to charoff == input_charlen for zero length input. */
		DUK_DDD(DUK_DDDPRINT("wtf8 find, charoff=%ld", (long) charoff));
		if (charoff + match_charlen <= input_charlen) {
			duk_hstring *h_tmp;

			h_tmp = duk_push_wtf8_substring_hstring(thr, h_input, charoff, charoff + match_charlen);
			DUK_DDD(DUK_DDDPRINT("substring=%!O, match=%!O", h_tmp, h_match));

			/* Rely on string interning! */
			if (h_tmp == h_match) {
				duk_pop_unsafe(thr);
				return (duk_int_t) charoff;
			}
			duk_pop_unsafe(thr);
		}
	}
	return -1;
}

DUK_INTERNAL duk_int_t duk_unicode_wtf8_search_forwards(duk_hthread *thr,
                                                        duk_hstring *h_input,
                                                        duk_hstring *h_match,
                                                        duk_uint32_t start_charoff) {
	return duk__unicode_wtf8_search_forwards_reference(thr, h_input, h_match, start_charoff);
}

/* Naive implementation for reference. */
DUK_LOCAL duk_int_t duk__unicode_wtf8_search_backwards_reference(duk_hthread *thr,
                                                                 duk_hstring *h_input,
                                                                 duk_hstring *h_match,
                                                                 duk_uint32_t start_charoff) {
	duk_uint32_t match_charlen;
	duk_uint32_t input_charlen;
	duk_int_t i;
	duk_uint32_t charoff;

	input_charlen = duk_hstring_get_charlen(h_input);
	match_charlen = duk_hstring_get_charlen(h_match);
	DUK_DD(DUK_DDPRINT("input_charlen=%ld, match_charlen=%d, start_charoff=%ld",
	                   (long) input_charlen,
	                   (long) match_charlen,
	                   (long) start_charoff));

	for (i = (duk_int_t) start_charoff; i >= 0; i--) {
		charoff = (duk_uint32_t) i;
		DUK_DDD(DUK_DDDPRINT("wtf8 find, charoff=%ld", (long) charoff));
		if (charoff + match_charlen <= input_charlen) {
			duk_hstring *h_tmp;

			h_tmp = duk_push_wtf8_substring_hstring(thr, h_input, charoff, charoff + match_charlen);
			DUK_DDD(DUK_DDDPRINT("substring=%!O, match=%!O", h_tmp, h_match));

			/* Rely on string interning! */
			if (h_tmp == h_match) {
				duk_pop_unsafe(thr);
				return (duk_int_t) charoff;
			}
			duk_pop_unsafe(thr);
		}
	}
	return -1;
}

DUK_INTERNAL duk_int_t duk_unicode_wtf8_search_backwards(duk_hthread *thr,
                                                         duk_hstring *h_input,
                                                         duk_hstring *h_match,
                                                         duk_uint32_t start_charoff) {
	return duk__unicode_wtf8_search_backwards_reference(thr, h_input, h_match, start_charoff);
}

/* Convert a valid WTF-8 string to CESU-8 representation.  This allows some
 * string algorithms to be implemented in a quick-and-dirty fashion before a
 * proper WTF-8 conversion.
 */
DUK_INTERNAL void duk_unicode_wtf8_to_cesu8(duk_hthread *thr, const duk_uint8_t *data, duk_size_t blen) {
	const duk_uint8_t *p;
	const duk_uint8_t *p_end;
	duk_uint8_t *buf;
	duk_uint8_t *q;
	duk_size_t nonbmp_count = 0;
	duk_size_t alloc_size = 0;

	DUK_ASSERT(data != NULL || blen == 0);
	DUK_ASSERT(duk_unicode_is_valid_wtf8(data, blen));

	/* To figure out the final size, do a first pass to detect non-BMP
	 * encodings.  Because we assume valid WTF-8 input, we can just
	 * look for bytes 0xf0-0xf4 (or just 0xf0-0xff for faster checking).
	 * We don't need to even parse the codepoints as continuation bytes
	 * won't match that range.
	 */
	p = data;
	p_end = data + blen;
	while (p != p_end) {
		duk_uint8_t t;

		t = *p++;
		if (DUK_UNLIKELY(t >= 0xf0U)) {
			nonbmp_count++;
		}
	}

	/* Each non-BMP codepoint is 4 bytes in WTF-8 and 3+3 = 6 bytes
	 * in CESU-8 (so +2 * nonbmp_count).
	 */
	alloc_size = blen + 2U * nonbmp_count;
	buf = (duk_uint8_t *) duk_push_fixed_buffer_nozero(thr, alloc_size);
	DUK_ASSERT(buf != NULL || alloc_size == 0U);

	/* When converting we can just copy bytes over until we encounter
	 * 0xf0-0xf4 (or just 0xf0-0xff for faster checking).  At that point
	 * we need to convert the sequence into a surrogate pair.
	 */
	p = data;
	q = buf;
	while (p != p_end) {
		duk_uint8_t t;

		t = *p++;
		if (DUK_UNLIKELY(t >= 0xf0U)) {
			duk_ucodepoint_t cp;
			duk_ucodepoint_t hi, lo;

			DUK_ASSERT(p_end - p >= 3U); /* Valid WTF-8. */
			cp = ((t & 0x07U) << 18U) + ((p[0] & 0x3fU) << 12U) + ((p[1] & 0x3fU) << 6U) + (p[2] & 0x3fU);
			cp -= 0x10000UL;
			hi = 0xd800UL + (cp >> 10U);
			lo = 0xdc00UL + (cp & 0x3ffU);

			DUK_ASSERT(0xe0U + (hi >> 12U) == 0xedU);
			*q++ = 0xedU;
			*q++ = 0x80U + ((hi >> 6) & 0x3fU);
			*q++ = 0x80U + (hi & 0x3fU);
			DUK_ASSERT(0xe0U + (lo >> 12U) == 0xedU);
			*q++ = 0xedU;
			*q++ = 0x80U + ((lo >> 6) & 0x3fU);
			*q++ = 0x80U + (lo & 0x3fU);
		} else {
			*q++ = t;
		}

		DUK_ASSERT(q <= buf + alloc_size);
	}

	DUK_ASSERT(q == buf + alloc_size);

	/* [ ... cesu8_buf ] */
}

DUK_INTERNAL duk_ucodepoint_t duk_unicode_wtf8_decode_known(const duk_uint8_t *p) {
	duk_uint8_t t;
	duk_ucodepoint_t cp;

	t = *p;
	if (DUK_LIKELY(t <= 0x7fU)) {
		return t;
	}

	DUK_ASSERT(t >= 0x80U);
	DUK_ASSERT(!(t <= 0xbfU)); /* Continuation byte, assume valid WTF-8. */
	DUK_ASSERT(t != 0xc0U);
	DUK_ASSERT(t != 0xc1U);
	DUK_ASSERT(t <= 0xf4U);

	/* High bit patterns here:
	 * 10xxxxxx  Continuation byte (cannot happen for valid WTF-8)
	 * 110xxxxx  2-byte codepoint
	 * 1110xxxx  3-byte codepoint, may contain unpaired surrogates (but not paired)
	 * 11110xxx  4-byte codepoint, always non-BMP (U+10000 or higher), counts as two ES chars
	 */
	if (t <= 0xdfU) {
		DUK_ASSERT(t >= 0xc0U && t <= 0xdfU);
		cp = (t & 0x1fU);
		cp = (cp << 6) + (p[1] & 0x3fU);
		return cp;
	} else if (t <= 0xefU) {
		DUK_ASSERT(t >= 0xe0U && t <= 0xefU);
		cp = (t & 0x0fU);
		cp = (cp << 12) + ((p[1] & 0x3fU) << 6) + (p[2] & 0x3fU);
		return cp;
	} else {
		DUK_ASSERT(t >= 0xf0U && t <= 0xf4U);
		cp = (t & 0x07U);
		cp = (cp << 18) + ((p[1] & 0x3fU) << 12) + ((p[2] & 0x3fU) << 6) + (p[3] & 0x3fU);
		return cp;
	}
}

DUK_INTERNAL duk_ucodepoint_t duk_unicode_wtf8_charcodeat_helper(duk_hthread *thr,
                                                                 duk_hstring *h,
                                                                 duk_uint_t pos,
                                                                 duk_bool_t surrogate_aware) {
	duk_uint32_t byteoff;
	duk_uint32_t charoff;
	duk_ucodepoint_t cp;

	/* Caller must check character offset to be inside the string. */
	DUK_ASSERT(thr != NULL);
	DUK_ASSERT(h != NULL);
	DUK_ASSERT_DISABLE(pos >= 0); /* unsigned */
	DUK_ASSERT(pos < (duk_uint_t) duk_hstring_get_charlen(h));
	DUK_HSTRING_ASSERT_VALID(h);

	if (duk_hstring_get_charlen(h) == duk_hstring_get_bytelen(h)) {
		const duk_uint8_t *p = duk_hstring_get_data(h);
		return (duk_ucodepoint_t) p[pos];
	}

	duk_strcache_scan_char2byte_wtf8(thr, h, pos, &byteoff, &charoff);
	cp = duk_unicode_wtf8_decode_known(duk_hstring_get_data(h) + byteoff);

	if (DUK_LIKELY(cp < 0x10000UL)) {
		DUK_ASSERT(charoff == pos);
		return cp;
	}

	DUK_ASSERT(charoff == pos || charoff + 1 == pos);
	if (charoff == pos) {
		if (surrogate_aware) {
			return cp;
		} else {
			/* High surrogate. */
			duk_ucodepoint_t hi = 0xd800UL + ((cp - 0x10000UL) >> 10);
			return hi;
		}
	} else {
		/* Low surrogate. */
		duk_ucodepoint_t lo = 0xdc00UL + ((cp - 0x10000UL) & 0x3ffUL);
		return lo;
	}
}
