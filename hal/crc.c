/**
 * @file crc.c
 *
 * @brief implement the crc module (this is hardware independent but can call hardware methods if available)
 *
 * @author OT
 *
 * @date March 2013
 *
 */


#include "crc.h"
#include <stdlib.h>
#include <stdio.h>
#define TRACE printf("%s:%i\n", __FUNCTION__, __LINE__);
//#define TRACE 


// crc a 32bit word buffer via the table method
static uint32_t crc_32w_buf_table(struct crc_h *h, void *buf, uint32_t len)
{
	uint32_t words = len / 4;
	uint32_t *_buf = (uint32_t *)buf;
	TRACE;

	cm_ini(&h->cm);

	while (words--)
	{
		int i;
		uint32_t w = *_buf++;
		uint8_t b;
		for (i = 0; i < sizeof(w); i++)
		{
			if (h->cm.cm_refin == FALSE)
			{
				b = (uint8_t)((w & 0xFF000000) >> 24);
				w <<= 8;
			}
			else
			{
				b = (uint8_t)(w & 0x000000FF);
				w >>= 8;
			}
			h->cm.cm_reg = ((uint32_t *)h->table)[((h->cm.cm_reg>>24) ^ b) & 0xFFL] ^ (h->cm.cm_reg << 8);
		}
	}

	return cm_crc(&h->cm);
}


// crc a 8bit word buffer via the table method
ulong reflect(ulong v,int b);
static uint8_t crc_8w_buf_table(struct crc_h *h, void *buf, uint32_t len)
{
	uint8_t *_buf = (uint8_t *)buf;
	uint8_t _crc = (uint8_t)h->cm.cm_init;
	TRACE;

	_crc = (h->cm.cm_refin)?reflect(_crc, 8): _crc;
	while (len--)
		_crc = ((uint8_t *)h->table)[_crc ^ *_buf++];
	_crc = (h->cm.cm_refin)?reflect(_crc, 8): _crc;
	h->cm.cm_reg = _crc;
	return cm_crc(&h->cm);
}


// crc anything using the soft method
static uint32_t crc_buf_soft(struct crc_h *h, void *buf, uint32_t len)
{
	TRACE;
	cm_ini(&h->cm);
	cm_blk(&h->cm, buf, len);
	return cm_crc(&h->cm);
}


// default handler (this should be overridden if possible)
///@todo make this weak
uint32_t crc_buf_hard(struct crc_h *h, void *buf, uint32_t len)
{
	TRACE;
	return 0;
}


// default handler (this should be overridden if possible)
///@todo make this weak
bool crc_init_hard(struct crc_h *h)
{
	TRACE;
	return false;
}


// generate a 32 bit crc table (4bytes*256 = 1KB of RAM needed)
static void gen_table32(struct crc_h *h)
{
	unsigned int i;
	uint32_t *tbl = h->table;
	TRACE;

	for (i=0; i < 256; i++)
		tbl[i] = (uint32_t)cm_tab(&h->cm, i);
}


// generate a 8 bit crc table (1byte*256 = 1/4KB of ram needed)
static void gen_table8(struct crc_h *h)
{
	unsigned int i;
	uint8_t *tbl = h->table;
	TRACE;

	for (i=0; i < 256; i++)
		tbl[i] = (uint8_t)cm_tab(&h->cm, i);
}


static bool crc_init_table(struct crc_h *h)
{
	TRACE;

	// if the caller has not given space for a table then give up
	if (h->table == NULL)
		return false;

	// we only support certain predefined (and tested) table layouts
	if (h->cm.cm_width == 32 && h->table_size >= 256*sizeof(uint32_t))
	{
		h->method = CRC_METHOD_TABLE_32W;
		gen_table32(h);
		return true;
	}
	if (h->cm.cm_width == 8 && h->table_size >= 256*sizeof(uint8_t))
	{
		h->method = CRC_METHOD_TABLE_8W;
		gen_table8(h);
		return true;
	}

	// others are not supported by table method atm so will have to use soft method
	return false;
}


// this is slow but it should work for anything
static bool crc_init_soft(struct crc_h *h)
{
	TRACE;
	h->method = CRC_METHOD_SOFT;
	return true;
}


bool crc_init(struct crc_h *h)
{
	TRACE;
	// try hardware method first as it is fastest
	if (h->method == CRC_METHOD_HARD || 
		h->method == CRC_METHOD_BEST)
		if (crc_init_hard(h))
			return h;

	// try table method next as it is still quite fast
	if (h->method == CRC_METHOD_TABLE_8W || 
		h->method == CRC_METHOD_TABLE_32W || 
		h->method == CRC_METHOD_BEST)
		if(crc_init_table(h))
			return h;

	// only option is full software method (quite slow)
	return crc_init_soft(h);
}


uint32_t crc_buf(struct crc_h *h, void *buf, uint32_t len)
{
	switch (h->method)
	{
		case CRC_METHOD_SOFT:
			return crc_buf_soft(h, buf, len);
		case CRC_METHOD_HARD:
			return crc_buf_hard(h, buf, len);
		case CRC_METHOD_TABLE_8W:
			return crc_8w_buf_table(h, buf, len);
		case CRC_METHOD_TABLE_32W:
			return crc_32w_buf_table(h, buf, len);
		default:
			///@todo error !!
			return -1;
	}
}


