/*-
 * Copyright (c) 2003, 2004 Lev Walkin <vlm@lionet.info>. All rights reserved.
 * Redistribution and modifications are permitted subject to BSD license.
 */
#include <asn_internal.h>
#include <constr_SET.h>
#include <assert.h>	/* for assert() */

#ifndef	WIN32
#include <netinet/in.h>	/* for ntohl() */
#else
#include <winsock2.h>	/* for ntohl() */
#endif

/*
 * Number of bytes left for this structure.
 * (ctx->left) indicates the number of bytes _transferred_ for the structure.
 * (size) contains the number of bytes in the buffer passed.
 */
#define	LEFT	((size<(size_t)ctx->left)?size:(size_t)ctx->left)

/*
 * If the subprocessor function returns with an indication that it wants
 * more data, it may well be a fatal decoding problem, because the
 * size is constrained by the <TLV>'s L, even if the buffer size allows
 * reading more data.
 * For example, consider the buffer containing the following TLVs:
 * <T:5><L:1><V> <T:6>...
 * The TLV length clearly indicates that one byte is expected in V, but
 * if the V processor returns with "want more data" even if the buffer
 * contains way more data than the V processor have seen.
 */
#define	SIZE_VIOLATION	(ctx->left >= 0 && (size_t)ctx->left <= size)

/*
 * This macro "eats" the part of the buffer which is definitely "consumed",
 * i.e. was correctly converted into local representation or rightfully skipped.
 */
#undef	ADVANCE
#define	ADVANCE(num_bytes)	do {		\
		size_t num = num_bytes;		\
		ptr = ((char *)ptr) + num;	\
		size -= num;			\
		if(ctx->left >= 0)		\
			ctx->left -= num;	\
		consumed_myself += num;		\
	} while(0)

/*
 * Switch to the next phase of parsing.
 */
#undef	NEXT_PHASE
#define	NEXT_PHASE(ctx)	do {			\
		ctx->phase++;			\
		ctx->step = 0;			\
	} while(0)

/*
 * Return a standardized complex structure.
 */
#undef	RETURN
#define	RETURN(_code)	do {			\
		rval.code = _code;		\
		rval.consumed = consumed_myself;\
		return rval;			\
	} while(0)

/*
 * Tags are canonically sorted in the tag2element map.
 */
static int
_t2e_cmp(const void *ap, const void *bp) {
	const asn_TYPE_tag2member_t *a = (const asn_TYPE_tag2member_t *)ap;
	const asn_TYPE_tag2member_t *b = (const asn_TYPE_tag2member_t *)bp;

	int a_class = BER_TAG_CLASS(a->el_tag);
	int b_class = BER_TAG_CLASS(b->el_tag);

	if(a_class == b_class) {
		ber_tlv_tag_t a_value = BER_TAG_VALUE(a->el_tag);
		ber_tlv_tag_t b_value = BER_TAG_VALUE(b->el_tag);

		if(a_value == b_value)
			return 0;
		else if(a_value < b_value)
			return -1;
		else
			return 1;
	} else if(a_class < b_class) {
		return -1;
	} else {
		return 1;
	}
}

/*
 * The decoder of the SET type.
 */
