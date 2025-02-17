/*
 *  Misc duk_hstring support functions.
 */

#include "duk_internal.h"

/*
 *  Simple getters and setters.
 */

DUK_INTERNAL duk_bool_t duk_hstring_is_ascii(duk_hstring *h) {
#if 0
	/* Slightly smaller code without explicit flag, but explicit flag
	 * is very useful when 'clen' is dropped.
	 */
	return duk_hstring_get_bytelen(h) == duk_hstring_get_charlen(h);
#endif
	return DUK_HSTRING_HAS_ASCII(h); /* lazily set! */
}

DUK_INTERNAL duk_bool_t duk_hstring_is_empty(duk_hstring *h) {
	return duk_hstring_get_bytelen(h) == 0U;
}

DUK_INTERNAL duk_uint32_t duk_hstring_get_hash(duk_hstring *h) {
#if defined(DUK_USE_STRHASH16)
	return h->hdr.h_flags >> 16U;
#else
	return h->hash;
#endif
}

DUK_INTERNAL void duk_hstring_set_hash(duk_hstring *h, duk_uint32_t hash) {
#if defined(DUK_USE_STRHASH16)
	DUK_ASSERT(hash <= 0xffffUL);
	h->hdr.h_flags = (h->hdr.h_flags & 0x0000ffffUL) | (hash << 16U);
#else
	h->hash = hash;
#endif
}

#if defined(DUK_USE_HSTRING_EXTDATA)
DUK_INTERNAL const duk_uint8_t *duk_hstring_get_extdata(duk_hstring *h) {
	DUK_ASSERT(DUK_HSTRING_HAS_EXTDATA(h));
	return h->extdata;
}
#endif

DUK_INTERNAL const duk_uint8_t *duk_hstring_get_data(duk_hstring *h) {
#if defined(DUK_USE_HSTRING_EXTDATA)
	if (DUK_HSTRING_HAS_EXTDATA(h)) {
		return duk_hstring_get_extdata(h);
	} else {
		return (const duk_uint8_t *) (x + 1);
	}
#else
	return (const duk_uint8_t *) (h + 1);
#endif
}

DUK_INTERNAL const duk_uint8_t *duk_hstring_get_data_and_bytelen(duk_hstring *h, duk_size_t *out_blen) {
	DUK_ASSERT(out_blen != NULL);
	*out_blen = duk_hstring_get_bytelen(h);
	return duk_hstring_get_data(h);
}

DUK_INTERNAL const duk_uint8_t *duk_hstring_get_data_end(duk_hstring *h) {
	return duk_hstring_get_data(h) + duk_hstring_get_bytelen(h);
}

/*
 *  duk_hstring charlen, when lazy charlen disabled.
 */

#if !defined(DUK_USE_HSTRING_LAZY_CLEN)
#if !defined(DUK_USE_HSTRING_CLEN)
#error non-lazy duk_hstring charlen but DUK_USE_HSTRING_CLEN not set
#endif
DUK_INTERNAL void duk_hstring_init_charlen(duk_hstring *h) {
	duk_uint32_t clen;

	DUK_ASSERT(h != NULL);
	DUK_ASSERT(!DUK_HSTRING_HAS_ASCII(h));
	DUK_ASSERT(!DUK_HEAPHDR_HAS_READONLY((duk_heaphdr *) h));

	if (DUK_HSTRING_HAS_SYMBOL(h)) {
		clen = 0;
	} else {
		clen = (duk_uint32_t) duk_unicode_wtf8_charlength(duk_hstring_get_data(h), duk_hstring_get_bytelen(h));
	}
#if defined(DUK_USE_STRLEN16)
	DUK_ASSERT(clen <= 0xffffUL); /* Bytelength checked during interning. */
	h->clen16 = (duk_uint16_t) clen;
#else
	h->clen = (duk_uint32_t) clen;
#endif
	if (DUK_LIKELY(clen == duk_hstring_get_bytelen(h))) {
		DUK_HSTRING_SET_ASCII(h);
	}
}

DUK_INTERNAL DUK_HOT duk_size_t duk_hstring_get_charlen(duk_hstring *h) {
#if defined(DUK_USE_STRLEN16)
	return h->clen16;
#else
	return h->clen;
#endif
}
#endif /* !DUK_USE_HSTRING_LAZY_CLEN */

/*
 *  duk_hstring charlen, when lazy charlen enabled.
 */

