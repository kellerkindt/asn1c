/*-
 * Copyright (c) 2004 Lev Walkin <vlm@lionet.info>. All rights reserved.
 * Redistribution and modifications are permitted subject to BSD license.
 */
#ifndef	ASN_TYPE_REAL_H
#define	ASN_TYPE_REAL_H

#include <constr_TYPE.h>

typedef struct REAL {
	uint8_t *buf;	/* Buffer with REAL type encoding */
	int size;	/* Size of the buffer */
} REAL_t;

extern asn1_TYPE_descriptor_t asn1_DEF_REAL;

asn_struct_print_f REAL_print;

/***********************************
 * Some handy conversion routines. *
 ***********************************/

/*
 * Convert between native double type and REAL representation (DER).
 * RETURN VALUES:
 *  0: Value converted successfully
 * -1: An error occured while converting the value: invalid format.
 */
int asn1_REAL2double(const REAL_t *real_ptr, double *d);
int asn1_double2REAL(REAL_t *real_ptr, double d);

#endif	/* ASN_TYPE_REAL_H */