ber_dec_rval_t
SET_decode_ber(asn_codec_ctx_t *opt_codec_ctx, asn_TYPE_descriptor_t *td,
	void **struct_ptr, void *ptr, size_t size, int tag_mode) {
	/*
	 * Bring closer parts of structure description.
	 */
	asn_SET_specifics_t *specs = (asn_SET_specifics_t *)td->specifics;
	asn_TYPE_member_t *elements = td->elements;

	/*
	 * Parts of the structure being constructed.
	 */
	void *st = *struct_ptr;	/* Target structure. */
	asn_struct_ctx_t *ctx;	/* Decoder context */

	ber_tlv_tag_t tlv_tag;	/* T from TLV */
	ber_dec_rval_t rval;	/* Return code from subparsers */

	ssize_t consumed_myself = 0;	/* Consumed bytes from ptr */
	int edx;			/* SET element's index */

	ASN_DEBUG("Decoding %s as SET", td->name);
	
	/*
	 * Create the target structure if it is not present already.
	 */
	if(st == 0) {
		st = *struct_ptr = CALLOC(1, specs->struct_size);
		if(st == 0) {
			RETURN(RC_FAIL);
		}
	}

	/*
	 * Restore parsing context.
	 */
	ctx = (asn_struct_ctx_t *)((char *)st + specs->ctx_offset);
	
	/*
	 * Start to parse where left previously
	 */
	switch(ctx->phase) {
	case 0:
		/*
		 * PHASE 0.
		 * Check that the set of tags associated with given structure
		 * perfectly fits our expectations.
		 */

		rval = ber_check_tags(opt_codec_ctx, td, ctx, ptr, size,
			tag_mode, 1, &ctx->left, 0);
		if(rval.code != RC_OK) {
			ASN_DEBUG("%s tagging check failed: %d",
				td->name, rval.code);
			consumed_myself += rval.consumed;
			RETURN(rval.code);
		}

		if(ctx->left >= 0)
			ctx->left += rval.consumed; /* ?Substracted below! */
		ADVANCE(rval.consumed);

		NEXT_PHASE(ctx);

		ASN_DEBUG("Structure advertised %ld bytes, "
			"buffer contains %ld", (long)ctx->left, (long)size);

		/* Fall through */
	case 1:
		/*
		 * PHASE 1.
		 * From the place where we've left it previously,
		 * try to decode the next member from the list of
		 * this structure's elements.
		 * (ctx->step) stores the member being processed
		 * between invocations and the microphase {0,1} of parsing
		 * that member:
		 * 	step = (2 * <member_number> + <microphase>).
		 * Note, however, that the elements in BER may arrive out of
		 * order, yet DER mandates that they shall arive in the
		 * canonical order of their tags. So, there is a room
		 * for optimization.
		 */
	  for(edx = (ctx->step >> 1); edx < td->elements_count;
			ctx->step = (ctx->step & ~1) + 2,
				edx = (ctx->step >> 1)) {
		void *memb_ptr;		/* Pointer to the member */
		void **memb_ptr2;	/* Pointer to that pointer */
		ssize_t tag_len;	/* Length of TLV's T */

		if(ctx->step & 1)
			goto microphase2;

		/*
		 * MICROPHASE 1: Synchronize decoding.
		 */

		if(ctx->left == 0)
			/*
			 * No more things to decode.
			 * Exit out of here and check whether all mandatory
			 * elements have been received (in the next phase).
			 */
			break;

		/*
		 * Fetch the T from TLV.
		 */
		tag_len = ber_fetch_tag(ptr, LEFT, &tlv_tag);
		switch(tag_len) {
		case 0: if(!SIZE_VIOLATION) RETURN(RC_WMORE);
			/* Fall through */
		case -1: RETURN(RC_FAIL);
		}

		if(ctx->left < 0 && ((uint8_t *)ptr)[0] == 0) {
			if(LEFT < 2) {
				if(SIZE_VIOLATION)
					RETURN(RC_FAIL);
				else
					RETURN(RC_WMORE);
			} else if(((uint8_t *)ptr)[1] == 0) {
				/*
				 * Found the terminator of the
				 * indefinite length structure.
				 * Invoke the generic finalization function.
				 */
				goto phase3;
			}
		}

		if(BER_TAGS_EQUAL(tlv_tag, elements[edx].tag)) {
			/*
			 * The elements seem to go in order.
			 * This is not particularly strange,
			 * but is not strongly anticipated either.
			 */
		} else {
			asn_TYPE_tag2member_t *t2m;
			asn_TYPE_tag2member_t key;

			key.el_tag = tlv_tag;
			(void *)t2m = bsearch(&key,
					specs->tag2el, specs->tag2el_count,
					sizeof(specs->tag2el[0]), _t2e_cmp);
			if(t2m) {
				/*
				 * Found the element corresponding to the tag.
				 */
				edx = t2m->el_no;
				ctx->step = 2 * edx;
			} else if(specs->extensible == 0) {
				ASN_DEBUG("Unexpected tag %s "
					"in non-extensible SET %s",
					ber_tlv_tag_string(tlv_tag), td->name);
				RETURN(RC_FAIL);
			} else {
				/* Skip this tag */
				ssize_t skip;

				ASN_DEBUG("Skipping unknown tag %s",
					ber_tlv_tag_string(tlv_tag));

				skip = ber_skip_length(opt_codec_ctx,
					BER_TLV_CONSTRUCTED(ptr),
					(char *)ptr + tag_len, LEFT - tag_len);

				switch(skip) {
				case 0: if(!SIZE_VIOLATION) RETURN(RC_WMORE);
					/* Fall through */
				case -1: RETURN(RC_FAIL);
				}

				ADVANCE(skip + tag_len);
				ctx->step -= 2;
				edx--;
				continue;  /* Try again with the next tag */
			}
		}

		/*
		 * MICROPHASE 2: Invoke the member-specific decoder.
		 */
		ctx->step |= 1;	/* Confirm entering next microphase */
	microphase2:

		/*
		 * Check for duplications: must not overwrite
		 * already decoded elements.
		 */
		if(ASN_SET_ISPRESENT2((char *)st + specs->pres_offset, edx)) {
			ASN_DEBUG("SET %s: Duplicate element %s (%d)",
				td->name, elements[edx].name, edx);
			RETURN(RC_FAIL);
		}
		
		/*
		 * Compute the position of the member inside a structure,
		 * and also a type of containment (it may be contained
		 * as pointer or using inline inclusion).
		 */
		if(elements[edx].flags & ATF_POINTER) {
			/* Member is a pointer to another structure */
			memb_ptr2 = (void **)((char *)st + elements[edx].memb_offset);
		} else {
			/*
			 * A pointer to a pointer
			 * holding the start of the structure
			 */
			memb_ptr = (char *)st + elements[edx].memb_offset;
			memb_ptr2 = &memb_ptr;
		}
		/*
		 * Invoke the member fetch routine according to member's type
		 */
		rval = elements[edx].type->ber_decoder(opt_codec_ctx,
				elements[edx].type,
				memb_ptr2, ptr, LEFT,
				elements[edx].tag_mode);
		switch(rval.code) {
		case RC_OK:
			ASN_SET_MKPRESENT((char *)st + specs->pres_offset, edx);
			break;
		case RC_WMORE: /* More data expected */
			if(!SIZE_VIOLATION) {
				ADVANCE(rval.consumed);
				RETURN(RC_WMORE);
			}
			/* Fail through */
		case RC_FAIL: /* Fatal error */
			RETURN(RC_FAIL);
		} /* switch(rval) */
		
		ADVANCE(rval.consumed);
	  }	/* for(all structure members) */

	phase3:
		ctx->phase = 3;
		/* Fall through */
	case 3:
	case 4:	/* Only 00 is expected */
		ASN_DEBUG("SET %s Leftover: %ld, size = %ld",
			td->name, (long)ctx->left, (long)size);

		/*
		 * Skip everything until the end of the SET.
		 */
		while(ctx->left) {
			ssize_t tl, ll;

			tl = ber_fetch_tag(ptr, LEFT, &tlv_tag);
			switch(tl) {
			case 0: if(!SIZE_VIOLATION) RETURN(RC_WMORE);
				/* Fall through */
			case -1: RETURN(RC_FAIL);
			}

			/*
			 * If expected <0><0>...
			 */
			if(ctx->left < 0
				&& ((uint8_t *)ptr)[0] == 0) {
				if(LEFT < 2) {
					if(SIZE_VIOLATION)
						RETURN(RC_FAIL);
					else
						RETURN(RC_WMORE);
				} else if(((uint8_t *)ptr)[1] == 0) {
					/*
					 * Correctly finished with <0><0>.
					 */
					ADVANCE(2);
					ctx->left++;
					ctx->phase = 4;
					continue;
				}
			}

			if(specs->extensible == 0 || ctx->phase == 4) {
				ASN_DEBUG("Unexpected continuation "
					"of a non-extensible type %s",
					td->name);
				RETURN(RC_FAIL);
			}

			ll = ber_skip_length(opt_codec_ctx,
				BER_TLV_CONSTRUCTED(ptr),
				(char *)ptr + tl, LEFT - tl);
			switch(ll) {
			case 0: if(!SIZE_VIOLATION) RETURN(RC_WMORE);
				/* Fall through */
			case -1: RETURN(RC_FAIL);
			}

			ADVANCE(tl + ll);
		}

		ctx->phase = 5;
	case 5:
		/*
		 * Check that all mandatory elements are present.
		 */
		for(edx = 0; edx < td->elements_count;
			edx += (8 * sizeof(specs->_mandatory_elements[0]))) {
			unsigned int midx, pres, must;

			midx = edx/(8 * sizeof(specs->_mandatory_elements[0]));
			pres = ((unsigned int *)((char *)st+specs->pres_offset))[midx];
			must = ntohl(specs->_mandatory_elements[midx]);

			if((pres & must) == must) {
				/*
				 * Yes, everything seems to be in place.
				 */
			} else {
				ASN_DEBUG("One or more mandatory elements "
					"of a SET %s %d (%08x.%08x)=%08x "
					"are not present",
					td->name,
					midx,
					pres,
					must,
					(~(pres & must) & must)
				);
				RETURN(RC_FAIL);
			}
		}

		NEXT_PHASE(ctx);
	}
	
	RETURN(RC_OK);
}