#if defined(DUK_USE_HSTRING_LAZY_CLEN)
#if defined(DUK_USE_HSTRING_CLEN)
DUK_LOCAL DUK_COLD duk_size_t duk__hstring_get_charlen_slowpath(duk_hstring *h) {
	duk_size_t res;

	DUK_ASSERT(h->clen == 0); /* Checked by caller. */

#if defined(DUK_USE_ROM_STRINGS)
	/* ROM strings have precomputed clen, but if the computed clen is zero
	 * we can still come here and can't write anything.
	 */
	if (DUK_HEAPHDR_HAS_READONLY((duk_heaphdr *) h)) {
		return 0;
	}
#endif

	if (DUK_HSTRING_HAS_SYMBOL(h)) {
		return 0;
	}
	res = duk_unicode_wtf8_charlength(duk_hstring_get_data(h), duk_hstring_get_bytelen(h));
#if defined(DUK_USE_STRLEN16)
	DUK_ASSERT(res <= 0xffffUL); /* Bytelength checked during interning. */
	h->clen16 = (duk_uint16_t) res;
#else
	h->clen = (duk_uint32_t) res;
#endif
	if (DUK_LIKELY(res == duk_hstring_get_bytelen(h))) {
		DUK_HSTRING_SET_ASCII(h);
	}
	return res;
}
#else /* DUK_USE_HSTRING_CLEN */
DUK_LOCAL duk_size_t duk__hstring_get_charlen_slowpath(duk_hstring *h) {
	if (DUK_LIKELY(DUK_HSTRING_HAS_ASCII(h))) {
		/* Most practical strings will go here. */
		return duk_hstring_get_bytelen(h);
	} else {
		/* ASCII flag is lazy, so set it here. */
		duk_size_t res;

		/* XXX: here we could use the strcache to speed up the
		 * computation (matters for 'i < str.length' loops).
		 */

		if (DUK_HSTRING_HAS_SYMBOL(h)) {
			return 0;
		}
		res = duk_unicode_wtf8_charlength(duk_hstring_get_data(h), duk_hstring_get_bytelen(h));

#if defined(DUK_USE_ROM_STRINGS)
		if (DUK_HEAPHDR_HAS_READONLY((duk_heaphdr *) h)) {
			/* For ROM strings, can't write anything; ASCII flag
			 * is preset so we don't need to update it.
			 */
			return res;
		}
#endif
		if (DUK_LIKELY(res == duk_hstring_get_bytelen(h))) {
			DUK_HSTRING_SET_ASCII(h);
		}
		return res;
	}
}
#endif /* DUK_USE_HSTRING_CLEN */

#if defined(DUK_USE_HSTRING_CLEN)
DUK_INTERNAL DUK_HOT duk_size_t duk_hstring_get_charlen(duk_hstring *h) {
#if defined(DUK_USE_STRLEN16)
	if (DUK_LIKELY(h->clen16 != 0)) {
		return h->clen16;
	}
#else
	if (DUK_LIKELY(h->clen != 0)) {
		return h->clen;
	}
#endif
	return duk__hstring_get_charlen_slowpath(h);
}
#else /* DUK_USE_HSTRING_CLEN */
DUK_INTERNAL DUK_HOT duk_size_t duk_hstring_get_charlen(duk_hstring *h) {
	/* Always use slow path. */
	return duk__hstring_get_charlen_slowpath(h);
}
#endif /* DUK_USE_HSTRING_CLEN */
#endif /* DUK_USE_HSTRING_LAZY_CLEN */

/*
 *  duk_hstring charCodeAt, with and without surrogate awareness.
 */

DUK_INTERNAL duk_ucodepoint_t duk_hstring_char_code_at_raw(duk_hthread *thr,
                                                           duk_hstring *h,
                                                           duk_uint_t pos,
                                                           duk_bool_t surrogate_aware) {
	return duk_unicode_wtf8_charcodeat_helper(thr, h, pos, surrogate_aware);
}

/*
 *  Bytelen.
 */

DUK_INTERNAL DUK_HOT duk_size_t duk_hstring_get_bytelen(duk_hstring *h) {
#if defined(DUK_USE_STRLEN16)
	return h->hdr.h_strextra16;
#else
	return h->blen;
#endif
}

DUK_INTERNAL void duk_hstring_set_bytelen(duk_hstring *h, duk_size_t len) {
#if defined(DUK_USE_STRLEN16)
	DUK_ASSERT(len <= 0xffffUL);
	h->hdr.h_strextra16 = len;
#else
	DUK_ASSERT(len <= 0xffffffffUL);
	h->blen = len;
#endif
}

/*
 *  Arridx.
 */

DUK_INTERNAL duk_uarridx_t duk_hstring_get_arridx_fast(duk_hstring *h) {
#if defined(DUK_USE_HSTRING_ARRIDX)
	return h->arridx;
#else
	/* Get array index related to string (or return DUK_HSTRING_NO_ARRAY_INDEX);
	 * avoids helper call if string has no array index value.
	 */
	if (DUK_HSTRING_HAS_ARRIDX(h)) {
		return duk_js_to_arrayindex_hstring_fast_known(h);
	} else {
		return DUK_HSTRING_NO_ARRAY_INDEX;
	}
#endif
}

DUK_INTERNAL duk_uarridx_t duk_hstring_get_arridx_fast_known(duk_hstring *h) {
	DUK_ASSERT(DUK_HSTRING_HAS_ARRIDX(h));

#if defined(DUK_USE_HSTRING_ARRIDX)
	return h->arridx;
#else
	return duk_js_to_arrayindex_hstring_fast_known(h);
#endif
}

DUK_INTERNAL duk_uarridx_t duk_hstring_get_arridx_slow(duk_hstring *h) {
#if defined(DUK_USE_HSTRING_ARRIDX)
	return h->arridx;
#else
	return duk_js_to_arrayindex_hstring_fast(h);
#endif
}

/*
 *  Compare duk_hstring to an ASCII cstring.
 */

DUK_INTERNAL duk_bool_t duk_hstring_equals_ascii_cstring(duk_hstring *h, const char *cstr) {
	duk_size_t len;

	DUK_ASSERT(h != NULL);
	DUK_ASSERT(cstr != NULL);

	len = DUK_STRLEN(cstr);
	if (len != duk_hstring_get_bytelen(h)) {
		return 0;
	}
	if (duk_memcmp((const void *) cstr, (const void *) duk_hstring_get_data(h), len) == 0) {
		return 1;
	}
	return 0;
}