/*
 * The DER encoder of the SET type.
 */
asn_enc_rval_t
SET_encode_der(asn_TYPE_descriptor_t *td,
	void *sptr, int tag_mode, ber_tlv_tag_t tag,
	asn_app_consume_bytes_f *cb, void *app_key) {
	asn_SET_specifics_t *specs = (asn_SET_specifics_t *)td->specifics;
	size_t computed_size = 0;
	asn_enc_rval_t er;
	int t2m_build_own = (specs->tag2el_count != td->elements_count);
	asn_TYPE_tag2member_t *t2m;
	int t2m_count;
	ssize_t ret;
	int edx;

	/*
	 * Use existing, or build our own tags map.
	 */
	if(t2m_build_own) {
		(void *)t2m = alloca(td->elements_count * sizeof(t2m[0]));
		if(!t2m) _ASN_ENCODE_FAILED; /* There are such platforms */
		t2m_count = 0;
	} else {
		/*
		 * There is no untagged CHOICE in this SET.
		 * Employ existing table.
		 */
		t2m = specs->tag2el;
		t2m_count = specs->tag2el_count;
	}

	/*
	 * Gather the length of the underlying members sequence.
	 */
	for(edx = 0; edx < td->elements_count; edx++) {
		asn_TYPE_member_t *elm = &td->elements[edx];
		asn_enc_rval_t tmper;
		void *memb_ptr;

		/*
		 * Compute the length of the encoding of this member.
		 */
		if(elm->flags & ATF_POINTER) {
			memb_ptr = *(void **)((char *)sptr + elm->memb_offset);
			if(!memb_ptr) {
				if(t2m_build_own) {
					t2m[t2m_count].el_no = edx;
					t2m[t2m_count].el_tag = 0;
					t2m_count++;
				}
				continue;
			}
		} else {
			memb_ptr = (void *)((char *)sptr + elm->memb_offset);
		}
		tmper = elm->type->der_encoder(elm->type, memb_ptr,
			elm->tag_mode, elm->tag,
			0, 0);
		if(tmper.encoded == -1)
			return tmper;
		computed_size += tmper.encoded;

		/*
		 * Remember the outmost tag of this member.
		 */
		if(t2m_build_own) {
			t2m[t2m_count].el_no = edx;
			t2m[t2m_count].el_tag = asn_TYPE_outmost_tag(
				elm->type, memb_ptr, elm->tag_mode, elm->tag);
			t2m_count++;
		} else {
			/*
			 * No dynamic sorting is necessary.
			 */
		}
	}

	/*
	 * Finalize order of the components.
	 */
	assert(t2m_count == td->elements_count);
	if(t2m_build_own) {
		/*
		 * Sort the underlying members according to their
		 * canonical tags order. DER encoding mandates it.
		 */
		qsort(t2m, t2m_count, sizeof(specs->tag2el[0]), _t2e_cmp);
	} else {
		/*
		 * Tags are already sorted by the compiler.
		 */
	}

	/*
	 * Encode the TLV for the sequence itself.
	 */
	ret = der_write_tags(td, computed_size, tag_mode, 1, tag, cb, app_key);
	if(ret == -1) _ASN_ENCODE_FAILED;
	er.encoded = computed_size + ret;

	if(!cb) return er;

	/*
	 * Encode all members.
	 */
	for(edx = 0; edx < td->elements_count; edx++) {
		asn_TYPE_member_t *elm;
		asn_enc_rval_t tmper;
		void *memb_ptr;

		/* Encode according to the tag order */
		elm = &td->elements[t2m[edx].el_no];

		if(elm->flags & ATF_POINTER) {
			memb_ptr = *(void **)((char *)sptr + elm->memb_offset);
			if(!memb_ptr) continue;
		} else {
			memb_ptr = (void *)((char *)sptr + elm->memb_offset);
		}
		tmper = elm->type->der_encoder(elm->type, memb_ptr,
			elm->tag_mode, elm->tag,
			cb, app_key);
		if(tmper.encoded == -1)
			return tmper;
		computed_size -= tmper.encoded;
	}

	if(computed_size != 0) {
		/*
		 * Encoded size is not equal to the computed size.
		 */
		_ASN_ENCODE_FAILED;
	}

	return er;
}

asn_enc_rval_t
SET_encode_xer(asn_TYPE_descriptor_t *td, void *sptr,
	int ilevel, enum xer_encoder_flags_e flags,
		asn_app_consume_bytes_f *cb, void *app_key) {
	asn_SET_specifics_t *specs = (asn_SET_specifics_t *)td->specifics;
	asn_enc_rval_t er;
	int xcan = (flags & XER_F_CANONICAL);
	asn_TYPE_tag2member_t *t2m = specs->tag2el_cxer;
	int t2m_count = specs->tag2el_cxer_count;
	int edx;

	if(!sptr)
		_ASN_ENCODE_FAILED;

	assert(t2m_count == td->elements_count);

	er.encoded = 0;

	for(edx = 0; edx < t2m_count; edx++) {
		asn_enc_rval_t tmper;
		asn_TYPE_member_t *elm;
		void *memb_ptr;
		const char *mname;
		unsigned int mlen;

		elm = &td->elements[t2m[edx].el_no];
		mname = elm->name;
		mlen = strlen(elm->name);

		if(elm->flags & ATF_POINTER) {
			memb_ptr = *(void **)((char *)sptr + elm->memb_offset);
			if(!memb_ptr) continue;	/* OPTIONAL element? */
		} else {
			memb_ptr = (void *)((char *)sptr + elm->memb_offset);
		}

		if(!xcan)
			_i_ASN_TEXT_INDENT(1, ilevel);
		_ASN_CALLBACK3("<", 1, mname, mlen, ">", 1);

		/* Print the member itself */
		tmper = elm->type->xer_encoder(elm->type, memb_ptr,
				ilevel + 1, flags, cb, app_key);
		if(tmper.encoded == -1) return tmper;

		_ASN_CALLBACK3("</", 2, mname, mlen, ">", 1);

		er.encoded += 5 + (2 * mlen) + tmper.encoded;
	}

	if(!xcan) _i_ASN_TEXT_INDENT(1, ilevel - 1);

	return er;
cb_failed:
	_ASN_ENCODE_FAILED;
}

int
SET_print(asn_TYPE_descriptor_t *td, const void *sptr, int ilevel,
		asn_app_consume_bytes_f *cb, void *app_key) {
	int edx;
	int ret;

	if(!sptr) return (cb("<absent>", 8, app_key) < 0) ? -1 : 0;

	/* Dump preamble */
	if(cb(td->name, strlen(td->name), app_key) < 0
	|| cb(" ::= {", 6, app_key) < 0)
		return -1;

	for(edx = 0; edx < td->elements_count; edx++) {
		asn_TYPE_member_t *elm = &td->elements[edx];
		const void *memb_ptr;

		if(elm->flags & ATF_POINTER) {
			memb_ptr = *(const void * const *)((const char *)sptr + elm->memb_offset);
			if(!memb_ptr) continue;
		} else {
			memb_ptr = (const void *)((const char *)sptr + elm->memb_offset);
		}

		_i_INDENT(1);

		/* Print the member's name and stuff */
		if(cb(elm->name, strlen(elm->name), app_key) < 0
		|| cb(": ", 2, app_key) < 0)
			return -1;

		/* Print the member itself */
		ret = elm->type->print_struct(elm->type, memb_ptr, ilevel + 1,
			cb, app_key);
		if(ret) return ret;
	}

	ilevel--;
	_i_INDENT(1);

	return (cb("}", 1, app_key) < 0) ? -1 : 0;
}

void
SET_free(asn_TYPE_descriptor_t *td, void *ptr, int contents_only) {
	int edx;

	if(!td || !ptr)
		return;

	ASN_DEBUG("Freeing %s as SET", td->name);

	for(edx = 0; edx < td->elements_count; edx++) {
		asn_TYPE_member_t *elm = &td->elements[edx];
		void *memb_ptr;
		if(elm->flags & ATF_POINTER) {
			memb_ptr = *(void **)((char *)ptr + elm->memb_offset);
			if(memb_ptr)
				elm->type->free_struct(elm->type, memb_ptr, 0);
		} else {
			memb_ptr = (void *)((char *)ptr + elm->memb_offset);
			elm->type->free_struct(elm->type, memb_ptr, 1);
		}
	}

	if(!contents_only) {
		FREEMEM(ptr);
	}
}

int
SET_constraint(asn_TYPE_descriptor_t *td, const void *sptr,
		asn_app_consume_bytes_f *app_errlog, void *app_key) {
	int edx;

	if(!sptr) {
		_ASN_ERRLOG(app_errlog, app_key,
			"%s: value not given (%s:%d)",
			td->name, __FILE__, __LINE__);
		return -1;
	}

	/*
	 * Iterate over structure members and check their validity.
	 */
	for(edx = 0; edx < td->elements_count; edx++) {
		asn_TYPE_member_t *elm = &td->elements[edx];
		const void *memb_ptr;

		if(elm->flags & ATF_POINTER) {
			memb_ptr = *(const void * const *)((const char *)sptr + elm->memb_offset);
			if(!memb_ptr) {
				if(elm->optional)
					continue;
				_ASN_ERRLOG(app_errlog, app_key,
				"%s: mandatory element %s absent (%s:%d)",
				td->name, elm->name, __FILE__, __LINE__);
				return -1;
			}
		} else {
			memb_ptr = (const void *)((const char *)sptr + elm->memb_offset);
		}

		if(elm->memb_constraints) {
			int ret = elm->memb_constraints(elm->type, memb_ptr,
				app_errlog, app_key);
			if(ret) return ret;
		} else {
			int ret = elm->type->check_constraints(elm->type,
				memb_ptr, app_errlog, app_key);
			if(ret) return ret;
			/*
			 * Cannot inherit it earlier:
			 * need to make sure we get the updated version.
			 */
			elm->memb_constraints = elm->type->check_constraints;
		}
	}

	return 0;
}
